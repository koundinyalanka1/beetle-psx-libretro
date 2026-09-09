#include "rhi_geometry_worker.h"
#include "../mednafen/rthreads_worker.h"
#include "../mednafen/worker_affinity.h"
#include <features/features_cpu.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Fixed storage, no allocations or locks per primitive. A full batch can
 * render while the CPU fills the next one. Backpressure bounds memory even
 * when the renderer falls behind. */
#define GEOMETRY_BATCH_SIZE 32
#define GEOMETRY_BATCH_COUNT 4

typedef struct
{
   struct rhi_geometry_worker *owner;
   unsigned count;
   bool pending;
   rhi_defer_op_t ops[GEOMETRY_BATCH_SIZE];
} geometry_batch_t;

struct rhi_geometry_worker
{
   rthreads_worker_t *thread;
   rhi_defer_dispatch_fn dispatch;
   void *user;
   unsigned current;
   bool affinity_initialized;
   rhi_geometry_stats_t stats;
   geometry_batch_t batches[GEOMETRY_BATCH_COUNT];
};

static void geometry_run(void *data)
{
   geometry_batch_t *batch = (geometry_batch_t *)data;
   rhi_geometry_worker_t *w = batch->owner;
   retro_time_t start;
   unsigned i;
   if (!w->affinity_initialized)
   {
      w->stats.eligible_cpus = beetle_worker_init_affinity();
      w->affinity_initialized = true;
   }
   start = cpu_features_get_time_usec();
   for (i = 0; i < batch->count; i++)
      w->dispatch(w->user, &batch->ops[i]);
   w->stats.busy_us += cpu_features_get_time_usec() - start;
   w->stats.commands += batch->count;
   w->stats.batches++;
   /* Published only after the last callback stops using the batch. */
   __atomic_store_n(&batch->pending, false, __ATOMIC_RELEASE);
}

/* True while any bank is posted or executing.  Each `pending` is released by
 * the worker only after the last callback of that batch has returned, so a
 * producer that reads them all false has synchronized with every renderer
 * write the worker made.  The producer is the only poster, so nothing can
 * become outstanding again behind this test. */
static bool geometry_inflight(const rhi_geometry_worker_t *w)
{
   unsigned i;
   for (i = 0; i < GEOMETRY_BATCH_COUNT; i++)
      if (__atomic_load_n(&w->batches[i].pending, __ATOMIC_ACQUIRE))
         return true;
   return false;
}

/* `barrier` separates the two reasons the producer stops: waiting for a free
 * bank (the renderer is behind) and waiting for exclusive ownership (the guest
 * asked for something the worker holds). They want opposite fixes, so they are
 * never summed into one number. */
static void geometry_wait(rhi_geometry_worker_t *w, bool barrier)
{
   retro_time_t start = cpu_features_get_time_usec();
   retro_time_t spent;
   rthreads_worker_wait(w->thread);
   spent = cpu_features_get_time_usec() - start;
   if (barrier)
   {
      w->stats.barrier_us += spent;
      w->stats.barrier_n++;
   }
   else
   {
      w->stats.backpressure_us += spent;
      w->stats.backpressure_n++;
   }
}

static void geometry_submit(rhi_geometry_worker_t *w)
{
   geometry_batch_t *batch = &w->batches[w->current];
   if (!batch->count)
      return;
   __atomic_store_n(&batch->pending, true, __ATOMIC_RELEASE);
   /* With one producer and no concurrent destruction, post cannot fail.
    * Keep an ordered inline fallback if that contract ever changes. */
   if (!rthreads_worker_post(w->thread, geometry_run, batch))
   {
      geometry_wait(w, false);
      geometry_run(batch);
   }
   w->current = (w->current + 1) % GEOMETRY_BATCH_COUNT;
   batch = &w->batches[w->current];
   if (__atomic_load_n(&batch->pending, __ATOMIC_ACQUIRE))
      geometry_wait(w, false);
   batch->count = 0;
}

rhi_geometry_worker_t *rhi_geometry_worker_new(rhi_defer_dispatch_fn dispatch,
      void *user)
{
   rhi_geometry_worker_t *w;
   unsigned i;
   if (!dispatch)
      return NULL;
   w = (rhi_geometry_worker_t *)calloc(1, sizeof(*w));
   if (!w)
      return NULL;
   w->dispatch = dispatch;
   w->user = user;
   for (i = 0; i < GEOMETRY_BATCH_COUNT; i++)
      w->batches[i].owner = w;
   w->thread = rthreads_worker_new("Vulkan geometry", GEOMETRY_BATCH_COUNT);
   if (!w->thread)
   {
      free(w);
      return NULL;
   }
   return w;
}

void rhi_geometry_worker_push(rhi_geometry_worker_t *w,
      const rhi_defer_op_t *op)
{
   geometry_batch_t *batch = &w->batches[w->current];
   assert(op->kind == RHI_DEFER_PUSH_TRIANGLE ||
          op->kind == RHI_DEFER_PUSH_QUAD || op->kind == RHI_DEFER_PUSH_LINE);
   batch->ops[batch->count++] = *op;
   if (batch->count == GEOMETRY_BATCH_SIZE)
      geometry_submit(w);
}

void rhi_geometry_worker_sync(rhi_geometry_worker_t *w)
{
   if (!w)
      return;
   w->stats.barriers++;
   /* Most barriers arrive with nothing outstanding - the guest changes state
    * far more often than it draws a full bank - and taking the worker's mutex
    * plus two clock reads to discover that is the whole cost on those. The
    * batch count is producer-owned and `pending` is an acquire load, so this
    * establishes the same ownership the drain below would. */
   if (!w->batches[w->current].count && !geometry_inflight(w))
      return;
   geometry_submit(w);
   geometry_wait(w, true);
}

void rhi_geometry_worker_stats(rhi_geometry_worker_t *w,
      rhi_geometry_stats_t *stats)
{
   rhi_geometry_worker_sync(w);
   *stats = w->stats;
   memset(&w->stats, 0, sizeof(w->stats));
   /* Not an interval measurement: it describes the worker, not the window. */
   w->stats.eligible_cpus = stats->eligible_cpus;
}

void rhi_geometry_worker_free(rhi_geometry_worker_t *w)
{
   if (!w)
      return;
   rhi_geometry_worker_sync(w);
   rthreads_worker_free(w->thread);
   free(w);
}
