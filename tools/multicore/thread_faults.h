#ifndef MULTICORE_THREAD_FAULTS_H
#define MULTICORE_THREAD_FAULTS_H

#include <assert.h>
#include <rthreads/rthreads.h>

/* Include the production implementation after these wrappers to exercise
 * every partially allocated worker without replacing real synchronization. */
static unsigned allocation_index;
static unsigned fail_allocation;
static unsigned live_locks, live_conds, live_threads;

static slock_t *test_slock_new(void)
{
   slock_t *lock;
   if (++allocation_index == fail_allocation)
      return NULL;
   lock = slock_new();
   live_locks += lock != NULL;
   return lock;
}

static void test_slock_free(slock_t *lock)
{
   assert(lock && live_locks);
   live_locks--;
   slock_free(lock);
}

static scond_t *test_scond_new(void)
{
   scond_t *cond;
   if (++allocation_index == fail_allocation)
      return NULL;
   cond = scond_new();
   live_conds += cond != NULL;
   return cond;
}

static void test_scond_free(scond_t *cond)
{
   assert(cond && live_conds);
   live_conds--;
   scond_free(cond);
}

static sthread_t *test_sthread_create(void (*fn)(void *), void *arg)
{
   sthread_t *thread;
   if (++allocation_index == fail_allocation)
      return NULL;
   thread = sthread_create(fn, arg);
   live_threads += thread != NULL;
   return thread;
}

static void test_sthread_join(sthread_t *thread)
{
   assert(thread && live_threads);
   live_threads--;
   sthread_join(thread);
}

#define slock_new test_slock_new
#define slock_free test_slock_free
#define scond_new test_scond_new
#define scond_free test_scond_free
#define sthread_create test_sthread_create
#define sthread_join test_sthread_join

#endif
