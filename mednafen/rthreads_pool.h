#ifndef __MDFN_RTHREADS_POOL_H
#define __MDFN_RTHREADS_POOL_H

#include <stdbool.h>
#include <stdint.h>
#include <rthreads/rthreads.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rthreads_pool rthreads_pool_t;

typedef void (*rthreads_task_fn_t)(void *userdata, unsigned thread_index);

/**
 * Creates a worker thread pool with the specified number of worker threads.
 * If num_threads == 0, autodetects physical/logical cores, clamped to [1, 16].
 * Returns NULL on failure or if threading is unavailable.
 */
rthreads_pool_t *rthreads_pool_new(unsigned num_threads);

/**
 * Returns the number of worker threads in the pool.
 */
unsigned rthreads_pool_get_thread_count(const rthreads_pool_t *pool);

/**
 * Submits a batch of tasks to the pool and waits for all of them to complete.
 * If pool is NULL or num_threads == 0, runs all tasks synchronously on the calling thread.
 *
 * @param pool The thread pool handle.
 * @param fn The task function to execute.
 * @param task_data Array of pointers to data passed to each task invocation.
 * @param task_count Number of tasks to execute.
 */
void rthreads_pool_dispatch_and_wait(rthreads_pool_t *pool,
                                     rthreads_task_fn_t fn,
                                     void **task_data,
                                     unsigned task_count);

/**
 * Destroys the thread pool, waiting for any pending work to finish and joining all threads.
 */
void rthreads_pool_free(rthreads_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* __MDFN_RTHREADS_POOL_H */
