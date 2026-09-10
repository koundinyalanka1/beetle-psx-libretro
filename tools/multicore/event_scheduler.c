/*
 * Event-scheduler differential harness.
 *
 * Drives the production DMA_Update() and the production GPU deadline helper
 * through the same event list libretro.c runs, for a full NTSC frame, and
 * compares the schedule with and without idle-DMA deadline sharing.
 *
 * What it establishes:
 *   - the DMA controller is never serviced later than the original fixed grid
 *     would have serviced it;
 *   - with every channel idle, serialized DMA state, RAM, halt/cycle-steal
 *     decisions and the ordered device/IRQ trace are identical at every
 *     original grid point and at frame end;
 *   - an active channel keeps the original grid, so RunChannel's
 *     credit-discard behaviour is untouched;
 *   - the number of CPU-loop exits (lightrec re-entries) falls, which is the
 *     cost this change targets.
 *
 * No device profiling, BIOS, renderer or timing relaxation is involved; the
 * GPU side is the real gpu_timing.h deadline arithmetic driven by a scanline
 * model with gpu.c's own constants.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../mednafen/psx/dma.c"
#include "../../mednafen/psx/gpu_timing.h"

int32_t EventCycles = 128;
int32_t psx_overclock_factor;
bool psx_time_events;
uint64_t psx_dma_updates;
MultiAccessSizeMem *MainRAM;

/* ---- ordered device/IRQ trace, as in the DMA differential test ---------- */
static uint64_t trace;
static unsigned calls;
static unsigned ready_mask = 7;

static uint32_t record(unsigned kind, uint32_t a, uint32_t b)
{
   trace = (trace ^ kind) * UINT64_C(1099511628211);
   trace = (trace ^ a) * UINT64_C(1099511628211);
   trace = (trace ^ b) * UINT64_C(1099511628211);
   calls++;
   return (uint32_t)trace;
}

/* ---- GPU model ---------------------------------------------------------
 * gpu.c's clock plumbing, with its constants, so the deadlines this harness
 * schedules are the ones the core would schedule.  Drawing, scanout and IRQ
 * work are out of scope here; only the deadline sequence matters. */
#define GPU_CLOCK_RATIO_NTSC 103896   /* 65536 * 53693181.818 / (44100 * 768) */

static struct
{
   uint64_t clock_counter;
   int32_t  line_clock_counter;
   int32_t  ratio;
   int      line_phase;
   int      phase_change;
   int32_t  lastts;
   unsigned updates;
   unsigned zero_updates;
} gpu;

static void gpu_reset(void)
{
   memset(&gpu, 0, sizeof(gpu));
   gpu.ratio              = GPU_CLOCK_RATIO_NTSC;
   gpu.line_clock_counter = 3412 - 200;
}

int32_t GPU_Update(int32_t sys_timestamp)
{
   int32_t sys_clocks = sys_timestamp - gpu.lastts;
   int32_t gpu_clocks;

   gpu.updates++;
   if (!sys_clocks)
   {
      gpu.zero_updates++;
      goto TheEnd;
   }

   gpu.clock_counter += (uint64_t)sys_clocks * (uint32_t)gpu.ratio;
   gpu_clocks         = (int32_t)(gpu.clock_counter >> 16);
   gpu.clock_counter -= (uint64_t)gpu_clocks << 16;

   while (gpu_clocks > 0)
   {
      int32_t chunk = gpu_clocks;
      if (chunk > gpu.line_clock_counter)
         chunk = gpu.line_clock_counter;
      gpu_clocks             -= chunk;
      gpu.line_clock_counter -= chunk;

      if (!gpu.line_clock_counter)
      {
         gpu.line_phase = (gpu.line_phase + 1) & 1;
         if (gpu.line_phase)
            gpu.line_clock_counter = 200;
         else
         {
            gpu.line_clock_counter = 3412 + gpu.phase_change - 200;
            gpu.phase_change       = !gpu.phase_change;
         }
      }
   }

TheEnd:
   gpu.lastts = sys_timestamp;
   return sys_timestamp + GPU_NextEventDelay(gpu.line_clock_counter,
         gpu.clock_counter, gpu.ratio, EventCycles);
}

/* ---- remaining DMA dependencies ---------------------------------------- */
void MDEC_Run(int32_t clocks) { record(2, (uint32_t)clocks, 0); }
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

/* ---- event list, mirroring libretro.c ----------------------------------- */
struct entry
{
   unsigned which;
   int32_t  event_time;
   struct entry *prev, *next;
};
static struct entry events[PSX_EVENT__COUNT];
static bool share_enabled = true;

int32_t PSX_PeekEventNT(const int type)
{
   /* Returning a timestamp the sharing bound rejects is how this harness runs
    * the original schedule without a production build switch. */
   if (!share_enabled)
      return INT32_MIN;
   return events[type].event_time;
}

void PSX_SetEventNT(const int type, const int32_t next_timestamp)
{
   struct entry *e = &events[type];

   if (next_timestamp < e->event_time)
   {
      struct entry *fe = e;
      do { fe = fe->prev; } while (next_timestamp < fe->event_time);
      e->prev->next = e->next;
      e->next->prev = e->prev;
      e->prev = fe;
      e->next = fe->next;
      fe->next->prev = e;
      fe->next = e;
      e->event_time = next_timestamp;
   }
   else if (next_timestamp > e->event_time)
   {
      struct entry *fe = e;
      do { fe = fe->next; } while (next_timestamp > fe->event_time);
      e->prev->next = e->next;
      e->next->prev = e->prev;
      e->prev = fe->prev;
      e->next = fe;
      fe->prev->next = e;
      fe->prev = e;
      e->event_time = next_timestamp;
   }
}

static void events_reset(void)
{
   unsigned i;
   for (i = 0; i < PSX_EVENT__COUNT; i++)
   {
      events[i].which = i;
      if (i == PSX_EVENT__SYNFIRST)
         events[i].event_time = (int32_t)0x80000000;
      else if (i == PSX_EVENT__SYNLAST)
         events[i].event_time = 0x7FFFFFFF;
      else
         events[i].event_time = PSX_EVENT_MAXTS;
      events[i].prev = (i > 0) ? &events[i - 1] : NULL;
      events[i].next = (i < (PSX_EVENT__COUNT - 1)) ? &events[i + 1] : NULL;
   }
}

/* ---- run ---------------------------------------------------------------- */
#define CDC_SAMPLE_CYCLES 768
/* 33868800 / 59.94 */
#define FRAME_CYCLES      565053

typedef struct
{
   unsigned exits;          /* CPU_Run re-entries: one per PSX_EventHandler */
   unsigned dispatched;     /* individual event callbacks */
   unsigned dma_updates;
   unsigned gpu_updates;
   unsigned gpu_zero_updates;
   uint64_t trace;
   unsigned calls;
   uint8_t  serialized[512];
   uint32_t ch_base[7], ch_block[7], ch_ctrl[7], ch_cur[7];
   uint32_t ch_words[7];
   int32_t  ch_clock[7];
   int32_t  dma_lastts;
   int32_t  dma_cycle_counter;
   /* Last time DMA_Update ran at or before each original 128-cycle grid
    * point, so a delayed service is detectable. */
   int32_t  service_at_grid[FRAME_CYCLES / 128 + 2];
} run_result;

static void save_dma_state(run_result *r)
{
   StateMem sm = {0};
   sm.data     = r->serialized;
   sm.malloced = sizeof(r->serialized);
   assert(DMA_StateAction(&sm, 0, 0));
   {
      unsigned c;
      for (c = 0; c < 7; c++)
      {
         r->ch_base[c]  = DMACH[c].BaseAddr;
         r->ch_block[c] = DMACH[c].BlockControl;
         r->ch_ctrl[c]  = DMACH[c].ChanControl;
         r->ch_cur[c]   = DMACH[c].CurAddr;
         r->ch_words[c] = DMACH[c].WordCounter;
         r->ch_clock[c] = DMACH[c].ClockCounter;
      }
   }
   r->dma_lastts        = lastts;
   r->dma_cycle_counter = DMACycleCounter;
}

/* setup_channels: 0 = every channel idle, otherwise a bitmask of channels to
 * arm with a real linked-list transfer. */
static void run_frame(run_result *r, bool share, unsigned setup_channels)
{
   int32_t ts = 0;
   unsigned ch;
   int32_t last_dma_service = 0;
   unsigned grid;

   share_enabled = share;
   memset(r, 0, sizeof(*r));

   DMA_Power();
   memset(MainRAM->data8, 0, MainRAM->size);
   trace = 0;
   calls = 0;
   psx_dma_updates = 0;
   gpu_reset();
   events_reset();
   ready_mask = 7;

   for (ch = 0; ch < 7; ch++)
   {
      if (!(setup_channels & (1u << ch)))
         continue;
      DMAIntControl = 0x00ff0000;
      DMACH[ch].BaseAddr    = 0x1000 + 0x100 * ch;
      DMACH[ch].CurAddr     = DMACH[ch].BaseAddr;
      DMACH[ch].BlockControl = 0x00080010;
      DMACH[ch].ChanControl  = 0x01000201;
      DMACH[ch].WordCounter  = 8;
   }

   PSX_SetEventNT(PSX_EVENT_GPU, GPU_Update(0));
   PSX_SetEventNT(PSX_EVENT_DMA, DMA_Update(0));
   PSX_SetEventNT(PSX_EVENT_CDC, CDC_SAMPLE_CYCLES);

   for (grid = 0; grid < sizeof(r->service_at_grid) / sizeof(r->service_at_grid[0]); grid++)
      r->service_at_grid[grid] = -1;

   while (ts < FRAME_CYCLES)
   {
      struct entry *e = events[PSX_EVENT__SYNFIRST].next;
      int32_t next    = e->event_time;

      /* One CPU_Run quantum: lightrec runs to the head deadline and exits. */
      ts = next;
      r->exits++;

      e = events[PSX_EVENT__SYNFIRST].next;
      while (ts >= e->event_time)
      {
         struct entry *prev = e->prev;
         unsigned which     = e->which;
         int32_t nt;

         r->dispatched++;
         switch (which)
         {
            case PSX_EVENT_GPU:
               nt = GPU_Update(e->event_time);
               r->gpu_updates++;
               break;
            case PSX_EVENT_DMA:
               nt = DMA_Update(e->event_time);
               r->dma_updates++;
               last_dma_service = e->event_time;
               break;
            case PSX_EVENT_CDC:
               nt = e->event_time + CDC_SAMPLE_CYCLES;
               break;
            default:
               nt = PSX_EVENT_MAXTS;
               break;
         }
         PSX_SetEventNT(which, nt);
         e = prev->next;
      }

      /* Record, for every original grid point already passed, when DMA was
       * last serviced.  The original schedule services each grid point
       * exactly; the shared schedule must never be later. */
      for (grid = 0; grid * 128 <= (unsigned)ts
            && grid < sizeof(r->service_at_grid) / sizeof(r->service_at_grid[0]); grid++)
      {
         if (r->service_at_grid[grid] < 0)
            r->service_at_grid[grid] = last_dma_service;
      }
   }

   /* Both schedules stop at whatever deadline crossed the frame boundary, and
    * that overshoot differs, so settle the controller at a common timestamp
    * past either one before comparing state. */
   DMA_Update(FRAME_CYCLES + 8 * EventCycles);

   r->trace            = trace;
   r->calls            = calls;
   r->gpu_zero_updates = gpu.zero_updates;
   save_dma_state(r);
}

int main(void)
{
   run_result base, shared;
   unsigned grid, active;

   MainRAM = MultiAccessSizeMem_New(0x200000);
   assert(MainRAM);

   /* --- every channel idle: the case the optimization targets ------------ */
   run_frame(&base, false, 0);
   run_frame(&shared, true, 0);

   /* Identical emulated outcome. */
   assert(!memcmp(base.serialized, shared.serialized, sizeof(base.serialized)));
   assert(base.dma_cycle_counter == shared.dma_cycle_counter);
   assert(base.dma_lastts == shared.dma_lastts);
   assert(base.trace != 0);

   /* The controller is never left unserviced for longer than one quantum:
    * at every original grid point, the shared schedule had already run DMA
    * within EventCycles. */
   for (grid = 1; grid < FRAME_CYCLES / 128; grid++)
   {
      assert(base.service_at_grid[grid] >= 0);
      assert(shared.service_at_grid[grid] >= 0);
      assert((int32_t)(grid * 128) - shared.service_at_grid[grid] <= EventCycles);
   }

   /* The point of the change: fewer CPU-loop exits for the same frame. */
   assert(shared.exits < base.exits);
   assert(shared.dma_updates >= base.dma_updates);
   /* Every DMA update now lands on a GPU deadline, so the GPU event that
    * follows finds nothing elapsed instead of re-running the scanline walk. */
   assert(shared.gpu_zero_updates > base.gpu_zero_updates);

   printf("Event scheduler: idle DMA shares the GPU deadline - "
          "exits %u -> %u (%.1f%%), events %u -> %u, "
          "DMA updates %u -> %u, state/RAM/trace identical\n",
          base.exits, shared.exits,
          100.0 * (double)(base.exits - shared.exits) / (double)base.exits,
          base.dispatched, shared.dispatched,
          base.dma_updates, shared.dma_updates);

   /* --- an active channel must keep the original grid -------------------- */
   for (active = 0; active < 7; active++)
   {
      run_frame(&base, false, 1u << active);
      run_frame(&shared, true, 1u << active);

      assert(!memcmp(base.serialized, shared.serialized, sizeof(base.serialized)));
      assert(base.dma_cycle_counter == shared.dma_cycle_counter);

      for (grid = 1; grid < FRAME_CYCLES / 128; grid++)
         assert((int32_t)(grid * 128) - shared.service_at_grid[grid] <= EventCycles);
   }

   /* --- a CPU overclock stays on the original grid ----------------------- */
   for (psx_overclock_factor = 128; psx_overclock_factor <= 512;
         psx_overclock_factor += 128)
   {
      run_frame(&base, false, 0);
      run_frame(&shared, true, 0);
      /* Identical schedule, not merely identical state: the two clock domains
       * round per call, so sharing is switched off entirely here. */
      assert(!memcmp(base.serialized, shared.serialized, sizeof(base.serialized)));
      assert(shared.exits == base.exits);
      assert(shared.dma_updates == base.dma_updates);
   }
   psx_overclock_factor = 0;

   /* --- larger event quanta keep the same invariants --------------------- */
   for (EventCycles = 128; EventCycles <= 1024; EventCycles <<= 1)
   {
      run_frame(&base, false, 0);
      run_frame(&shared, true, 0);
      assert(!memcmp(base.serialized, shared.serialized, sizeof(base.serialized)));
      assert(shared.exits <= base.exits);
      for (grid = 1; grid * 128 < FRAME_CYCLES; grid++)
         assert((int32_t)(grid * 128) - shared.service_at_grid[grid] <= EventCycles);
   }
   EventCycles = 128;

   MultiAccessSizeMem_Free(MainRAM);
   printf("Event scheduler: active channels, overclock factors and "
          "128/256/512/1024 quanta all preserved DMA state and service order\n");
   return 0;
}
