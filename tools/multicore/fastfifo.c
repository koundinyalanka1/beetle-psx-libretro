/* Differential command reads, including every ring boundary and occupancy. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../mednafen/psx/FastFIFO.h"

int main(void)
{
   unsigned pos, available, count, cases = 0;
   for (pos = 0; pos < FASTFIFO_SIZE; pos++)
      for (available = 0; available <= FASTFIFO_SIZE; available++)
         for (count = 0; count <= available; count++)
         {
            FastFIFO scalar, batch;
            uint32_t expected[FASTFIFO_SIZE + 2];
            uint32_t actual[FASTFIFO_SIZE + 2];
            unsigned i;
            FastFIFO_Init(&scalar);
            scalar.read_pos = scalar.write_pos = pos;
            for (i = 0; i < available; i++)
               FastFIFO_Write(&scalar, 0x12345678u ^ (i * 0x9e3779b9u));
            batch = scalar;
            memset(expected, 0xa5, sizeof(expected));
            memset(actual, 0xa5, sizeof(actual));
            for (i = 0; i < count; i++)
               expected[i + 1] = FastFIFO_Read(&scalar);
            FastFIFO_ReadMany(&batch, actual + 1, count);
            assert(!memcmp(actual, expected, sizeof(actual)));
            assert(!memcmp(&batch, &scalar, sizeof(batch)));

            /* Refill after consuming, then drain: catches incorrect indices
             * even when a read's output happened to match at a wrap. */
            for (i = 0; i < count; i++)
            {
               FastFIFO_Write(&scalar, i ^ 0xdeadbeefu);
               FastFIFO_Write(&batch, i ^ 0xdeadbeefu);
            }
            for (i = 0; i < available; i++)
               expected[i] = FastFIFO_Read(&scalar);
            FastFIFO_ReadMany(&batch, actual, available);
            assert(!memcmp(actual, expected, available * sizeof(*actual)));
            assert(!memcmp(&batch, &scalar, sizeof(batch)));
            cases++;
         }
   printf("FastFIFO: %u differential read/refill cases passed\n", cases);
   return 0;
}
