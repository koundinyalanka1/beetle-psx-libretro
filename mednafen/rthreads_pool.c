#include "rthreads_pool.h"
#include <features/features_cpu.h>
#include <stdlib.h>
#include <string.h>

#define MAX_POOL_THREADS 16

struct rthreads_pool
{
   sthread_t *threads[MAX_POOL_THREADS];
   unsigned thread_count;

   slock_t *lock;
   scond_t *work_cond;
   scond_t *done_cond;

   rthreads_task_fn_t current_fn;
   void **current_task_data;
   unsigned current_task_count;
   unsigned next_task_index;
   unsigned tasks_remaining;

   bool stopping;
};

static void pool_worker_loop(void *userdata)
{
   rthreads_pool_t *pool = (rthreads_pool_t *)userdata;

   slock_lock(pool->lock);

   while (!pool->stopping)
   {
      /* While there's work to do in the current dispatch */
      while (pool->next_task_index < pool->current_task_count && !pool->stopping)
      {
         unsigned task_idx = pool->next_task_index++;
         rthreads_task_fn_t fn = pool->current_fn;
         void *data = pool->current_task_data ? pool->current_task_data[task_idx] : NULL;

         slock_unlock(pool->lock);

         /* Execute the task without holding the lock */
         if (fn)
            fn(data, task_idx);

         slock_lock(pool->lock);

         pool->tasks_remaining--;
         if (pool->tasks_remaining == 0)
            scond_signal(pool->done_cond);
      }

      if (pool->stopping)
         break;

      /* Wait for next dispatch */
      scond_wait(pool->work_cond, pool->lock);
   }

   slock_unlock(pool->lock);
}

rthreads_pool_t *rthreads_pool_new(unsigned num_threads)
{
   unsigned i;
   rthreads_pool_t *pool;

   if (num_threads == 0)
   {
      num_threads = cpu_features_get_core_amount();
      if (num_threads < 1)
         num_threads = 1;
   }

   if (num_threads > MAX_POOL_THREADS)
      num_threads = MAX_POOL_THREADS;

   pool = (rthreads_pool_t *)calloc(1, sizeof(rthreads_pool_t));
   if (!pool)
      return NULL;

   pool->lock = slock_new();
   pool->work_cond = scond_new();
   pool->done_cond = scond_new();

   if (!pool->lock || !pool->work_cond || !pool->done_cond)
   {
      rthreads_pool_free(pool);
      return NULL;
   }

   pool->thread_count = num_threads;
   pool->stopping = false;

   for (i = 0; i < num_threads; i++)
   {
      pool->threads[i] = sthread_create(pool_worker_loop, pool);
      if (!pool->threads[i])
      {
         pool->thread_count = i;
         break;
      }
   }

   return pool;
}

unsigned rthreads_pool_get_thread_count(const rthreads_pool_t *pool)
{
   return pool ? pool->thread_count : 0;
}

void rthreads_pool_dispatch_and_wait(rthreads_pool_t *pool,
                                     rthreads_task_fn_t fn,
                                     void **task_data,
                                     unsigned task_count)
{
   unsigned i;

   if (!fn || task_count == 0)
      return;

   /* If pool is inactive or has only 1 thread, execute synchronously on caller */
   if (!pool || pool->thread_count <= 1)
   {
      for (i = 0; i < task_count; i++)
      {
         void *data = task_data ? task_data[i] : NULL;
         fn(data, i);
      }
      return;
   }

   slock_lock(pool->lock);

   pool->current_fn = fn;
   pool->current_task_data = task_data;
   pool->current_task_count = task_count;
   pool->next_task_index = 0;
   pool->tasks_remaining = task_count;

   /* Wake up all workers */
   scond_broadcast(pool->work_cond);

   /* Wait for all tasks to finish */
   while (pool->tasks_remaining > 0 && !pool->stopping)
   {
      scond_wait(pool->done_cond, pool->lock);
   }

   pool->current_fn = NULL;
   pool->current_task_data = NULL;
   pool->current_task_count = 0;

   slock_unlock(pool->lock);
}

void rthreads_pool_free(rthreads_pool_t *pool)
{
   unsigned i;
   if (!pool)
      return;

   if (pool->lock)
   {
      slock_lock(pool->lock);
      pool->stopping = true;
      if (pool->work_cond)
         scond_broadcast(pool->work_cond);
      slock_unlock(pool->lock);
   }

   for (i = 0; i < pool->thread_count; i++)
   {
      if (pool->threads[i])
      {
         sthread_join(pool->threads[i]);
         pool->threads[i] = NULL;
      }
   }

   if (pool->done_cond)
      scond_free(pool->done_cond);
   if (pool->work_cond)
      scond_free(pool->work_cond);
   if (pool->lock)
      slock_free(pool->lock);

   free(pool);
}
