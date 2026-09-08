TV capture and FIFO follow-up — 2026-09-08

Reviewed `full_logcat_tv.txt` and `tv_logs.txt` from
`/Users/koundinya/flutter_projects/yage/`. The relevant session is PID 10375
on September 7 around 21:45; the full log also contains earlier runs. An old
SIGILL from another session is not evidence of a crash in this capture.

The active renderer is Vulkan at native resolution, with GPU presentation,
Lightrec execute, the SPU worker enabled and the GPU worker disabled. The
capture shows no persistent Lightrec fallback. Presentation readback and fence
times are zero in the frontend health samples.

Representative gameplay windows in `tv_logs.txt`:

| Line | Core frame time | CPU bucket | Finalize | GP0 words/frame |
| --- | ---: | ---: | ---: | ---: |
| 779 | 41.73 ms | 38.87 ms | 2.70 ms | 8,229 |
| 1091 | 38.08 ms | 36.37 ms | 1.57 ms | 8,309 |

The CPU bucket includes inline GPU execution. Its zero GP0 timing field means
timing was disabled; it does not mean GPU commands were free. Worker-off sync
polls return immediately, so thousands of polls are not thousands of costly
thread drains. The frontend also reports about 3.1–3.3 ms of sleep per iteration
while over budget, with frameskip 3 and about eight presentations per second.
Core queue changes cannot remove that frontend scheduling cost.

Changes implemented

- The draft's three independently published atomics were replaced with one
  atomic byte containing readiness and the compatibility threshold setting.
  The existing FIFO decoder computes it. Publication follows command execution,
  clock advancement, reset, readback completion and state restoration.
- DMA uses the publication only when the worker retirement tail covers every
  submitted command, staging is empty, no possible IRQ remains unapplied, and
  the compatibility setting still matches. Pending work retains the barrier.
- A staging batch now uses at most two contiguous copies, one head publication
  and one worker wake check, rather than repeating queue publication for every
  word. The normal ring and 1,024-word staging threshold are unchanged.
- `beetle_psx_hw_gpu_diagnostics` exposes `disabled`, `timing`, `fifo` and `all`.
  Timing is available with the worker disabled; FIFO validation compares exact
  snapshots against the synchronized decoder. Diagnostics default to disabled.
  The existing `BEETLE_PSX_GPU_TIME` override still works.

This is not the proposed complete emulation-thread shadow decoder. FIFO command
consumption depends on drawing-cycle progress, including quad and polyline
continuations. A command-length counter or a coherent but lagging worker
snapshot cannot safely replace those timing decisions. Zero observed mismatches
alone would not justify removing the remaining barriers. The retirement check
is the condition that makes the implemented fast path safe.

Validation

The generated test exercises PIO and real DMA2 linked lists, including uploads
split over packet boundaries, quad/polyline continuation, masks, overlapping
copies, texture/CLUT dependencies and pixel readback: 22 cases, 1,734 words.
The hardware-core matrix compares threaded and inline RAM, scratchpad, audio
and video hashes over 320 frames; it also exercises save/load every frame,
context recreation, diagnostics, 2x rendering and the software framebuffer.

A separate test-only 16-entry queue with four-word staging forces actual worker
execution, wraparound and backpressure through the same production queue code.
The stress matrix passed with 1,693 total queue-full stalls and zero FIFO
validation mismatches. A native GL line-pixel probe initially failed equally
with threading on and off; the retained polyline test verifies decoder
termination, not native line coverage. No general line-rasterization fix is
claimed here.

SPU state/audio/IRQ, worker ordering/drain and allocation-failure suites pass.
The Android release libraries include Vulkan, GLES3 and Lightrec for ARM32,
ARM64 and x86-64, with 16 KB LOAD alignment. Libretro exports, achievement
memory-map code and serialized guest state layout are unchanged.

Device measurement still needed

Use the same real-game scene with GPU Diagnostics = Command Timing and GPU
threading disabled to establish the inline GPU cost. Then compare threading
enabled and disabled with diagnostics off for the actual speed comparison.
FIFO Validation is for correctness captures, not performance measurements.
The worker remains off by default because the supplied capture cannot establish
a win from enabling it, particularly on the low-end TV. No new device tuning,
CPU affinity, readback, frontend modification or guest timing relaxation was
introduced. No physical-device Vulkan or live RetroAchievements test was run;
these local results do not establish a TV FPS increase or universal compatibility.
