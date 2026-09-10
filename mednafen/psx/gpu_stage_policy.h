#ifndef BEETLE_GPU_STAGE_POLICY_H
#define BEETLE_GPU_STAGE_POLICY_H

#include <stdint.h>
#include <stdbool.h>

#include <retro_inline.h>

/*
 * When GP0 staging is worth its keep.
 *
 * gpu.c stages GP0 words so a long burst can be handed to the render worker
 * instead of executed on the emulation thread.  That only pays when the guest
 * actually streams.  The September 8 Tekken 3 capture shows the opposite in
 * the scene that misses the frame budget: 14,357 GP0 words a frame, all of
 * them retired inline, worker busy 0.00 ms/frame in every sampled window, and
 * ~1,032 DMA readiness polls a frame of which ~98% found staged words and
 * forced an immediate resolve - about fourteen words per batch against a
 * publication threshold of 1,024.  A later scene in the same session behaves
 * the other way round (1,230 of 1,405 polls answered from the published
 * snapshot), so this is a property of what the guest is doing at the time, not
 * of the game or the device.
 *
 * This is the decision, separated from the ordering and barrier rules it
 * feeds, so it can be replayed against recorded batch shapes without a
 * renderer, a GPU or a device.  It holds no emulated state: every transition
 * is driven by counts gpu.c already had, and the words it governs execute in
 * the same order on the same thread either way.
 *
 * Hysteresis in both directions, so a scene that mixes the two shapes does not
 * thrash: engage only after a long run of batches too short to have paid for a
 * handover, and re-arm the worker as soon as one run reaches the publication
 * threshold.
 */

/* A batch shorter than this could not have paid for a worker handover. */
#define GPU_STAGE_POLICY_SHORT_DIV  8
/* Consecutive short batches before staging is bypassed. */
#define GPU_STAGE_POLICY_TRIP       64

typedef struct
{
   /* Publication threshold, mirroring GPU_STAGE_MAX. */
   uint32_t stage_max;
   bool     bypass;
   /* Consecutive inline resolves too short to have paid for a handover. */
   uint32_t short_n;
   /* GP0 words retired since the last drain while bypassing. */
   uint32_t run;
   /* Diagnostics, reset with the rest of the worker statistics. */
   uint32_t words;
   uint32_t polls;
   uint32_t entries;
   uint32_t exits;
} gpu_stage_policy_t;

static INLINE uint32_t GPU_StagePolicy_ShortBatch(const gpu_stage_policy_t *p)
{
   uint32_t n = p->stage_max / GPU_STAGE_POLICY_SHORT_DIV;
   return n ? n : 1;
}

/* Forget what the guest was doing: a new worker session, a reset, or a state
 * load that replaces the FIFO the bypass answers from.  Diagnostics survive,
 * because they are cleared on the statistics interval instead. */
static INLINE void GPU_StagePolicy_Reset(gpu_stage_policy_t *p,
      uint32_t stage_max)
{
   p->stage_max = stage_max;
   p->bypass    = false;
   p->short_n   = 0;
   p->run       = 0;
}

/* Leave the bypass.  Returns true when it had been engaged, which is the
 * caller's cue to refresh anything the bypass left stale. */
static INLINE bool GPU_StagePolicy_End(gpu_stage_policy_t *p)
{
   bool was = p->bypass;
   p->bypass  = false;
   p->short_n = 0;
   p->run     = 0;
   if (was)
      p->exits++;
   return was;
}

/* A staged batch of `n` words just ran on the emulation thread, so nothing is
 * outstanding at this point.  Returns true when that engaged the bypass. */
static INLINE bool GPU_StagePolicy_Resolved(gpu_stage_policy_t *p, uint32_t n)
{
   if (n >= GPU_StagePolicy_ShortBatch(p))
   {
      p->short_n = 0;
      return false;
   }

   if (++p->short_n < GPU_STAGE_POLICY_TRIP || p->bypass)
      return false;

   p->bypass = true;
   p->run    = 0;
   p->entries++;
   return true;
}

/* One GP0 word retired on the bypass path.  Returns true when the run has
 * reached the publication threshold and the worker should be re-armed. */
static INLINE bool GPU_StagePolicy_Word(gpu_stage_policy_t *p)
{
   p->words++;
   return ++p->run >= p->stage_max;
}

/* An observation that ends whatever batch the guest was building: a status
 * read, a readiness poll, or any other drain. */
static INLINE void GPU_StagePolicy_Drain(gpu_stage_policy_t *p)
{
   p->run = 0;
}

/* A readiness poll answered from the decoder without a barrier. */
static INLINE void GPU_StagePolicy_Poll(gpu_stage_policy_t *p)
{
   p->polls++;
   p->run = 0;
}

#endif
