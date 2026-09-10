#ifndef BEETLE_RHI_GEOMETRY_WORKER_H
#define BEETLE_RHI_GEOMETRY_WORKER_H

#include "rhi_defer.h"

/* Single producer; the callback runs on one worker and must not call back
 * into this queue. Only value-owned geometry ops are accepted (no pixels).
 * Sync transfers exclusive backend ownership back to the producer. */
typedef struct rhi_geometry_worker rhi_geometry_worker_t;
/*
 * Where the frame went, split so the two costs are not confused.
 *
 * busy_us is worker-thread time and overlaps the emulation thread; it is the
 * work that moved off the critical path, and it must not be added to frame
 * time.  The three wait figures are emulation-thread time and are serial:
 *
 *   backpressure_us  the producer outran the renderer and blocked for a free
 *                    batch bank.  Rising with busy_us means the renderer is
 *                    the limit; rising alone means the banks are too few.
 *   barrier_us       a state change, VRAM access or frame boundary needed
 *                    exclusive backend ownership and had to drain.
 *   barriers         drains requested, whether or not they blocked.  Compared
 *                    against commands this says how long a run of geometry
 *                    the guest actually leaves undisturbed.
 */
typedef struct
{
   uint64_t commands, batches, busy_us;
   uint64_t backpressure_us, barrier_us;
   uint32_t backpressure_n, barrier_n, barriers;
   unsigned eligible_cpus;
} rhi_geometry_stats_t;

rhi_geometry_worker_t *rhi_geometry_worker_new(rhi_defer_dispatch_fn dispatch,
      void *user);
void rhi_geometry_worker_push(rhi_geometry_worker_t *worker,
      const rhi_defer_op_t *op);
/* Queue `op` only when the worker already has work outstanding. With an empty
 * queue and a parked worker the caller already owns the backend, so applying
 * the change itself is both ordered and cheaper: a queued op costs a
 * full-union copy (232 bytes) and a wakeup, which an eight-byte renderer state
 * change should not pay when there is nothing to stay ahead of. Returns false
 * when the caller should apply it directly. */
bool rhi_geometry_worker_queue_if_busy(rhi_geometry_worker_t *worker,
      const rhi_defer_op_t *op);
void rhi_geometry_worker_sync(rhi_geometry_worker_t *worker);
void rhi_geometry_worker_free(rhi_geometry_worker_t *worker);
/* Drains before reading/resetting the worker-owned counters. */
void rhi_geometry_worker_stats(rhi_geometry_worker_t *worker,
      rhi_geometry_stats_t *stats);

#endif
