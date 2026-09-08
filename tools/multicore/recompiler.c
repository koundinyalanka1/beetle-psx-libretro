/* Exercise the production queue/lifecycle with real threads and stub codegen. */
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "thread_faults.h"
#include "../../deps/lightrec/recompiler.c"

static atomic_bool hold_start;
static atomic_uint compiled, live_allocations, live_cstates;
unsigned cpu_features_get_core_amount(void) { return 4; }
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
   /* Give idle workers time to inspect a queue containing only claimed work. */
   usleep(20000);
   atomic_fetch_add(&compiled, 1);
   return 0;
}
u32 lightrec_emulate_block(struct lightrec_state *s, struct block *b, u32 pc) { return pc; }
void lightrec_free_opcode_list(struct lightrec_state *s, struct opcode *l) {}
void lightrec_remove_outdated_blocks(struct blockcache *c, const struct block *b) {}
int lightrec_reaper_add(struct reaper *r, reap_func_t f, void *d) { abort(); }

static void wait_compiled(unsigned expected)
{
   unsigned tries;
   for (tries = 0; tries < 2000 && atomic_load(&compiled) < expected; tries++)
      usleep(1000);
   assert(atomic_load(&compiled) == expected);
}
static void assert_clean(void)
{
   assert(!live_threads && !live_locks && !live_conds);
   assert(!atomic_load(&live_allocations) && !atomic_load(&live_cstates));
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
   puts("Lightrec startup, claimed queue, pause/resume and allocation failures: PASS");
   return 0;
}
