#ifndef __MDFN_RTHREADS_WORKER_H
#define __MDFN_RTHREADS_WORKER_H

#include <stdbool.h>
#include <stdint.h>
#include <rthreads/rthreads.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rthreads_worker rthreads_worker_t;

typedef void (*rthreads_job_fn_t)(void *userdata);

/**
 * Creates an asynchronous worker thread with a job queue.
 * @param name Diagnostic name for the worker thread.
 * @param queue_capacity Maximum capacity of job queue (e.g. 4096 or 65536).
 */
rthreads_worker_t *rthreads_worker_new(const char *name, unsigned queue_capacity);

/**
 * Enqueues a job for execution on the worker thread.
 * If queue is full, blocks until space becomes available.
 */
bool rthreads_worker_post(rthreads_worker_t *worker, rthreads_job_fn_t fn, void *data);

/**
 * Blocks the calling thread until all currently pending jobs have finished executing.
 */
void rthreads_worker_wait(rthreads_worker_t *worker);

/**
 * Finishes all queued jobs, joins the worker thread, and frees all resources.
 * The owner must stop other callers before freeing the worker. Jobs must not
 * post to, wait for, or destroy their own worker.
 */
void rthreads_worker_free(rthreads_worker_t *worker);

#ifdef __cplusplus
}
#endif

#endif /* __MDFN_RTHREADS_WORKER_H */
