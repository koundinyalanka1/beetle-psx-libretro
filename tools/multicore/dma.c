/* Compare production DMA updates with the original unconditional channel walk.
 * Device stubs record ordered calls; production channel execution and RAM access
 * remain real. No BIOS, renderer, timing relaxation or device profiling needed. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "../../mednafen/psx/dma.c"

int32_t EventCycles = 128;
int32_t psx_overclock_factor;
bool psx_time_events;
uint64_t psx_dma_updates;
MultiAccessSizeMem *MainRAM;
static uint64_t trace;
static unsigned calls, ready_mask = 7;

static uint32_t record(unsigned kind, uint32_t a, uint32_t b)
{
   trace = (trace ^ kind) * UINT64_C(1099511628211);
   trace = (trace ^ a) * UINT64_C(1099511628211);
   trace = (trace ^ b) * UINT64_C(1099511628211);
   calls++;
   return (uint32_t)trace;
}
int32_t GPU_Update(int32_t ts) { record(1, ts, 0); return ts + 128; }
void MDEC_Run(int32_t clocks) { record(2, clocks, 0); }
bool MDEC_DMACanWrite(void) { record(3, 0, 0); return ready_mask & 1; }
bool MDEC_DMACanRead(void) { record(4, 0, 0); return ready_mask & 2; }
bool GPU_DMACanWrite(void) { record(5, 0, 0); return ready_mask & 4; }
void MDEC_DMAWrite(uint32_t v) { record(6, v, 0); }
uint32_t MDEC_DMARead(uint32_t *offs) { *offs = 1; return record(7, 0, 0); }
void GPU_WriteDMA(uint32_t v, uint32_t addr) { record(8, v, addr); }
uint32_t GPU_ReadDMA(void) { return record(9, 0, 0); }
uint32_t CDC_DMARead(void) { return record(10, 0, 0); }
void SPU_WriteDMA(uint32_t v) { record(11, v, 0); }
uint32_t SPU_ReadDMA(void) { return record(12, 0, 0); }
void IRQ_Assert(int which, bool value) { assert(which == IRQ_DMA); record(13, which, value); }
void PSX_SetDMACycleSteal(unsigned value) { record(14, value, 0); }
void CPU_SetHalt(bool value) { record(15, value, 0); }
void PSX_SetEventNT(int type, int32_t ts) { record(16, type, ts); }
/* Deadline the GPU event currently holds.  INT32_MIN keeps the differential
 * comparisons below on the original fixed grid; the sharing cases at the end
 * of main() set it explicitly. */
static int32_t gpu_event_ts = INT32_MIN;
int32_t PSX_PeekEventNT(int type) { assert(type == PSX_EVENT_GPU); return gpu_event_ts; }
void CPU_LightrecClear(uint32_t addr, uint32_t size) { record(17, addr, size); }

int MDFNSS_StateAction(void *data, int load, int data_only,
      SFORMAT *sf, const char *name)
{
   StateMem *sm = data;
   assert(!strcmp(name, "DMA"));
   for (; sf->v; sf++)
   {
      assert(sm->loc + sf->size <= sm->malloced);
      if (load) memcpy(sf->v, sm->data + sm->loc, sf->size);
      else memcpy(sm->data + sm->loc, sf->v, sf->size);
      sm->loc += sf->size;
   }
   return 1;
}

/* Original DMA_Update control flow: every channel enters RunChannel. */
static int32_t reference_update(int32_t timestamp)
{
   int32_t clocks = timestamp - lastts;
   int i;
   if (psx_time_events) psx_dma_updates++;
   overclock_cpu_to_device(&clocks);
   lastts = timestamp;
   GPU_Update(timestamp);
   MDEC_Run(clocks);
   for (i = 0; i < 7; i++) RunChannel(timestamp, clocks, i);
   DMACycleCounter -= clocks;
   while (DMACycleCounter <= 0) DMACycleCounter += EventCycles;
   RecalcHalt();
   return timestamp + CalcNextEvent(0x10000000);
}

typedef struct
{
   uint8_t serialized[512];
   int32_t timestamp;
   uint64_t trace, updates;
   unsigned calls;
} snapshot;

static void state(snapshot *s, bool load)
{
   StateMem sm = {0};
   sm.data = s->serialized;
   sm.malloced = sizeof(s->serialized);
   assert(DMA_StateAction(&sm, load, 0));
   if (load)
   {
      lastts = s->timestamp;
      trace = s->trace;
      calls = s->calls;
      psx_dma_updates = s->updates;
   }
   else
   {
      s->timestamp = lastts;
      s->trace = trace;
      s->calls = calls;
      s->updates = psx_dma_updates;
   }
}

static uint8_t initial_ram[0x200000], expected_ram[0x200000];
static unsigned comparisons, sharing_cases;
static snapshot expected_state, actual_state;
static void compare_update(int32_t timestamp)
{
   snapshot before = {0}, expected = {0}, actual = {0};
   int32_t deadline;
   state(&before, false);
   memcpy(initial_ram, MainRAM->data8, sizeof(initial_ram));
   deadline = reference_update(timestamp);
   state(&expected, false);
   memcpy(expected_ram, MainRAM->data8, sizeof(expected_ram));
   state(&before, true);
   memcpy(MainRAM->data8, initial_ram, sizeof(initial_ram));
   assert(DMA_Update(timestamp) == deadline);
   state(&actual, false);
   assert(!memcmp(actual.serialized, expected.serialized, sizeof(actual.serialized)));
   assert(actual.timestamp == expected.timestamp && actual.trace == expected.trace);
   assert(actual.calls == expected.calls && actual.updates == expected.updates);
   assert(!memcmp(MainRAM->data8, expected_ram, sizeof(expected_ram)));
   comparisons++;
}

static void reset(void)
{
   DMA_Power();
   memset(MainRAM->data8, 0, MainRAM->size);
   trace = 0;
   calls = 0;
   psx_dma_updates = 0;
}

int main(void)
{
   static const int32_t debts[] = {INT32_MIN, -2048, -129, -1, 0, 1, 128, INT32_MAX};
   static const int32_t factors[] = {0, 256, 384, 512};
   uint32_t random = 1;
   unsigned i, ch, mask, f;
   MainRAM = MultiAccessSizeMem_New(0x200000);
   assert(MainRAM);
   /* Idle credit/debt, zero elapsed updates, and both diagnostics states. */
   for (f = 0; f < sizeof(factors) / sizeof(factors[0]); f++)
      for (i = 0; i < sizeof(debts) / sizeof(debts[0]); i++)
      {
         psx_overclock_factor = factors[f];
         psx_time_events = i & 1;
         reset();
         for (ch = 0; ch < 7; ch++) DMACH[ch].ClockCounter = debts[i];
         compare_update(128);
         compare_update(128);
         compare_update(4096);
      }
   psx_overclock_factor = 0;
   /* Every active-channel combination, ready and backpressured devices,
    * both directions and partially consumed blocks with busy already clear. */
   for (mask = 0; mask < 128; mask++)
      for (i = 0; i < 4; i++)
      {
         reset();
         ready_mask = (i & 1) ? 7 : 0;
         DMAIntControl = 0x00ff0000;
         for (ch = 0; ch < 7; ch++)
         {
            random = random * 1664525u + 1013904223u;
            DMACH[ch].BaseAddr = DMACH[ch].CurAddr = 0x1000 + 0x100 * ch;
            DMACH[ch].BlockControl = 0x00010004;
            DMACH[ch].ChanControl = (random & 1) | ((i & 2) ? 0x100 : 0);
            if (mask & (1u << ch))
            {
               DMACH[ch].ChanControl |= 0x11000000;
               if (i & 2) DMACH[ch].ChanControl &= ~(1u << 24);
               DMACH[ch].WordCounter = (i & 2) ? 3 : 0;
            }
            DMACH[ch].ClockCounter = -(int32_t)(random & 127);
         }
         compare_update(0);
         compare_update(128);
         ready_mask = 7;
         compare_update(256);
      }
   /* Real register writes start/stop DMA; linked-list end markers and IRQs. */
   for (ch = 0; ch < 7; ch++)
   {
      reset();
      DMA_Write(0, 0x74, 0x00ff0000);
      DMA_Write(0, ch * 16, 0x1000);
      DMA_Write(0, ch * 16 + 4, 0x00010008);
      DMA_Write(0, ch * 16 + 8, 0x11000000 | (ch == 6 ? 2 : 1));
      compare_update(128);
      DMA_Write(128, ch * 16 + 8, 0);
      compare_update(256);
      DMA_ResetTS();
      compare_update(0);
   }
   for (i = 0; i < 3; i++)
   {
      reset();
      DMAIntControl = 0x00ff0000;
      DMACH[2].ChanControl = 0x01000401;
      DMACH[2].BaseAddr = i == 0 ? 0xffffff : (i == 1 ? 0x800000 : 0x1000);
      MASMEM_WriteU32(MainRAM, 0x1000, 0x02ffffff);
      compare_update(128);
      compare_update(256);
   }
   /* Idle-DMA deadline sharing.
    *
    * The emulated outcome must be byte-identical to the original walk; only
    * the returned deadline may differ.  The stub GPU returns ts + 128 from
    * GPU_Update(), so the adopted deadline is the armed entry while that is
    * still in the future and the freshly computed one otherwise.  Either way
    * the controller is never left unserviced for longer than one quantum. */
   {
      static const int32_t peeks[] = {
         INT32_MIN, -1, 0, 1, 17, 64, 65, 128, 129, 192, 193, 256,
         PSX_EVENT_MAXTS, INT32_MAX
      };
      unsigned p;
      for (p = 0; p < sizeof(peeks) / sizeof(peeks[0]); p++)
         for (i = 0; i < 2; i++)
         {
            int32_t at, base_deadline, actual, expected, armed, shared;
            int32_t offset = i ? 4096 : 0;
            snapshot before = {0};

            reset();
            gpu_event_ts = INT32_MIN;
            assert(DMA_Update(offset) == offset + 128);
            at = offset + 64;
            state(&before, false);

            base_deadline = DMA_Update(at);
            assert(base_deadline == offset + 128);   /* original grid point */
            state(&expected_state, false);

            state(&before, true);
            gpu_event_ts = peeks[p];
            actual = DMA_Update(at);
            state(&actual_state, false);

            /* State, RAM and the ordered device/IRQ trace never depend on the
             * peeked deadline. */
            assert(!memcmp(actual_state.serialized, expected_state.serialized,
                     sizeof(actual_state.serialized)));
            assert(actual_state.trace == expected_state.trace);
            assert(actual_state.calls == expected_state.calls);

            /* Mirror of DMA_IdleShareDeadline: prefer the armed entry while
             * it is still in the future, otherwise the deadline GPU_Update()
             * just returned (the stub's ts + 128), and fall back to the grid
             * when neither lands inside the quantum. */
            armed    = peeks[p] - at;
            shared   = (armed > 0) ? armed : 128;
            expected = (shared > 0 && shared <= EventCycles)
               ? at + shared : base_deadline;
            assert(actual == expected);
            /* Never left unserviced for longer than one quantum, and never
             * earlier than the update that produced it. */
            assert(actual > at && actual - at <= EventCycles);
            sharing_cases++;
         }
   }

   /* An active channel keeps the original grid whatever the GPU holds, so
    * RunChannel's credit-discard behaviour is never subdivided. */
   for (ch = 0; ch < 7; ch++)
      for (i = 0; i < 2; i++)
      {
         int32_t deadline;
         reset();
         gpu_event_ts = 64 + 100;   /* inside the quantum, so it would be taken */
         DMAIntControl = 0x00ff0000;
         DMACH[ch].BaseAddr = DMACH[ch].CurAddr = 0x1000;
         DMACH[ch].BlockControl = 0x00081000;
         DMACH[ch].ChanControl = 0x01000201;
         /* i == 0: busy with a block in flight.  i == 1: forced stop cleared
          * busy but left the block partially consumed. */
         DMACH[ch].WordCounter = 0x1000;
         if (i)
            DMACH[ch].ChanControl &= ~(1u << 24);
         deadline = DMA_Update(64);
         assert(DMACH[ch].WordCounter || (DMACH[ch].ChanControl & (1u << 24)));
         assert(deadline == 64 + CalcNextEvent(0x10000000));
         assert(deadline != gpu_event_ts);
         sharing_cases++;
      }
   gpu_event_ts = INT32_MIN;

   MultiAccessSizeMem_Free(MainRAM);
   printf("DMA: %u differential updates matched state, RAM, deadlines and device/IRQ/invalidation ordering\n", comparisons);
   printf("DMA: %u idle-deadline-sharing cases kept state/trace identical, stayed inside one quantum, and left active channels on the original grid\n", sharing_cases);
   return 0;
}
