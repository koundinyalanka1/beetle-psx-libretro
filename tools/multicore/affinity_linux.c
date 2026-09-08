#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include "../../mednafen/worker_affinity.h"

static cpu_set_t original;
static unsigned expected;

static void *worker(void *unused)
{
   unsigned i;
   cpu_set_t pinned, actual;
   (void)unused;
   CPU_ZERO(&pinned);
   for (i = 0; i < CPU_SETSIZE; i++)
      if (CPU_ISSET(i, &original)) { CPU_SET(i, &pinned); break; }
   assert(sched_setaffinity(0, sizeof(pinned), &pinned) == 0);
   assert(beetle_worker_init_affinity() == expected);
   assert(sched_getaffinity(0, sizeof(actual), &actual) == 0);
   assert(CPU_EQUAL(&actual, &original));
   return NULL;
}

int main(void)
{
   pthread_t thread;
   cpu_set_t after;
   assert(sched_getaffinity(0, sizeof(original), &original) == 0);
   expected = CPU_COUNT(&original);
   assert(expected);
   assert(pthread_create(&thread, NULL, worker, NULL) == 0);
   assert(pthread_join(thread, NULL) == 0);
   assert(sched_getaffinity(0, sizeof(after), &after) == 0);
   assert(CPU_EQUAL(&original, &after));
   printf("Affinity: real worker eligible on %u CPUs; creator mask preserved\n", expected);
   return 0;
}
