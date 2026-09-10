#include "rthreads_worker.h"
#include <stdlib.h>
#include <string.h>

/* Bounded waits, so a missed signal degrades into a short stall rather than a
 * permanently blocked caller. */
#define RTHREADS_WORKER_WAIT_US 20000

typedef struct
{
   rthreads_job_fn_t fn;
   void *data;
} rthreads_job_t;

struct rthreads_worker
{
   sthread_t *thread;
   slock_t *lock;
   scond_t *not_empty;
   scond_t *not_full;
   scond_t *drained;

   rthreads_job_t *queue;
   unsigned capacity;
   unsigned read_pos;
   unsigned write_pos;
   unsigned count;

   bool stopping;
   bool idle;
};

static void worker_thread_entry(void *userdata)
{
   rthreads_worker_t *worker = (rthreads_worker_t *)userdata;

   slock_lock(worker->lock);

   for (;;)
   {
      while (worker->count == 0 && !worker->stopping)
      {
         worker->idle = true;
         scond_broadcast(worker->drained);
         scond_wait_timeout(worker->not_empty, worker->lock,
               RTHREADS_WORKER_WAIT_US);
      }

      if (worker->stopping && worker->count == 0)
         break;

      worker->idle = false;
      rthreads_job_t job = worker->queue[worker->read_pos];
      worker->read_pos = (worker->read_pos + 1) % worker->capacity;
      worker->count--;

      scond_broadcast(worker->not_full);

      slock_unlock(worker->lock);

      if (job.fn)
         job.fn(job.data);

      slock_lock(worker->lock);

      if (worker->count == 0)
      {
         worker->idle = true;
         scond_broadcast(worker->drained);
      }
   }

   worker->idle = true;
   scond_broadcast(worker->drained);
   slock_unlock(worker->lock);
}

rthreads_worker_t *rthreads_worker_new(const char *name, unsigned queue_capacity)
{
   rthreads_worker_t *worker;

   (void)name;
   if (queue_capacity < 16)
      queue_capacity = 16;

   worker = (rthreads_worker_t *)calloc(1, sizeof(rthreads_worker_t));
   if (!worker)
      return NULL;

   worker->capacity = queue_capacity;
   worker->queue = (rthreads_job_t *)calloc(queue_capacity, sizeof(rthreads_job_t));
   if (!worker->queue)
   {
      free(worker);
      return NULL;
   }

   worker->lock = slock_new();
   worker->not_empty = scond_new();
   worker->not_full = scond_new();
   worker->drained = scond_new();

   if (!worker->lock || !worker->not_empty || !worker->not_full || !worker->drained)
   {
      rthreads_worker_free(worker);
      return NULL;
   }

   worker->idle = true;
   worker->stopping = false;
   worker->thread = sthread_create(worker_thread_entry, worker);
   if (!worker->thread)
   {
      rthreads_worker_free(worker);
      return NULL;
   }

   return worker;
}

bool rthreads_worker_post(rthreads_worker_t *worker, rthreads_job_fn_t fn, void *data)
{
   if (!worker || !fn)
      return false;

   slock_lock(worker->lock);

   while (worker->count >= worker->capacity && !worker->stopping)
   {
      scond_wait_timeout(worker->not_full, worker->lock,
            RTHREADS_WORKER_WAIT_US);
   }

   if (worker->stopping)
   {
      slock_unlock(worker->lock);
      return false;
   }

   worker->queue[worker->write_pos].fn = fn;
   worker->queue[worker->write_pos].data = data;
   worker->write_pos = (worker->write_pos + 1) % worker->capacity;
   worker->count++;

   scond_broadcast(worker->not_empty);
   slock_unlock(worker->lock);

   return true;
}

void rthreads_worker_wait(rthreads_worker_t *worker)
{
   if (!worker)
      return;

   slock_lock(worker->lock);
   while (worker->count > 0 || !worker->idle)
   {
      scond_wait_timeout(worker->drained, worker->lock,
            RTHREADS_WORKER_WAIT_US);
   }
   slock_unlock(worker->lock);
}

void rthreads_worker_free(rthreads_worker_t *worker)
{
   if (!worker)
      return;

   if (worker->lock)
   {
      slock_lock(worker->lock);
      worker->stopping = true;
      if (worker->not_empty)
         scond_broadcast(worker->not_empty);
      if (worker->not_full)
         scond_broadcast(worker->not_full);
      slock_unlock(worker->lock);
   }

   if (worker->thread)
   {
      sthread_join(worker->thread);
      worker->thread = NULL;
   }

   if (worker->drained)
      scond_free(worker->drained);
   if (worker->not_full)
      scond_free(worker->not_full);
   if (worker->not_empty)
      scond_free(worker->not_empty);
   if (worker->lock)
      slock_free(worker->lock);

   if (worker->queue)
      free(worker->queue);

   free(worker);
}
