/* Exercise the production queue/lifecycle with real threads and stub codegen. */
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "thread_faults.h"
#include "../../deps/lightrec/recompiler.c"

static atomic_bool hold_start, hold_compile;
static atomic_uint compiled, compile_started, clock_reads, live_allocations, live_cstates;
static atomic_int compile_result;
static atomic_llong clock_us;
static reap_func_t pending_reap;
static void *pending_reap_data;
unsigned cpu_features_get_core_amount(void) { return 4; }
retro_time_t cpu_features_get_time_usec(void)
{
   atomic_fetch_add(&clock_reads, 1);
   return atomic_fetch_add(&clock_us, 100);
}
unsigned beetle_worker_init_affinity(void)
{
   while (atomic_load(&hold_start)) usleep(1000);
   return 4;
}
void *lightrec_malloc(struct lightrec_state *s, enum mem_type t, unsigned len)
{
   void *p = calloc(1, len);
   if (p) atomic_fetch_add(&live_allocations, 1);
   return p;
}
void lightrec_free(struct lightrec_state *s, enum mem_type t, unsigned len, void *p)
{
   assert(atomic_load(&live_allocations));
   atomic_fetch_sub(&live_allocations, 1);
   free(p);
}
struct lightrec_cstate *lightrec_create_cstate(struct lightrec_state *s)
{
   struct lightrec_cstate *c = calloc(1, sizeof(*c));
   assert(c);
   c->state = s;
   atomic_fetch_add(&live_cstates, 1);
   return c;
}
void lightrec_free_cstate(struct lightrec_cstate *c)
{
   atomic_fetch_sub(&live_cstates, 1);
   free(c);
}
int lightrec_compile_block(struct lightrec_cstate *c, struct block *b)
{
   atomic_fetch_add(&compile_started, 1);
   while (atomic_load(&hold_compile)) usleep(1000);
   /* Give idle workers time to inspect a queue containing only claimed work. */
   usleep(20000);
   atomic_fetch_add(&compiled, 1);
   return atomic_load(&compile_result);
}
u32 lightrec_emulate_block(struct lightrec_state *s, struct block *b, u32 pc) { return pc; }
void lightrec_free_opcode_list(struct lightrec_state *s, struct opcode *l) {}
void lightrec_remove_outdated_blocks(struct blockcache *c, const struct block *b) {}
int lightrec_reaper_add(struct reaper *r, reap_func_t f, void *d)
{
   assert(!pending_reap);
   pending_reap = f;
   pending_reap_data = d;
   return 0;
}

static void wait_compiled(unsigned expected)
{
   unsigned tries;
   for (tries = 0; tries < 2000 && atomic_load(&compiled) < expected; tries++)
      usleep(1000);
   assert(atomic_load(&compiled) == expected);
}
static void wait_started(unsigned expected)
{
   unsigned tries;
   for (tries = 0; tries < 2000 && atomic_load(&compile_started) < expected; tries++)
      usleep(1000);
   assert(atomic_load(&compile_started) == expected);
}

static void wait_reap(struct recompiler *rec)
{
   unsigned tries;
   bool ready = false;
   for (tries = 0; tries < 2000 && !ready; tries++) {
      slock_lock(rec->mutex);
      ready = pending_reap != NULL;
      slock_unlock(rec->mutex);
      if (!ready) usleep(1000);
   }
   assert(ready);
}

static void assert_clean(void)
{
   assert(!live_threads && !live_locks && !live_conds);
   assert(!atomic_load(&live_allocations) && !atomic_load(&live_cstates));
}
static void test_profile(void)
{
   struct lightrec_state state = {0};
   struct lightrec_profile profile = {0};
   struct block blocks[3] = {0};
   struct recompiler *rec;
   unsigned i, reads;
   u32 pc = 0x10000;

   atomic_store(&compiled, 0);
   atomic_store(&compile_started, 0);
   atomic_store(&clock_reads, 0);
   atomic_store(&clock_us, 1000);
   for (i = 0; i < 3; i++) {
      blocks[i].pc = (i + 1) * 0x10000;
      blocks[i].nb_ops = 4;
   }
   rec = lightrec_recompiler_init(&state);
   assert(rec);
   state.rec = rec;
   lightrec_recompiler_pause(rec);
   assert(!lightrec_recompiler_add(rec, &blocks[0]));
   lightrec_recompiler_get_profile(rec, &profile, false);
   assert(profile.compiler_workers == 3);
   assert(profile.queue_depth == 1 && !profile.queue_max);
   assert(!profile.compile_requests && !profile.compile_completions);
   assert(!profile.compile_failures && !profile.compile_time_us);
   assert(!atomic_load(&clock_reads));

   lightrec_recompiler_set_profiling(rec, true);
   state.profiling_enabled = true;
   lightrec_recompiler_get_profile(rec, &profile, false);
   assert(profile.queue_depth == 1 && profile.queue_max == 1);
   assert(!profile.compile_requests);
   lightrec_recompiler_remove(rec, &blocks[0]);
   lightrec_recompiler_get_profile(rec, &profile, true);
   assert(!profile.queue_depth && profile.queue_max == 1);

   assert(!lightrec_recompiler_run_first_pass(&state, &blocks[0], &pc));
   assert(state.profile.first_pass_blocks == 1);
   assert(state.profile.interpreted_blocks == 1);
   block_set_flags(&blocks[0], BLOCK_NEVER_COMPILE);
   assert(!lightrec_recompiler_run_first_pass(&state, &blocks[0], &pc));
   assert(state.profile.first_pass_blocks == 1);
   block_clear_flags(&blocks[0], BLOCK_NEVER_COMPILE);
   state.profiling_enabled = false;
   assert(!lightrec_recompiler_run_first_pass(&state, &blocks[0], &pc));
   assert(state.profile.interpreted_blocks == 1);
   state.profiling_enabled = true;

   for (i = 0; i < 3; i++)
      assert(!lightrec_recompiler_add(rec, &blocks[i]));
   assert(!lightrec_recompiler_add(rec, &blocks[0]));
   lightrec_recompiler_get_profile(rec, &profile, false);
   assert(profile.compile_requests == 4);
   assert(profile.queue_depth == 3 && profile.queue_max == 3);
   lightrec_recompiler_remove(rec, &blocks[2]);
   lightrec_recompiler_get_profile(rec, &profile, true);
   assert(profile.compile_requests == 4);
   assert(profile.queue_depth == 2 && profile.queue_max == 3);
   lightrec_recompiler_get_profile(rec, &profile, false);
   assert(!profile.compile_requests);
   assert(profile.queue_depth == 2 && profile.queue_max == 2);

   atomic_store(&hold_compile, true);
   lightrec_recompiler_unpause(rec);
   wait_started(2);
   for (i = 0; i < 20; i++) {
      lightrec_recompiler_get_profile(rec, &profile, true);
      assert(!profile.compile_completions && !profile.compile_time_us);
      assert(profile.queue_depth == 2 && profile.queue_max == 2);
   }
   assert(atomic_load(&clock_reads) == 2);
   atomic_store(&hold_compile, false);
   wait_compiled(2);
   lightrec_recompiler_remove(rec, &blocks[0]);
   lightrec_recompiler_remove(rec, &blocks[1]);
   lightrec_recompiler_get_profile(rec, &profile, true);
   assert(!profile.compile_requests && profile.compile_completions == 2);
   assert(!profile.compile_failures && profile.compile_time_us > 0);
   assert(!profile.queue_depth && profile.queue_max == 2);
   assert(atomic_load(&clock_reads) == 4);

   lightrec_recompiler_set_profiling(rec, false);
   assert(!lightrec_recompiler_add(rec, &blocks[2]));
   wait_compiled(3);
   lightrec_recompiler_remove(rec, &blocks[2]);
   lightrec_recompiler_get_profile(rec, &profile, false);
   assert(!profile.compile_requests && !profile.compile_completions);
   assert(!profile.compile_time_us && !profile.queue_depth);
   assert(atomic_load(&clock_reads) == 4);

   atomic_store(&hold_compile, true);
   assert(!lightrec_recompiler_add(rec, &blocks[0]));
   wait_started(4);
   lightrec_recompiler_set_profiling(rec, true);
   lightrec_recompiler_get_profile(rec, &profile, false);
   assert(profile.queue_depth == 1 && profile.queue_max == 1);
   atomic_store(&hold_compile, false);
   wait_compiled(4);
   lightrec_recompiler_remove(rec, &blocks[0]);
   lightrec_recompiler_get_profile(rec, &profile, true);
   assert(!profile.compile_requests && !profile.compile_completions);
   assert(!profile.compile_time_us && !profile.queue_depth);
   assert(atomic_load(&clock_reads) == 4);

   atomic_store(&hold_compile, true);
   assert(!lightrec_recompiler_add(rec, &blocks[0]));
   wait_started(5);
   reads = atomic_load(&clock_reads);
   lightrec_recompiler_set_profiling(rec, false);
   lightrec_recompiler_set_profiling(rec, true);
   atomic_store(&hold_compile, false);
   wait_compiled(5);
   lightrec_recompiler_remove(rec, &blocks[0]);
   lightrec_recompiler_get_profile(rec, &profile, true);
   assert(profile.compile_requests == 1 && !profile.compile_completions);
   assert(!profile.compile_time_us && !profile.queue_depth);
   assert(atomic_load(&clock_reads) == reads);

   atomic_store(&hold_compile, true);
   assert(!lightrec_recompiler_add(rec, &blocks[1]));
   wait_started(6);
   lightrec_recompiler_set_profiling(rec, true);
   lightrec_recompiler_get_profile(rec, &profile, true);
   assert(profile.compile_requests == 1 && profile.queue_depth == 1);
   atomic_store(&hold_compile, false);
   wait_compiled(6);
   lightrec_recompiler_remove(rec, &blocks[1]);
   lightrec_recompiler_get_profile(rec, &profile, true);
   assert(!profile.compile_requests && profile.compile_completions == 1);
   assert(profile.compile_time_us == 100 && !profile.queue_depth);

   atomic_store(&compile_result, -EIO);
   assert(!lightrec_recompiler_add(rec, &blocks[0]));
   wait_compiled(7);
   lightrec_recompiler_remove(rec, &blocks[0]);
   lightrec_recompiler_get_profile(rec, &profile, true);
   assert(profile.compile_requests == 1 && !profile.compile_completions);
   assert(profile.compile_failures == 1 && profile.compile_time_us == 100);
   assert(!profile.queue_depth);

   atomic_store(&compile_result, -ENOMEM);
   assert(!lightrec_recompiler_add(rec, &blocks[0]));
   wait_compiled(8);
   wait_reap(rec);
   lightrec_recompiler_get_profile(rec, &profile, false);
   assert(profile.compile_requests == 1 && !profile.compile_completions);
   assert(profile.compile_failures == 1 && profile.compile_time_us == 100);
   assert(!profile.queue_depth && profile.queue_max == 1);
   assert(!lightrec_recompiler_add(rec, &blocks[1]));
   lightrec_recompiler_get_profile(rec, &profile, false);
   assert(profile.compile_requests == 2 && !profile.queue_depth);
   pending_reap(&state, pending_reap_data);
   pending_reap = NULL;
   pending_reap_data = NULL;
   assert(state.profile.codecache_reclaims == 1);
   state.profiling_enabled = false;
   lightrec_flush_code_buffer(&state, rec);
   assert(state.profile.codecache_reclaims == 1);
   atomic_store(&compile_result, 0);
   lightrec_free_recompiler(rec);
   assert_clean();
}

int main(void)
{
   struct lightrec_state state = {0};
   struct block blocks[3] = {0};
   struct recompiler *rec;
   unsigned failure;
   /* Two conditions, two mutexes, three workers; fail every allocation. */
   for (failure = 1; failure <= 7; failure++) {
      allocation_index = 0;
      fail_allocation = failure;
      assert(!lightrec_recompiler_init(&state));
      assert_clean();
   }
   fail_allocation = 0;
   atomic_store(&hold_start, true);
   rec = lightrec_recompiler_init(&state);
   assert(rec);
   blocks[0].pc = 0x10000;
   blocks[1].pc = 0x20000;
   blocks[2].pc = 0x30000;
   /* Queue and signal before any worker reaches its first wait. */
   assert(!lightrec_recompiler_add(rec, &blocks[0]));
   atomic_store(&hold_start, false);
   wait_compiled(1);
   lightrec_recompiler_remove(rec, &blocks[0]);
   lightrec_recompiler_pause(rec);
   assert(!lightrec_recompiler_add(rec, &blocks[1]));
   usleep(30000);
   assert(atomic_load(&compiled) == 1);
   lightrec_recompiler_unpause(rec);
   wait_compiled(2);
   lightrec_recompiler_remove(rec, &blocks[1]);
   assert(!lightrec_recompiler_add(rec, &blocks[2]));
   /* Teardown must safely cancel or join outstanding work. */
   lightrec_free_recompiler(rec);
   assert_clean();
   assert(!atomic_load(&clock_reads));
   test_profile();
   puts("Lightrec startup, claimed queue, pause/resume, allocation failures and profiling: PASS");
   return 0;
}
