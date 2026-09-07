#ifndef MULTICORE_TESTROM_REPORT_H
#define MULTICORE_TESTROM_REPORT_H

/* Protocol emitted by make_gpu_test.py, at physical RAM 0x1f0000. */
static unsigned testrom_u32(const unsigned char *p)
{
   return (unsigned)p[0] | (unsigned)p[1] << 8 |
          (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

static int testrom_check_report(const unsigned char *ram, size_t len)
{
   const unsigned char *p;
   unsigned cases, done, words, failures;
   if (!ram || len < 0x1f0028)
   { fprintf(stderr, "[testrom] main RAM unavailable\n"); return 1; }
   p = ram + 0x1f0000;
   if (testrom_u32(p) != 0x54555047)
   { fprintf(stderr, "[testrom] ROM did not start (report magic missing)\n"); return 1; }
   done = testrom_u32(p + 4);
   cases = testrom_u32(p + 8);
   words = testrom_u32(p + 12);
   failures = testrom_u32(p + 16);
   fprintf(stderr, "[testrom] %s: %u cases, %u words checked, %u mismatches\n",
         done == 1 && !failures ? "PASS" : "FAIL", cases, words, failures);
   if (failures)
      fprintf(stderr, "[testrom] first mismatch case=%u word=%u actual=%08x expected=%08x\n",
            testrom_u32(p + 20), testrom_u32(p + 24),
            testrom_u32(p + 28), testrom_u32(p + 32));
   if (done != 1) fprintf(stderr, "[testrom] ROM did not finish\n");
   return done != 1 || failures != 0 || !cases || !words;
}
#endif
