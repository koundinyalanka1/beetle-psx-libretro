#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "thread_faults.h"
#include "../../mednafen/psx/spu.c"

uint8_t spu_samples = 1;
bool psx_spu_silent_voice_opt = false;

static pthread_t emulation_thread;
static unsigned cd_sample_count, emulated_tick;
static uint32_t cd_epoch;
static uint64_t irq_hash;

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
   const unsigned char *p = data;
   while (size--)
      hash = (hash ^ *p++) * UINT64_C(1099511628211);
   return hash;
}

retro_time_t cpu_features_get_time_usec(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (retro_time_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

void CDC_GetCDAudioSample(int32_t samples[2])
{
   assert(pthread_equal(pthread_self(), emulation_thread));
   samples[0] = (int16_t)(cd_epoch + cd_sample_count * 197);
   samples[1] = (int16_t)(cd_epoch ^ (cd_sample_count * 251));
   cd_sample_count++;
}

void IRQ_Assert(int which, bool asserted)
{
   assert(pthread_equal(pthread_self(), emulation_thread));
   assert(which == IRQ_SPU);
   irq_hash = hash_bytes(irq_hash, &emulated_tick, sizeof(emulated_tick));
   irq_hash = hash_bytes(irq_hash, &asserted, sizeof(asserted));
}

/* Raw same-host serialization through the production SFORMAT table. */
int MDFNSS_StateAction(void *data, int load, int data_only,
      SFORMAT *sf, const char *name)
{
   StateMem *sm = data;
   (void)data_only;
   assert(!strcmp(name, "SPU"));
   for (; sf->v; sf++)
   {
      assert(sm->loc + sf->size <= sm->malloced);
      if (load)
         memcpy(sf->v, sm->data + sm->loc, sf->size);
      else
         memcpy(sm->data + sm->loc, sf->v, sf->size);
      sm->loc += sf->size;
   }
   return 1;
}

static void advance(unsigned ticks)
{
   while (ticks--)
   {
      unsigned before = cd_sample_count;
      emulated_tick++;
      cd_epoch = cd_epoch * 1664525u + 1013904223u;
      assert(SPU_UpdateFromCDC(768) > 0);
      /* CDC consumption must happen before UpdateFromCDC returns. */
      if (spu_samples == 1 || spu_samples == 0)
         assert(cd_sample_count == before + 1);
   }
}

#define FRAMES 48
static void run_scenario(bool threaded, uint8_t samples, uint64_t hashes[FRAMES])
{
   unsigned frame, voice;
   uint8_t state[600000];
   StateMem sm = {0};
   sm.data = state;
   sm.malloced = sizeof(state);
   SPU_Init();
   SPU_Power();
   spu_samples = samples;
   SPU_SetThreaded(threaded);
   SPU_Worker_Refresh();
   assert(SPU_Worker_Active() == threaded);
   cd_sample_count = emulated_tick = 0;
   cd_epoch = 0x12345678;
   irq_hash = 0;

   /* Exercise voice decoding, envelopes, pitch modulation, capture and reverb. */
   SPU_Write(0, 0x1A6, 0x1000);
   for (voice = 0; voice < 256; voice++)
      SPU_WriteDMA(voice % 4 ? 0x12345678u + voice : 0x73120300u);
   SPU_Write(0, 0x180, 0x3FFF);
   SPU_Write(0, 0x182, 0x3FFF);
   SPU_Write(0, 0x1B0, 0x6000);
   SPU_Write(0, 0x1B2, 0x7000);
   SPU_Write(0, 0x184, 0x1800);
   SPU_Write(0, 0x186, 0x2800);
   SPU_Write(0, 0x198, 0xFFFF);
   SPU_Write(0, 0x1A2, 0x8000);
   for (voice = 0; voice < 24; voice++)
   {
      SPU_Write(0, voice * 16, 0x3000);
      SPU_Write(0, voice * 16 + 2, 0x3800);
      SPU_Write(0, voice * 16 + 4, 0x0800 + voice * 53);
      SPU_Write(0, voice * 16 + 6, 0x1000);
      SPU_Write(0, voice * 16 + 8, 0x00FF);
      SPU_Write(0, voice * 16 + 10, 0x4000);
   }
   SPU_Write(0, 0x188, 0xFFFF);
   SPU_Write(0, 0x18A, 0x00FF);
   SPU_Write(0, 0x1AA, 0xC085);

   for (frame = 0; frame < FRAMES; frame++)
   {
      uint64_t hash = 0;
      advance(113);
      SPU_Write(0, 0x1A6, 0x1000 + frame);
      SPU_WriteDMA(0x89ABCDEFu ^ frame);
      advance(137);
      (void)SPU_Read(0, 0x1AE);
      (void)SPU_ReadDMA();
      if (frame % 4 == 0)
      {
         /* This IRQ address will be visited by the capture buffer shortly. */
         SPU_Write(0, 0x1A4, ((CWA + 8) & 0x1FC) >> 2);
         SPU_Write(0, 0x1AA, 0xC0C5);
      }
      advance(127);
      SPU_Write(0, 0x1AA, 0xC085);
      advance(167);
      if (frame % 7 == 0)
      {
         sm.loc = 0;
         assert(SPU_StateAction(&sm, 0, 1));
         advance(5);
         sm.loc = 0;
         assert(SPU_StateAction(&sm, 1, 1));
      }
      advance(191);
      if (threaded && frame % 8 == 0)
      {
         /* Disable with a partial batch outstanding, then resume threading. */
         SPU_SetThreaded(false);
         SPU_Worker_Refresh();
         SPU_SetThreaded(true);
         SPU_Worker_Refresh();
      }
      sm.loc = 0;
      assert(SPU_StateAction(&sm, 0, 1));
      hash = hash_bytes(hash, state, sm.loc);
      hash = hash_bytes(hash, IntermediateBuffer,
            IntermediateBufferPos * sizeof(IntermediateBuffer[0]));
      hash = hash_bytes(hash, &irq_hash, sizeof(irq_hash));
      hashes[frame] = hash_bytes(hash, &cd_sample_count, sizeof(cd_sample_count));
      IntermediateBufferPos = 0;
   }
   SPU_Kill();
}

int main(void)
{
   unsigned i, mode;
   const uint8_t sample_modes[] = {1, 4, 16, 0};
   uint64_t serial[FRAMES], threaded[FRAMES];
   uint32_t jobs;
   emulation_thread = pthread_self();

   for (i = 1; i <= 4; i++)
   {
      allocation_index = 0;
      fail_allocation = i;
      SPU_Worker_Init();
      assert(!SPU_Worker_Active());
      SPU_Worker_Kill();
      assert(!live_locks && !live_conds && !live_threads);
   }
   fail_allocation = 0;

   for (mode = 0; mode < sizeof(sample_modes); mode++)
   {
      run_scenario(false, sample_modes[mode], serial);
      run_scenario(true, sample_modes[mode], threaded);
      for (i = 0; i < FRAMES; i++)
         assert(serial[i] == threaded[i]);
   }

   SPU_Init();
   SPU_Power();
   spu_samples = 1;
   SPU_Worker_Init();
   SPU_Worker_TakeStats(NULL, NULL, NULL);
   advance(735);
   SPU_Worker_Sync();
   SPU_Worker_TakeStats(NULL, NULL, &jobs);
   assert(jobs == 23);
   SPU_Kill();
   assert(!live_locks && !live_conds && !live_threads);
   puts("SPU: matching state/audio/IRQ traces, CDC thread ownership, batching, lifecycle and allocation failures passed");
   return 0;
}
