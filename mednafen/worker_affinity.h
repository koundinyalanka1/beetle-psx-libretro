#ifndef BEETLE_WORKER_AFFINITY_H
#define BEETLE_WORKER_AFFINITY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call once, from a core-owned worker. On Linux/Android, widen an inherited
 * single-CPU pin to the thread-group leader's mask if it is a strict superset.
 * Existing multi-CPU masks, scheduling priority and the caller's creator are
 * untouched. Return the verified eligible CPU count, or 0 if unavailable. */
unsigned beetle_worker_init_affinity(void);

#ifdef __cplusplus
}
#endif
#endif
