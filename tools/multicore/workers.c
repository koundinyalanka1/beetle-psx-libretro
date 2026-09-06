#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "thread_faults.h"
#include "../../mednafen/rthreads_worker.c"
#include "../../mednafen/rthreads_pool.c"

unsigned cpu_features_get_core_amount(void) { return 4; }

typedef struct
{
   unsigned expected;
   unsigned *completed;
} job_data_t;

static void ordered_job(void *data)
{
   job_data_t *job = data;
   struct timespec delay = {0, 10000};
   assert(*job->completed == job->expected);
   nanosleep(&delay, NULL);
   (*job->completed)++;
}

static void pool_task(void *data, unsigned index)
{
   unsigned *seen = data;
   assert(*seen == 0);
   *seen = index + 1;
}

typedef struct { rthreads_pool_t *pool; } dispatcher_t;
static void *dispatch_batches(void *data)
{
   dispatcher_t *dispatcher = data;
   unsigned batch, i;
   for (batch = 0; batch < 200; batch++)
   {
      unsigned seen[97] = {0};
      void *tasks[97];
      for (i = 0; i < 97; i++)
         tasks[i] = &seen[i];
      rthreads_pool_dispatch_and_wait(dispatcher->pool, pool_task, tasks, 97);
      for (i = 0; i < 97; i++)
         assert(seen[i] == i + 1);
   }
   return NULL;
}

int main(void)
{
   unsigned i, completed = 0;
   job_data_t jobs[1024];
   rthreads_worker_t *worker;
   rthreads_pool_t *pool;

   for (i = 1; i <= 5; i++)
   {
      fail_allocation = i;
      allocation_index = 0;
      assert(!rthreads_worker_new("fault", 16));
      assert(!live_locks && !live_conds && !live_threads);
   }
   for (i = 1; i <= 8; i++)
   {
      fail_allocation = i;
      allocation_index = 0;
      pool = rthreads_pool_new(4);
      if (pool)
      {
         dispatcher_t dispatcher = {pool};
         dispatch_batches(&dispatcher);
         rthreads_pool_free(pool);
      }
      assert(!live_locks && !live_conds && !live_threads);
   }
   fail_allocation = 0;

   worker = rthreads_worker_new("queue", 16);
   assert(worker);
   for (i = 0; i < 1024; i++)
   {
      jobs[i].expected = i;
      jobs[i].completed = &completed;
      assert(rthreads_worker_post(worker, ordered_job, &jobs[i]));
      if (i == 500)
      {
         rthreads_worker_wait(worker);
         assert(completed == 501);
      }
   }
   /* Destruction must execute every queued job, even without an explicit wait. */
   rthreads_worker_free(worker);
   assert(completed == 1024);

   pool = rthreads_pool_new(4);
   assert(pool);
   {
      pthread_t callers[3];
      dispatcher_t dispatcher = {pool};
      for (i = 0; i < 3; i++)
         assert(pthread_create(&callers[i], NULL, dispatch_batches, &dispatcher) == 0);
      for (i = 0; i < 3; i++)
         assert(pthread_join(callers[i], NULL) == 0);
   }
   rthreads_pool_free(pool);
   assert(!live_locks && !live_conds && !live_threads);
   puts("Workers: FIFO/backpressure/drain, concurrent pool batches and allocation failures passed");
   return 0;
}
