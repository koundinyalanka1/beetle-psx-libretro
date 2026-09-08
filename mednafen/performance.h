#ifndef BEETLE_PERFORMANCE_H
#define BEETLE_PERFORMANCE_H

#include <stdint.h>

#define BEETLE_PERF_BINS 256
#define BEETLE_PERF_BIN_US 250

typedef struct
{
   uint64_t total_us;
   uint64_t min_us;
   uint64_t max_us;
   uint64_t count;
   uint64_t bins[BEETLE_PERF_BINS];
} beetle_perf_histogram;

static inline void beetle_perf_add(beetle_perf_histogram *hist, uint64_t us)
{
   unsigned bin = us / BEETLE_PERF_BIN_US < BEETLE_PERF_BINS ?
      (unsigned)(us / BEETLE_PERF_BIN_US) : BEETLE_PERF_BINS - 1;
   if (!hist->count || us < hist->min_us)
      hist->min_us = us;
   if (us > hist->max_us)
      hist->max_us = us;
   hist->total_us += us;
   hist->count++;
   hist->bins[bin]++;
}

static inline uint64_t beetle_perf_percentile_upper(
      const beetle_perf_histogram *hist, unsigned percentile)
{
   uint64_t rank, count = 0;
   unsigned i;
   if (!hist->count)
      return 0;
   if (!percentile)
      return hist->min_us;
   if (percentile >= 100)
      return hist->max_us;
   rank = hist->count / 100 * percentile +
      ((hist->count % 100) * percentile + 99) / 100;
   for (i = 0; i < BEETLE_PERF_BINS; i++)
   {
      uint64_t upper;
      count += hist->bins[i];
      if (count < rank)
         continue;
      upper = (uint64_t)(i + 1) * BEETLE_PERF_BIN_US - 1;
      return i == BEETLE_PERF_BINS - 1 || upper > hist->max_us ?
         hist->max_us : upper;
   }
   return hist->max_us;
}

#endif
