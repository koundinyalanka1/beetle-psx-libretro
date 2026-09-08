#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "worker_affinity.h"

/* Tests inject the OS calls while exercising this same implementation. */
#if defined(__linux__) && !defined(BEETLE_WORKER_AFFINITY_TEST)
#include <sched.h>
#include <unistd.h>
#endif

#if defined(__linux__) || defined(BEETLE_WORKER_AFFINITY_TEST)
static unsigned worker_mask_count(const cpu_set_t *mask)
{
   unsigned i, count = 0;
   for (i = 0; i < CPU_SETSIZE; i++)
      count += CPU_ISSET(i, mask) != 0;
   return count;
}
#endif

unsigned beetle_worker_init_affinity(void)
{
#if defined(__linux__) || defined(BEETLE_WORKER_AFFINITY_TEST)
   cpu_set_t inherited, process, actual;
   unsigned i, count;

   CPU_ZERO(&inherited);
   if (sched_getaffinity(0, sizeof(inherited), &inherited) != 0)
      return 0;
   count = worker_mask_count(&inherited);

   /* A multi-CPU mask may be an intentional frontend cluster policy. The
    * problem addressed here is workers inheriting one emulation CPU. */
   if (count != 1)
      return count;

   CPU_ZERO(&process);
   if (sched_getaffinity(getpid(), sizeof(process), &process) != 0 ||
         worker_mask_count(&process) <= count)
      return count;
   for (i = 0; i < CPU_SETSIZE; i++)
      if (CPU_ISSET(i, &inherited) && !CPU_ISSET(i, &process))
         return count;

   /* pid 0 changes only this worker, never the frontend/emulation thread.
    * The kernel still intersects this request with this worker's cpuset and
    * online CPUs. Do not guess CPU IDs or raise scheduling priority. */
   if (sched_setaffinity(0, sizeof(process), &process) != 0)
      return count;
   CPU_ZERO(&actual);
   if (sched_getaffinity(0, sizeof(actual), &actual) != 0)
      return 0;
   return worker_mask_count(&actual);
#else
   /* Other platforms retain their normal scheduler policy. */
   return 0;
#endif
}
