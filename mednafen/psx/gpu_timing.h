#ifndef BEETLE_GPU_TIMING_H
#define BEETLE_GPU_TIMING_H
#include <stdint.h>
#include <limits.h>

static inline uint32_t GPU_DotClocks(uint32_t clocks, unsigned mode)
{
   /* Constant divisors avoid the ARMv7 software divide used for a table
    * lookup divisor. Keep the existing four-mode timer interpretation. */
   switch (mode & 3)
   {
      case 0: return clocks / 10;
      case 1: return clocks / 8;
      case 2: return clocks / 5;
      default: return clocks / 4;
   }
}

static inline int32_t GPU_NextEventDelay(int32_t line_clocks,
      uint64_t fractional_clocks, int32_t ratio, int32_t quantum)
{
   uint64_t numerator = ((int64_t)line_clocks * 65536) - fractional_clocks + ratio - 1;
   int32_t delay;

   /* Most updates are far from a scanline boundary and clamp to the event
    * quantum anyway. Avoid a 64-bit divide without changing that deadline.
    * The bound preserves the old int32_t conversion on unusual state values. */
   if (ratio > 0 && quantum > 0 && numerator <= INT32_MAX &&
         numerator >= (uint64_t)(uint32_t)quantum * (uint32_t)ratio)
      return quantum;

   if (ratio > 0 && numerator <= UINT32_MAX)
      delay = (int32_t)((uint32_t)numerator / (uint32_t)ratio);
   else
      delay = (int32_t)(numerator / ratio);
   if (delay < 1) delay = 1;
   if (delay > quantum) delay = quantum;
   return delay;
}
#endif
