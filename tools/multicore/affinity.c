/* Exercise production affinity handling with deterministic OS responses. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BEETLE_WORKER_AFFINITY_TEST
#define CPU_SETSIZE 128
typedef struct { uint64_t bits[2]; } cpu_set_t;
#define CPU_ZERO(p) memset((p), 0, sizeof(*(p)))
#define CPU_SET(i, p) ((p)->bits[(i) / 64] |= UINT64_C(1) << ((i) % 64))
#define CPU_ISSET(i, p) (((p)->bits[(i) / 64] >> ((i) % 64)) & 1)

static cpu_set_t current, leader, permitted;
static unsigned get_calls, set_calls, fail_get;
static int fail_set;

static int getpid(void) { return 42; }
static int sched_getaffinity(int pid, size_t size, cpu_set_t *mask)
{
   assert(size == sizeof(*mask));
   assert(pid == 0 || pid == 42);
   if (++get_calls == fail_get)
      return -1;
   *mask = pid == 0 ? current : leader;
   return 0;
}
static int sched_setaffinity(int pid, size_t size, const cpu_set_t *mask)
{
   unsigned i;
   assert(pid == 0); /* Never change the frontend or the emulation thread. */
   assert(size == sizeof(*mask));
   set_calls++;
   if (fail_set)
      return -1;
   for (i = 0; i < 2; i++)
      current.bits[i] = mask->bits[i] & permitted.bits[i];
   return 0;
}

#include "../../mednafen/worker_affinity.c"

static void reset(void)
{
   unsigned i;
   CPU_ZERO(&current);
   CPU_ZERO(&leader);
   CPU_SET(3, &current);
   for (i = 0; i < 4; i++) CPU_SET(i, &leader);
   permitted = leader;
   get_calls = set_calls = fail_get = 0;
   fail_set = 0;
}

int main(void)
{
   reset();
   assert(beetle_worker_init_affinity() == 4 && set_calls == 1);
   assert(memcmp(&current, &leader, sizeof(current)) == 0);

   reset(); /* Existing cluster policy. */
   CPU_SET(2, &current);
   assert(beetle_worker_init_affinity() == 2 && set_calls == 0);

   reset(); /* App restricted to one CPU. */
   leader = current;
   assert(beetle_worker_init_affinity() == 1 && set_calls == 0);

   reset(); /* The leader's mask must not replace a disjoint performance pin. */
   CPU_ZERO(&current);
   CPU_SET(7, &current);
   assert(beetle_worker_init_affinity() == 1 && set_calls == 0);
   assert(CPU_ISSET(7, &current));

   reset(); /* Per-thread cpuset can be narrower than the leader's mask. */
   permitted = current;
   assert(beetle_worker_init_affinity() == 1 && set_calls == 1);

   reset(); /* Sparse CPU IDs, including IDs above one machine word. */
   CPU_ZERO(&current);
   CPU_ZERO(&leader);
   CPU_SET(97, &current);
   CPU_SET(97, &leader);
   CPU_SET(65, &leader);
   permitted = leader;
   assert(beetle_worker_init_affinity() == 2 && set_calls == 1);

   reset(); fail_get = 1;
   assert(beetle_worker_init_affinity() == 0 && set_calls == 0);
   reset(); fail_get = 2;
   assert(beetle_worker_init_affinity() == 1 && set_calls == 0);
   reset(); fail_set = 1;
   assert(beetle_worker_init_affinity() == 1 && CPU_ISSET(3, &current));
   reset(); fail_get = 3;
   assert(beetle_worker_init_affinity() == 0 && set_calls == 1);
   puts("Affinity: inherited pins, cluster policy, sparse masks, cpuset limits and OS failures passed");
   return 0;
}
