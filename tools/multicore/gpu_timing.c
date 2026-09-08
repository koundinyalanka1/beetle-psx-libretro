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

int main(void)
{
   static const int32_t ratios[] = {1, 7, 32768, 51474, 65536, 102948, 103896, 207792, INT32_MAX};
   static const uint64_t fractions[] = {0, 1, 65535, UINT32_MAX, UINT64_MAX};
   static const int32_t quanta[] = {128, 256, 512, 1024, 2048};
   static const unsigned divisors[] = {10, 8, 5, 4};
   uint32_t random = 1;
   unsigned r, f, q, line, i, mode;
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
   puts("GPU timing: event deadlines and dot-clock division match reference across boundaries and randomized states");
   return 0;
}
