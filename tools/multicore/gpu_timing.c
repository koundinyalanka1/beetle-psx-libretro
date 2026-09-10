#include <assert.h>
#include <stdio.h>
#include "../../mednafen/psx/gpu_timing.h"

static int32_t reference(int32_t line, uint64_t fraction, int32_t ratio, int32_t quantum)
{
   int32_t delay = (((int64_t)line << 16) - fraction + ratio - 1) / ratio;
   if (delay < 1) delay = 1;
   if (delay > quantum) delay = quantum;
   return delay;
}

static void check_display_clocks(void)
{
   static const uint32_t ratios[5] = {10, 8, 5, 4, 7};
   static const uint32_t boundaries[] = {
      0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 342, 488, 608, 938, 2800,
      53203425, 53693182, INT32_MAX, (uint32_t)INT32_MIN, UINT32_MAX
   };
   unsigned mode, i;
   uint32_t random = 1;
   for (mode = 0; mode < 256; mode++)
   {
      uint32_t ratio = ratios[(mode & 0x40) ? 4 : (mode & 3)];
      assert(GPU_DisplayWidth(mode) == 2800 / ratio);
      for (i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++)
         assert(GPU_DisplayClocks(boundaries[i], mode) == boundaries[i] / ratio);
      /* Full 12-bit GP1 horizontal range, plus values used by cropping. */
      for (i = 0; i < 8192; i++)
         assert(GPU_DisplayClocks(i, mode) == i / ratio);
   }
   for (i = 0; i < 1000000; i++)
   {
      random = random * 1664525u + 1013904223u;
      for (mode = 0; mode < 5; mode++)
         assert(GPU_DisplayClocks(random, mode == 4 ? 0x40 : mode) ==
               random / ratios[mode]);
   }
}

int main(void)
{
   static const int32_t ratios[] = {1, 7, 32768, 51474, 65536, 102948, 103896, 207792, INT32_MAX};
   static const uint64_t fractions[] = {0, 1, 65535, UINT32_MAX, UINT64_MAX};
   static const int32_t quanta[] = {128, 256, 512, 1024, 2048};
   static const unsigned divisors[] = {10, 8, 5, 4};
   uint32_t random = 1;
   unsigned r, f, q, line, i, mode;
   check_display_clocks();
   for (r = 0; r < sizeof(ratios) / sizeof(ratios[0]); r++)
      for (f = 0; f < sizeof(fractions) / sizeof(fractions[0]); f++)
         for (q = 0; q < sizeof(quanta) / sizeof(quanta[0]); q++)
            for (line = 0; line <= 3413; line++)
               assert(GPU_NextEventDelay(line, fractions[f], ratios[r], quanta[q]) ==
                     reference(line, fractions[f], ratios[r], quanta[q]));
   for (i = 0; i < 1000000; i++)
   {
      random = random * 1664525u + 1013904223u;
      for (mode = 0; mode < 4; mode++)
         assert(GPU_DotClocks(random, mode) == random / divisors[mode]);
      line = random & 0x7fffffff;
      r = 1 + (random % 1000000);
      assert(GPU_NextEventDelay(line, random, r, 128) == reference(line, random, r, 128));
   }
   puts("GPU timing: event deadlines, timer and five-mode scanout division match reference across boundaries and randomized states");
   return 0;
}
