/*
 * GP0 staging-policy regression tests.
 *
 * Replays recorded guest batching shapes through the production policy in
 * mednafen/psx/gpu_stage_policy.h and checks the transitions gpu.c depends on:
 *
 *   - the bypass never engages before the trip count, and never on a shape
 *     the worker could actually help with;
 *   - one long batch inside a poll-heavy stretch resets the streak, so a
 *     mixed scene does not flip the policy on a single sample;
 *   - a streaming run reaching the publication threshold re-arms the worker;
 *   - drains (status reads, readiness polls) end a run, so a burst only counts
 *     when the guest really did stream it uninterrupted;
 *   - reset and end are idempotent and leave the invariant gpu.c asserts.
 *
 * The two shapes come from the September 8 capture: the heavy Tekken 3 scene
 * interleaving ~14 GP0 words with a readiness poll, and the later scene that
 * streams and answers 1,230 of 1,405 polls from the published snapshot.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../mednafen/psx/gpu_stage_policy.h"

#define STAGE_MAX 1024

static unsigned checks;

/* One drain: whatever the guest was building ends, and the words that had
 * accumulated run on the emulation thread. */
static void drain(gpu_stage_policy_t *p, uint32_t pending)
{
   if (p->bypass)
      GPU_StagePolicy_Drain(p);
   else
      GPU_StagePolicy_Resolved(p, pending);
   checks++;
}

/* `words` GP0 words followed by a drain, the way the guest interleaves them. */
static void burst(gpu_stage_policy_t *p, uint32_t words)
{
   uint32_t staged = 0;
   uint32_t i;

   for (i = 0; i < words; i++)
   {
      if (p->bypass)
      {
         if (GPU_StagePolicy_Word(p))
            GPU_StagePolicy_End(p);   /* gpu.c re-arms the worker here */
      }
      else if (++staged >= p->stage_max)
      {
         /* Published to the worker; gpu.c clears the policy on any push. */
         GPU_StagePolicy_End(p);
         staged = 0;
      }
   }
   drain(p, staged);
}

static void test_poll_heavy_engages(void)
{
   gpu_stage_policy_t p = {0};
   unsigned i;

   GPU_StagePolicy_Reset(&p, STAGE_MAX);

   /* The measured plateau shape: ~14 words between readiness polls. */
   for (i = 0; i < GPU_STAGE_POLICY_TRIP - 1; i++)
   {
      burst(&p, 14);
      assert(!p.bypass);   /* never before the trip count */
   }
   burst(&p, 14);
   assert(p.bypass);
   assert(p.entries == 1 && p.exits == 0);

   /* Once engaged it stays engaged for the rest of the scene, and every word
    * and poll is answered without a barrier. */
   for (i = 0; i < 1000; i++)
      burst(&p, 14);
   assert(p.bypass);
   assert(p.entries == 1 && p.exits == 0);
   assert(p.words == 14 * 1000);
   checks++;
}

static void test_streaming_never_engages(void)
{
   gpu_stage_policy_t p = {0};
   unsigned i;

   GPU_StagePolicy_Reset(&p, STAGE_MAX);

   /* Batches long enough to have paid for a handover: the worker is exactly
    * what this shape wants, so the policy must stay out of the way. */
   for (i = 0; i < 4000; i++)
   {
      burst(&p, GPU_StagePolicy_ShortBatch(&p));
      assert(!p.bypass);
   }
   assert(p.entries == 0 && p.words == 0 && p.polls == 0);
   checks++;
}

static void test_one_long_batch_resets_the_streak(void)
{
   gpu_stage_policy_t p = {0};
   unsigned i, j;

   GPU_StagePolicy_Reset(&p, STAGE_MAX);

   /* A single publishable batch anywhere in the run keeps the policy off. */
   for (j = 0; j < 8; j++)
   {
      for (i = 0; i < GPU_STAGE_POLICY_TRIP - 1; i++)
      {
         burst(&p, 14);
         assert(!p.bypass);
      }
      burst(&p, GPU_StagePolicy_ShortBatch(&p) + 3);
      assert(!p.bypass);
   }
   assert(p.entries == 0);
   checks++;
}

static void test_streaming_re_arms_the_worker(void)
{
   gpu_stage_policy_t p = {0};
   unsigned i;

   GPU_StagePolicy_Reset(&p, STAGE_MAX);
   for (i = 0; i < GPU_STAGE_POLICY_TRIP; i++)
      burst(&p, 14);
   assert(p.bypass);

   /* The guest switches to streaming: one uninterrupted run reaching the
    * publication threshold hands the next batch back to the worker. */
   burst(&p, STAGE_MAX);
   assert(!p.bypass);
   assert(p.exits == 1);

   /* And the streak has to be earned again from scratch.  The drain that
    * ended the streaming burst is itself the first short batch of the new
    * streak, so one fewer poll-heavy batch is needed this time. */
   for (i = 0; !p.bypass && i < GPU_STAGE_POLICY_TRIP * 2; i++)
      burst(&p, 14);
   assert(p.bypass && p.entries == 2);
   assert(i == GPU_STAGE_POLICY_TRIP - 1);
   checks++;
}

static void test_drains_end_a_run(void)
{
   gpu_stage_policy_t p = {0};
   unsigned i;

   GPU_StagePolicy_Reset(&p, STAGE_MAX);
   for (i = 0; i < GPU_STAGE_POLICY_TRIP; i++)
      burst(&p, 14);
   assert(p.bypass);

   /* STAGE_MAX words in total, but never uninterrupted: this is the plateau,
    * not streaming, so the worker must not be re-armed. */
   for (i = 0; i < STAGE_MAX; i++)
   {
      GPU_StagePolicy_Word(&p);
      GPU_StagePolicy_Poll(&p);
   }
   assert(p.bypass && p.exits == 0);
   assert(p.polls == STAGE_MAX);

   /* A status read is a drain too. */
   for (i = 0; i < STAGE_MAX - 1; i++)
      assert(!GPU_StagePolicy_Word(&p));
   GPU_StagePolicy_Drain(&p);
   for (i = 0; i < STAGE_MAX - 1; i++)
      assert(!GPU_StagePolicy_Word(&p));
   assert(p.bypass);
   /* The threshold word does trip it. */
   assert(GPU_StagePolicy_Word(&p));
   checks++;
}

static void test_lifecycle(void)
{
   gpu_stage_policy_t p = {0};
   unsigned i;

   GPU_StagePolicy_Reset(&p, STAGE_MAX);
   for (i = 0; i < GPU_STAGE_POLICY_TRIP; i++)
      burst(&p, 14);
   assert(p.bypass);

   /* End reports whether it had been engaged, which is what tells gpu.c to
    * refresh the published readiness byte, and is idempotent after that. */
   assert(GPU_StagePolicy_End(&p));
   assert(!GPU_StagePolicy_End(&p));
   assert(!p.bypass && p.run == 0 && p.short_n == 0);
   assert(p.exits == 1);

   /* Reset keeps the interval diagnostics (gpu.c clears those with the rest
    * of the worker statistics) but drops the learned policy. */
   for (i = 0; i < GPU_STAGE_POLICY_TRIP; i++)
      burst(&p, 14);
   assert(p.bypass);
   burst(&p, 14);            /* the first batch actually retired by the bypass */
   assert(p.words == 14);
   GPU_StagePolicy_Reset(&p, STAGE_MAX);
   assert(!p.bypass && p.run == 0 && p.short_n == 0);
   assert(p.entries == 2 && p.exits == 1 && p.words == 14);
   checks++;
}

static void test_small_threshold_build(void)
{
   /* BEETLE_GPU_QUEUE_STRESS_TEST builds set GPU_STAGE_MAX to 4, so the
    * short-batch divisor must not collapse to zero and make every batch
    * "long". */
   gpu_stage_policy_t p = {0};
   unsigned i;

   GPU_StagePolicy_Reset(&p, 4);
   assert(GPU_StagePolicy_ShortBatch(&p) == 1);

   /* A batch of one word is still short at that threshold. */
   for (i = 0; i < GPU_STAGE_POLICY_TRIP - 1; i++)
   {
      GPU_StagePolicy_Resolved(&p, 0);
      assert(!p.bypass);
   }
   GPU_StagePolicy_Resolved(&p, 0);
   assert(p.bypass);

   /* Four uninterrupted words reach that build's publication threshold. */
   assert(!GPU_StagePolicy_Word(&p));
   assert(!GPU_StagePolicy_Word(&p));
   assert(!GPU_StagePolicy_Word(&p));
   assert(GPU_StagePolicy_Word(&p));
   checks++;
}

/* Replay of the two measured scenes, reporting the barrier-free share the
 * change is aiming at.  Counts only; no timing is claimed. */
static void report_measured_shapes(void)
{
   gpu_stage_policy_t p = {0};
   unsigned i;
   const unsigned plateau_polls = 1032;
   const unsigned plateau_words = 14357;
   unsigned per_poll = plateau_words / plateau_polls;   /* ~13 */

   GPU_StagePolicy_Reset(&p, STAGE_MAX);
   for (i = 0; i < plateau_polls * 3; i++)
   {
      if (p.bypass)
      {
         unsigned w;
         for (w = 0; w < per_poll; w++)
            if (GPU_StagePolicy_Word(&p))
               GPU_StagePolicy_End(&p);
         GPU_StagePolicy_Poll(&p);
      }
      else
         GPU_StagePolicy_Resolved(&p, per_poll);
   }

   printf("GPU staging: plateau shape (%u words / %u polls per frame) engages "
          "the bypass after %u short batches; %u of %u polls then answer "
          "without a barrier, %u entries / %u re-arms\n",
          plateau_words, plateau_polls, GPU_STAGE_POLICY_TRIP,
          p.polls, plateau_polls * 3, p.entries, p.exits);
   assert(p.entries == 1 && p.exits == 0);
   assert(p.polls > plateau_polls * 3 - GPU_STAGE_POLICY_TRIP - 1);
}

int main(void)
{
   test_poll_heavy_engages();
   test_streaming_never_engages();
   test_one_long_batch_resets_the_streak();
   test_streaming_re_arms_the_worker();
   test_drains_end_a_run();
   test_lifecycle();
   test_small_threshold_build();
   report_measured_shapes();

   printf("GPU staging: %u policy transitions over recorded batch shapes "
          "matched the engage/re-arm rules\n", checks);
   return 0;
}
