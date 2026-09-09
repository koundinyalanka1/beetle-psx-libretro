#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "thread_faults.h"
#include "../../mednafen/rthreads_worker.c"
#include "../../rhi/rhi_geometry_worker.c"

static pthread_t producer;
static unsigned consumed, state_version;
static unsigned affinity_calls;

unsigned beetle_worker_init_affinity(void)
{
   assert(!pthread_equal(pthread_self(), producer));
   affinity_calls++;
   return 4;
}

retro_time_t cpu_features_get_time_usec(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (retro_time_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void draw(void *user, const rhi_defer_op_t *op)
{
   const unsigned *expected_state = (const unsigned *)user;
   assert(!pthread_equal(pthread_self(), producer));
   assert(state_version == *expected_state);
   assert(op->u.push_poly.c[0] == consumed);
   assert(op->u.push_poly.precise_rgb[0] == (float)consumed);
   assert(op->u.push_poly.fog[11] == (float)consumed + 11);
   assert(op->u.push_poly.has_precise_rgb && op->u.push_poly.has_fog);
   assert(op->u.push_poly.px[1] == 4 && op->u.push_poly.ty[2] == 18);
   assert(op->u.push_poly.blend_mode == 2 && op->u.push_poly.set_mask);
   if ((consumed & 63) == 0)
   {
      /* Let the producer outrun the renderer and wrap the bounded banks. */
      struct timespec delay = {0, 100000};
      nanosleep(&delay, NULL);
   }
   consumed++;
}

int main(void)
{
   unsigned i, round, expected_state = 0;
   rhi_geometry_worker_t *w;
   rhi_geometry_stats_t stats;
   producer = pthread_self();

   for (i = 1; i <= 5; i++)
   {
      allocation_index = 0;
      fail_allocation = i;
      assert(!rhi_geometry_worker_new(draw, &expected_state));
      assert(!live_locks && !live_conds && !live_threads);
   }
   fail_allocation = 0;
   w = rhi_geometry_worker_new(draw, &expected_state);
   assert(w);
   for (round = 0; round < 8; round++)
   {
      for (i = 0; i < 263; i++)
      {
         unsigned id = round * 263 + i;
         float rgb[9] = {0}, fog[12];
         unsigned j;
         rhi_defer_op_t op;
         rhi_defer_queue_t q = {0};
         q.ops = &op;
         q.capacity = 1;
         rgb[0] = (float)id;
         for (j = 0; j < 12; j++)
            fog[j] = (float)id + j;
         rhi_defer_push_triangle(&q,
               1, 2, 3, 4, 5, 6, 7, 8, 9, id, 10, 11, rgb, fog,
               13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
               25, 26, 2, 1, true, 2, true, true);
         rhi_geometry_worker_push(w, &op);
         /* Reused stack inputs must not alter an already queued command. */
         memset(&op, 0xa5, sizeof(op));
         memset(rgb, 0, sizeof(rgb));
         memset(fog, 0, sizeof(fog));
      }
      /* Partial batches must finish before state mutation / readback. */
      rhi_geometry_worker_sync(w);
      assert(consumed == (round + 1) * 263);
      state_version++;
      expected_state++;
   }
   rhi_geometry_worker_stats(w, &stats);
   assert(stats.commands == 2104 && stats.batches == 72);
   assert(stats.eligible_cpus == 4 && affinity_calls == 1);
   /* Every sync counts as a barrier whether or not it blocked: 8 round
    * barriers plus the one this stats call performed. */
   assert(stats.barriers == 9);
   /* A partial batch was always outstanding at each round barrier, so those
    * really blocked; the producer also outran four banks often enough to be
    * pushed back at least once. */
   assert(stats.barrier_n == 8 && stats.backpressure_n > 0);
   assert(stats.barrier_us + stats.backpressure_us > 0);
   rhi_geometry_worker_stats(w, &stats);
   assert(!stats.commands && !stats.batches && !stats.busy_us);
   assert(!stats.backpressure_n && !stats.barrier_us);
   /* Nothing was queued, so that stats call took the lock-free path. */
   assert(stats.barriers == 1 && stats.barrier_n == 0);

   /* An idle barrier must not block, must not disturb the worker, and must
    * still leave the caller owning the backend. */
   for (i = 0; i < 64; i++)
      rhi_geometry_worker_sync(w);
   rhi_geometry_worker_stats(w, &stats);
   assert(stats.barriers == 65 && stats.barrier_n == 0 && !stats.barrier_us);
   assert(!stats.commands && !stats.batches);

   /* One command then a barrier: the partial batch must run before the sync
    * returns, and that barrier must be counted as having blocked. */
   consumed = 0;
   state_version = 0;
   expected_state = 0;
   {
      float rgb[9] = {0}, fog[12];
      unsigned j;
      rhi_defer_op_t op;
      rhi_defer_queue_t q = {0};
      q.ops = &op;
      q.capacity = 1;
      for (j = 0; j < 12; j++)
         fog[j] = (float)j;
      rhi_defer_push_triangle(&q, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 10, 11, rgb, fog,
            13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 2, 1,
            true, 2, true, true);
      rhi_geometry_worker_push(w, &op);
      rhi_geometry_worker_sync(w);
      assert(consumed == 1);
   }
   rhi_geometry_worker_stats(w, &stats);
   assert(stats.commands == 1 && stats.batches == 1);
   assert(stats.barriers == 2 && stats.barrier_n == 1);

   rhi_geometry_worker_free(w);
   assert(!live_locks && !live_conds && !live_threads);

   /* An empty lifecycle is valid too. */
   w = rhi_geometry_worker_new(draw, &expected_state);
   assert(w);
   rhi_geometry_worker_free(w);
   assert(!live_locks && !live_conds && !live_threads);
   puts("Geometry worker: 2104 ordered snapshots, backpressure, state barriers, idle-barrier fast path, split stall accounting and startup failures passed");
   return 0;
}
