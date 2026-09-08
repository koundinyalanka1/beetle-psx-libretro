#include "../../mednafen/performance.h"

#if defined(_WIN32) && !defined(BEETLE_PERF_STANDALONE)
__declspec(dllexport)
#endif
int beetle_perf_selftest(void)
{
   static beetle_perf_histogram hist;
   unsigned i;
   hist.total_us = hist.min_us = hist.max_us = hist.count = 0;
   for (i = 0; i < BEETLE_PERF_BINS; i++)
      hist.bins[i] = 0;
   if (beetle_perf_percentile_upper(&hist, 95) != 0)
      return __LINE__;
   beetle_perf_add(&hist, 0);
   if (beetle_perf_percentile_upper(&hist, 95) != 0)
      return __LINE__;
   for (i = 1; i < 100; i++)
      beetle_perf_add(&hist, i * 250);
   if (hist.count != 100 || hist.total_us != 1237500 ||
         hist.min_us != 0 || hist.max_us != 24750)
      return __LINE__;
   if (beetle_perf_percentile_upper(&hist, 50) != 12499 ||
         beetle_perf_percentile_upper(&hist, 95) != 23749 ||
         beetle_perf_percentile_upper(&hist, 99) != 24749 ||
         beetle_perf_percentile_upper(&hist, 100) != 24750)
      return __LINE__;
   for (i = 0; i < 100; i++)
      beetle_perf_add(&hist, UINT64_C(10000000000));
   if (beetle_perf_percentile_upper(&hist, 95) != UINT64_C(10000000000) ||
         beetle_perf_percentile_upper(&hist, 101) != hist.max_us)
      return __LINE__;
   return 0;
}

#ifdef BEETLE_PERF_STANDALONE
int main(void)
{
   return beetle_perf_selftest();
}
#endif
