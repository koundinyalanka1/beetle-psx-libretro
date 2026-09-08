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

---

The preceding review is historical. The approved plan is preserved below, followed by the current Phase 1 handoff, verification results, and remaining work.

---
agent: devin-local
session: locrian-bromine
created: 2026-09-08T07:47:14Z
---
# Megaplan: full-speed PS1 with clean audio on low-end Android TVs and phones

Optimize this Beetle PSX fork as a drop-in libretro core using measured CPU/JIT, event scheduling, rendering, and streaming improvements, while preserving native game timing and leaving YAGE source unchanged.

## Summary

The target is **low-end Android TVs generally, across manufacturers**, plus low-end Android phones—not a Sony/BRAVIA-specific patch. BRAVIA BF1 is the supplied measured example, not a device whitelist or the assumed minimum hardware.

The dominant observed problem is time spent inside the core's **inclusive CPU_Run path**. The capture has both a roughly 22 ms plateau and sustained 41–44 ms heavy sections. Audio underproduction and frontend video skipping follow from that. Vulkan presentation, native resolution, Lightrec, and SPU threading are already enabled; simply enabling them again will not solve this.

This is a staged optimization program, with correctness tests and device measurements deciding which changes ship. It is **not a promise that every low-end TV can run every PS1 game at full speed**. If the measured hardware floor remains above some devices, report that limit rather than substitute slowed audio, inaccurate emulation, or skipped presentation for success.

## 1. Scope and non-negotiable requirements

### Approved scope
- Modify only `beetle-psx-libretro`: core implementation, its build system, and its test/benchmark tools.
- Deliver `libmednafen_psx_hw_libretro_android.so` separately for each supported process ABI. A single binary cannot serve both ARM32 and ARM64.
- Preserve YAGE's libretro contract. YAGE was inspected to understand the integration, but its Dart, native, Android, settings registry, build manifest, and audio/presentation policies are **not implementation targets**.
- Native 1x graphics, ordinary SDR, existing native audio fidelity, correct game speed and pitch. Enhancements remain optional and are not part of minimum-device acceptance.
- No new dependencies unless a demonstrated requirement warrants separate review.

### Meaning of 60 fps
- Target approximately 60 emulated frames/fields per second for NTSC, using the core's actual reported timing rather than hardcoding 60.000.
- Native-60 games should produce smooth, near-native-rate visual updates. A game designed for 30 fps will still have approximately 30 unique pictures; repeated presentation at 60 Hz is not new game frames.
- Preserve PAL's native approximately 50 Hz timing. Do not enable fast-PAL or alter guest clocks to satisfy a display counter.
- Preserve 44.1 kHz stereo SPU output. A larger buffer, repeated audio, resampling slow game audio to a nominal rate, or halved sink pitch does not establish full-speed emulation.

### Correctness/security boundaries
- Preserve libretro exports, option namespaces, achievement memory maps, memory cards, and serialized guest-state compatibility wherever possible.
- Keep interrupt ordering, DMA visibility, VRAM dependencies, CPU-visible GPU status, SPU register/DMA timing, and save/load barriers correct.
- Preserve Android executable-memory/interworking safeguards and 16 KB ELF LOAD alignment. Never use destructive address-space probing or weaken platform security to improve a benchmark.
- No model-name performance hacks, forced CPU-ID pinning, real-time priority escalation, or disabling thermal protection.
- Baseline Lightrec is already performance-oriented: `include/lightrec-config.h` has load-delay handling disabled, and the logged opcode-cycle setting is 2. Do not describe it as cycle-exact or silently add further guest-timing shortcuts. Compare candidate changes against the current baseline, plus interpreter/hardware-oriented tests for the behavior being changed.

## 2. What the supplied logs establish

### Capture identity and deployment uncertainty
- Current core HEAD: `6afe79ec7ca9549687e5778e459e675939f6b46c`.
- `yage/tv_logs.txt:113` reports runtime version `0.9.44.1-GLES3 6afe79e`, matching this checkout's short revision.
- YAGE's checked-in `scripts/libretro_cores.json:145–151` instead pins `b0b900f8d324a4ae4976012849da366905b2f033`. A normal manifest build could therefore replace the manually tested core with another revision. This must be resolved in artifact provenance, not by assuming the manifest describes the captured binary.
- Current relevant app PID is **8742**. The large full log contains this session: for example `full_logcat_tv.txt:83006–83032` and `83704–83819`. It is not merely a boot log.
- Earlier boot crashes, other PIDs, and `tools/multicore/TV-performance-review.md`'s older PID 10375 session must not be attributed to this capture.

### Runtime configuration already in use

From `tv_logs.txt:113–161`, `221`, and `254–266`:
- BRAVIA BF1, Mali-G52, four homogeneous CPUs reported around 1.53 GHz; ARM32 path is in use.
- Vulkan renderer and GPU presentation.
- Internal resolution 1x, PGXP disabled, software framebuffer disabled, native guest CPU/GPU clocks.
- Lightrec `execute`, full invalidation, opcode cycles 2, SP/GP option disabled.
- `EventCycles=128`, `spu_samples=1`.
- GPU and SPU workers enabled.
- Reported video timing approximately 59.94/59.83 Hz and audio 44,100 Hz.
- OpenSL ES uses six 512-frame buffers and advertises playback-rate range 500–2000 permille.

### Performance is not one number

| Capture region | Observed result | Interpretation |
|---|---|---|
| `tv_logs.txt:326–340` | 22.15–22.67 ms/core frame; inclusive CPU 21.73–22.27 ms; about 44 runs/s and 22 presentations/s | Persistent over-budget plateau; frontend skips half the images |
| `tv_logs.txt:443–453` | 60.1 runs/s; 58.3 presentations/s in one window; 12.64–14.79 ms/core averages | Some scenes can approach full speed already; not proof for heavy gameplay |
| `tv_logs.txt:459–466` | 42.33 ms/core average, CPU 37.65 ms, finalize 4.45 ms; underrun field 419 in a later health window | Heavy cost includes both CPU-path work and occasional more expensive finalization |
| `tv_logs.txt:484–489` | Explicit playback rate 500 permille; a 5-second window at 16 runs/s, run 60.7 ms | Severe stall window and audible-speed compromise; not a universal steady-state figure |
| `tv_logs.txt:738–739`, `1041`, `1307–1313` | Repeated 41–42 ms/core averages; CPU approximately 39–40 ms; 200 of 300 images skipped | Sustained heavy sections, not just a one-off shader compilation |
| `tv_logs.txt:1101`, `1226`, `1294`, `1446` | 22.8–24.1 runs/s, 7.6–8.2 presentations/s, repeated nonzero underrun fields | The demanding acceptance problem is substantially worse than the initial 44 Hz plateau |

The current capture does not label exact game scenes or distinguish guest computation from host preemption, compilation, alignment-fault handling, or storage waits. These are **wall-time buckets**, not a CPU sampling profile. Avoid assigning a precise cause to the 40 ms CPU bucket before measuring it.

### Audio diagnosis
- `spf_in≈spf_out≈736` means YAGE is receiving approximately the expected number of **stereo sample frames per emulated frame**. These are not 736 scalar int16 samples and not 736 frames per wall-clock second.
- At 44 runs/s, supply is approximately `44 × 736 = 32,384` stereo frames/s, versus 44,100 required at normal speed.
- At 23–24 runs/s, supply is only about 17–18 kHz-equivalent stereo frames/s. The captured driver's 500-permille minimum still consumes approximately 22,050 frames/s, so it cannot absorb such a sustained shortage even by halving playback speed.
- YAGE proactively reduces playback rate when frame intervals are more than 5% late (`native/yage_audio.c:132–194, 670–700`). This explains why a slow core can have a nonempty ring and zero reported underruns while audio pitch is wrong.
- Hold/repeat activity and trimming are visible. In addition, `g_underrun_count` resets when real samples resume (`yage_audio.c:704–727`), so the health field is not a cumulative lifetime dropout count. Never use its zero value as the sole audio acceptance test.
- Core audio callback cost around 0.01 ms measures callback submission, **not** the total SPU synthesis cost. SPU worker time is reported separately.

### GPU/threading diagnosis
- Logged GPU worker busy time is zero, all GP0 words are inline, and the worker queue remains empty.
- Plateau inline GP0 work is about 2.1 ms/frame; later heavy sections show about 1.4 ms despite CPU-path time approaching 40 ms.
- SPU already runs about 0.7–1.0 ms/frame on its worker, with typically small synchronization waits. This is not a missing-audio-thread problem.
- `readback=0`, `fence=0`, and typical presentation wait around 0.8–1.0 ms establish that CPU framebuffer readback is not the plateau bottleneck. Zero frontend fence counters do not prove the core has no internal waits.
- `to_set_image` includes preceding core work and is not additive GPU-only time. GPU worker time and inline GPU cost must not be added again to an inclusive CPU bucket.

## 3. Exact hot-path map

```text
YAGE native loop (read-only integration reference)
  -> retro_run() [libretro.c]
     -> AV enable poll / worker sync / RHI prepare
     -> input and option work
     -> CPU_Run() [mednafen/psx/cpu.c]
        -> lightrec_plugin_execute()
           -> lightrec_execute() / first-pass interpretation
              -> generated ARM code, memory wrappers, GTE helpers
              -> compilation queue / invalidation / code cache
           -> PSX_EventHandler() [libretro.c]
              -> GPU_Update()
              -> DMA_Update() -> GPU_Update(), MDEC_Run(), seven channel checks
              -> PS_CDC_Update() -> SPU_UpdateFromCDC()
              -> TIMER_Update(), FrontIO_Update()
     -> drain workers / finish SPU and frame state
     -> rhi_vulkan_finalize_frame() or other renderer
     -> audio_batch_cb(IntermediateBuffer, SoundBufSize)
```

### Scheduling detail that matters
- `cpu.c:3757–3840` re-enters Lightrec around event handling; current code already copies COP0 primarily at frame entry/exit, not every quantum. Do not propose that already-implemented optimization again.
- `libretro.c:1526–1582` dispatches and reorders due events. Existing diagnostics report inclusive event time and sampled per-type estimates.
- `dma.c:555–577` has its own `EventCycles` cadence even when little DMA work is available, calls `GPU_Update`, runs MDEC, and scans seven channels.
- `gpu.c:3234–3273, 3745–3746` has a separate GPU deadline and resolves staged commands on clock updates. GPU/DMA deadlines can be out of phase; there is not one universal 128-cycle tick.
- `cdc.c:410…`, `1654–1887` also schedules SPU sample deadlines; `spu.c:1702–1727` uses 768 guest cycles per sample at the native setting.
- Consequently about 8,100 Lightrec quanta and 9,966 dispatched events are not evidence of 9,966 SPU mixes. Obtain counts by event type and actual emulated progress per exit before deciding which cadence to change.
- `EventCycles` also controls the drawing-credit limit (`gpu.c:3246–3247`). Raising it is not a purely host-side scheduling optimization.
- The current `gpu_timing.h` already avoids common ARM software divides while preserving the previous calculation. Optimize remaining measured work, not a stale version of this code.

### GPU staging detail
- `GPU_STAGE_MAX` is 1024; `GPU_Stage_GP0` publishes only at this threshold.
- `GPU_Stage_Resolve` executes short staged batches inline when the worker is idle (`gpu.c:2813–2910`). GPU clock updates and status/DMA synchronization repeatedly force resolution.
- Merely turning threading on therefore does not make this workload parallel. Conversely, forcing every short batch onto a worker can increase wakeups and immediate waits.
- Staged inline execution performs two clock reads per batch even when per-word diagnostics are disabled. Measure that instrumentation overhead and make production timing policy consistent.
- Software rendering currently disallows this worker because emulation-thread scanout reads live VRAM. GL uses deferred command recording; Vulkan can execute through the worker with queue locking. PGXP excludes the worker. Preserve these ownership constraints (`gpu.c:2631–2681`).

### JIT/build detail
- `deps/lightrec/recompiler.c:205–283` currently creates `logical_cpu_count - 1` compile workers, minimum one. On the captured four-core CPU this means three possible compiler workers in addition to emulation, SPU, renderer, frontend and system activity. Idle sleeping workers are not themselves a bottleneck; measure when they are runnable and contending.
- First-pass execution and compilation paths are in `lightrec.c:get_next_block_func`; cancellation can wait for a compile in flight. Code-cache exhaustion triggers reclamation.
- Memory emitters already include runtime guards. The fallback map can route dynamic accesses through generic C wrappers (`lightrec.c:298–379`, `emitter.c:2348–2411`). Log the actual mapping mode before treating this as the active bottleneck.
- Android A32 emission/interworking is already explicitly selected in `deps/lightning/lib/jit_arm.c:235–258`. Preserve it. The captured SIGBUS handler installation does not prove faults occurred; profile actual fixups before blaming them.
- YAGE builds via the **root Makefile with `platform=unix` and NDK compilers**, not `jni/Android.mk`. Root release builds already append `-O3 -DNDEBUG` and preserve `-fwrapv -fsigned-char`. Adding these only to JNI would miss the captured deployment path.
- Root `platform=unix` still has host `uname` architecture/CD-ROM decisions. Verify target-derived behavior on Windows/Linux/macOS build hosts instead of assuming host architecture equals Android target architecture.

## 4. Acceptance targets and measurement rules

### Proposed engineering gates
These are goals to validate, not measured results or guaranteed savings.

| Metric | Acceptance target |
|---|---|
| Emulation speed | Native reported rate within approximately ±0.5% over a stable multi-minute workload; no growing lag |
| Core frame-time headroom | Aim for p95 at or below about 14 ms and p99 below the native NTSC frame budget in warm gameplay; also report median, maximum and deadline misses |
| Recovery from frameskip | Sustained core EWMA below `0.85 × native frame period` (about 14.2 ms here) so unmodified YAGE can leave its skip state |
| Native-60 visual output | No sustained frontend skips; submitted valid images and observed display updates tracked separately; investigate unexplained gaps |
| Audio | Correct duration/pitch; correct cumulative sample count per emulated time; no unexpected repetition/dropout or sustained hold/trim/underrun behavior |
| Thermal endurance | Repeat acceptance after 20–30 minutes; no progressive performance or memory collapse |
| Compatibility | No new state/audio/IRQ/render mismatches in differential tests; representative games and lifecycle cases pass |
| Production instrumentation | No per-word/per-event clock reads or disk writes unless diagnostics are explicitly enabled; quantify overhead of the disabled path |

At 22.5 ms, reaching 14 ms requires about 38% less frame time. At 41.84 ms it requires about 67% less time, approximately 3× throughput. Merely reaching 16.7 ms from 41.84 ms needs approximately 2.5× throughput. These ratios describe the gap, not a forecast for any optimization.

### Benchmark protocol
1. Use identical core, frontend, content, BIOS choice, options, input sequence, scene and starting state for paired tests.
2. Label boot, title/menu, attract/demo, active match, character/stage changes, loading, FMV, and return-to-menu separately. Do not average them into one FPS number.
3. Measure cold launch/JIT/shader warmup and warmed repeatable gameplay separately. Record compile/cache state and temperature; a saved state is not automatically a warmed JIT cache.
4. Run baseline/candidate/baseline in alternating order, with at least three repeats, so DVFS/thermal variance is visible.
5. Use ordinary release builds for final speed comparisons. Diagnostic, sanitizer, validation-layer, per-frame hashing and readback builds are correctness/profiling tools, not shipping-performance evidence.
6. Collect monotonic timestamps and align core counters with YAGE health windows. Record both wall time and emulation-thread CPU time where available; use CPU sampling plus scheduler traces for preemption, futex waits, memory faults and driver time.
7. Use application/process-filtered captures and bounded log volume. The full log and later TV log contain extensive input/system logging; its impact is a measurement confounder, not an established cause of the CPU bottleneck.
8. Save machine-readable benchmark output outside source code. Keep raw captures and symbolized binaries associated by build ID and SHA-256.

## 5. Implementation steps, priorities and decision gates

### Phase A — Reproducible baseline and honest profiling (P0)

**Files:** `libretro.c`, `mednafen/psx/cpu.c`, relevant Lightrec sources, `tools/vkhost/vkhost.c`, `tools/glhost/glhost.c`, `tools/multicore/Makefile`.

1. Record core revision/dirty status, NDK version, compiler target, ABI, enabled backend/JIT/SIMD features, active renderer, mapping mode, compiler-worker count, and effective options once per session.
2. Extend the existing diagnostics rather than adding a separate always-on profiler. Provide bounded frame-time histograms and distinguish valid image / frontend-requested skip / core duplicate.
3. Add per-type event counts, aggregate guest cycles per Lightrec exit, exit-reason counts, first-pass/interpreted versus compiled work, compilation queue depth/time, invalidation and code-cache reclamation counts.
4. Measure direct and nested GPU/DMA calls without double-counting. Existing per-type event estimates are inclusive: DMA calls GPU; document this in the report format.
5. Break down RHI prepare/finalize into CPU work, resource reuse waits, pipeline creation, scanout, flush/queue-lock time and optional texture-tracker work.
6. Add optional aggregate PCM frame counts/hashes or WAV capture to existing hosts. Add a benchmark mode that disables image dumping, validation layers and per-frame hashing so the validation host can also measure CPU/core throughput without those costs.
7. Compare diagnostics off/on overhead, especially the unconditional staged GP0 timers. Keep heavy timing/counters compile-time or runtime opt-in as appropriate; do not call frontend logs from JIT inner loops or audio workers.

**Gate A:** A reproducible heavy scene has a defensible cost breakdown. The next code change must name the measured component it targets. If hardware/profiling access is unavailable, complete instrumentation/tests but do not claim an optimization win.

### Phase B — Portable, consistent Android release builds (P0/P1)

**Files:** `Makefile`, `Makefile.common`, `jni/Android.mk`, `jni/Application.mk`; tests under existing tools where appropriate.

1. Make Android target architecture decisions come from the compiler/ABI rather than build-host `uname`. Preserve explicit user/toolchain overrides. Test both the existing `platform=unix` NDK route and `platform=android`.
2. Verify final effective C/C++/link flags rather than editing flags blindly. Preserve release optimization and integer behavior flags; compare `-O2`/`-O3`, host Thumb/A32 choices and optional ThinLTO only with measurements and differential tests.
3. Separate host compilation instruction-set choices from the JIT emitter's Android A32 requirement. Do not re-enable JIT Thumb merely because host Thumb is compact.
4. Confirm ARM NEON paths are compiled on supported targets. For any declared legacy non-NEON support, retain scalar baseline or dispatch only separately compiled SIMD code after a reliable feature check; do not make a supposedly portable ARMv7 binary execute NEON unconditionally on unsupported CPUs.
5. Keep ARM64 baseline instructions portable; runtime-gate optional extensions. Do not use `-march=native` or tune every TV binary to BRAVIA's SoC.
6. Bring JNI builds into semantic parity where necessary, but do not mistake JNI-only changes for fixes to the captured root-Makefile build.
7. Emit build ID and verify exports, architecture, dependencies, stack budget and 16 KB LOAD alignment. Keep unstripped symbols separately.

**Gate B:** Same source/options behave consistently across build hosts and ABIs. Existing `-O3`, enabled JIT and known interworking repairs are baseline facts, not claimed new FPS gains.

### Phase C — JIT execution and memory-access critical path (P1, potentially highest leverage)

**Files:** `mednafen/psx/cpu.c`; `deps/lightrec/{lightrec.c,emitter.c,optimizer.c,regcache.c,blockcache.c,recompiler.c}` as indicated by the profile; `deps/lightning/lib/jit_arm.c` only for demonstrated backend defects.

1. Measure actual compiled execution versus first-pass/generic memory/helper work in the 40 ms scene. Inspect representative emitted ARM blocks for spills, repeated map tests, helper transitions and software division.
2. If fallback mappings cause generic RAM traffic, extend the existing **guarded** RAM path to cover proven common accesses on both mapping layouts, retaining an exact slow path for MMIO, BIOS, scratchpad, cache-isolated RAM and unmapped regions.
3. Preserve load/store width, sign extension, alignment, LWL/LWR/SWL/SWR behavior, wrap/mirror semantics, COP2 effects, and the baseline load-delay model. Validate a register whose address changes from RAM to MMIO across executions; never specialize from a single observed address without guards.
4. Keep self-modifying-code invalidation correct for CPU stores, DMA, memory clears, aliases and writes crossing code/page boundaries. Profile existing code-page tracking before adding another system. Do not simply set DMA-only invalidation globally.
5. Reduce redundant work at JIT/event/helper boundaries only where state ownership is clear. Existing frame-level COP0 synchronization must remain correct under IRQs, syscall exits, save/load and interpreter fallback.
6. Preserve completed unmapped-access recovery; distinguish it from a stuck/no-progress fault or allocation failure. A rare recoverable access must not silently retire JIT for the session.
7. If GTE helpers dominate, optimize their existing integer math/SIMD/register transfer with equivalence tests. Do not remove GTE latency or alter guest clocks. If the profile does not implicate GTE, defer it.
8. Fix any demonstrated host unaligned accesses rather than relying on a signal handler as normal execution. Require real ARM32 tests; desktop success cannot prove A32 correctness.

**Tests:** Extend generated test ROM probes and host state/hash comparisons; add a focused new harness only where existing tools cannot exercise the relevant memory/JIT boundary. Validate both ARM32 and ARM64, full/fallback mapping paths, cold/warm execution, reset and save/load.

**Gate C:** Every fast path has a conservative fallback and a failing-before/passing-after targeted test. Ship only measured improvements with no new guest-state/audio/IRQ mismatch. Existing baseline differences from the interpreter must be characterized rather than blindly using whole-game equality as an impossible oracle.

### Phase D — Reduce event overhead without relaxing device timing (P1)

**Files:** `libretro.c`, `mednafen/psx/{cpu.c,dma.c,gpu.c,gpu_timing.h,cdc.c,mdec.c,timer.c}` as the profile requires.

1. Measure GPU, DMA, CDC/SPU, timer and input event counts/costs, including DMA's nested GPU/MDEC updates and idle-channel checks.
2. First optimize equivalent work: zero-elapsed updates, repeated clock/mode computations, event-list/minimum selection, and inactive-channel checks whose state can be preserved exactly. Do not assume inactive channels have no accounting side effects.
3. Implement an idle-DMA/MDEC fast path only after proving how pending transfers, register writes, readiness transitions, chopping, IRQs and CPU halt state re-arm processing. Keep the existing periodic path for active/uncertain cases.
4. Consider shared/coalesced GPU/DMA bookkeeping only if it preserves original observable deadlines and tie ordering. Do not silently align previously distinct device events or delay status updates.
5. Add a focused scheduler/DMA harness (proposed `tools/multicore/event_scheduler.c`) and extend GPU timing tests. Test same-timestamp events, register accesses between events, simultaneous interrupts, line boundaries, DMA readiness and state restoration.
6. Run an **experimental** sweep of EventCycles 128/256/512, then 1024 only if warranted. Record changes in guest state/IRQ/render/audio results as well as throughput. These are diagnosis/compatibility experiments, not preapproved global defaults.
7. Keep `spu_samples=1` in the production fidelity baseline. Larger values are a separate experiment because `SPU_UpdateFromCDC` generates several samples when a batch becomes due, changing when those samples see register state. Worker batching is the safer amortization lever.

**Gate D:** Prefer equivalent scheduling/idle-work savings. Any candidate that changes guest-observable timing is not promoted as a universal low-end fix merely because Tekken boots or sounds acceptable.

### Phase E — Bounded compiler concurrency and work queues (P1/P2)

**Files:** `deps/lightrec/recompiler.c`, `mednafen/worker_affinity.{c,h}`, `tools/multicore/{recompiler.c,affinity.c,affinity_linux.c,workers.c}`.

1. Compare one, two and current automatic compile-worker counts, plus synchronous compilation as a diagnostic control, on cold launches and warm heavy scenes.
2. Base worker limits on CPUs actually available to the app and measured contention, not just total logical CPUs or TV model. Preserve intentional frontend cluster masks and Android cpusets.
3. Reserve CPU headroom for emulation/audio/frontend by bounding background compilation only when the matrix demonstrates a win. Do not impose one universal cap without warmup/throughput measurements on dual-core, quad-core and asymmetric systems.
4. Profile the linked-list request scan, allocator lock, cancellation waits and code-cache flushes. Optimize only if queue depth makes them material; keep existing highest-request priority and lifecycle correctness.
5. Preserve the recent predicate-before-sleep fix and allocation-failure cleanup. Ensure pause, flush, reset, shutdown and second launch cannot strand work or wait forever.
6. Do not persist raw generated native code across sessions as a shortcut; ASLR, mappings, ABI, build and invalidation make this a separate design problem.

**Gate E:** Reduced p95/p99 launch and gameplay cost with no excessive time interpreted, deadlocks, memory growth or thermal regressions. If workers sleep through warm gameplay, do not claim their count explains warm CPU cost.

### Phase F — GPU command batching and native renderer overhead (P2)

**Files:** `mednafen/psx/gpu.c`, `rhi/{rhi_intf.c,rhi_defer.c,rhi_lib_vulkan.c,rhi_lib_gl.c,rhi_tt.c}`, existing GPU test/host tools.

1. Establish equivalent diagnostic baselines with GPU threading enabled and disabled. The present capture's inline-only execution means disabling unnecessary staging may be cheaper; measure rather than assume.
2. Record batch-size distribution and why batches resolve: clock tick, status read, DMA readiness, readback, queue threshold or end-of-frame.
3. Sweep bounded staging thresholds in experimental builds (for example 64/128/256/1024 words). Retain ordering and all necessary synchronization. Do not equate more worker activity with higher performance.
4. Remove unnecessary staging copies/publications/timers on the inline path if profiles show a benefit. Publish bursts only where CPU work can overlap them before the next required observation.
5. Preserve authoritative FIFO readiness, retirement checks, IRQ delivery, VRAM readbacks, texture/CLUT dependencies, quad/polyline continuation and GL context affinity. A shadow decoder or wholesale barrier removal is explicitly not the first implementation.
6. Investigate heavy finalization separately: pipeline creation, scanout image allocation/reuse, descriptor/resource churn, transfer/render-pass breaks and queue-lock contention.
7. Pipeline caching already exists in the Vulkan renderer, including on-disk validation. Verify hits/misses and write placement before proposing a new cache. If cache saving causes stalls, move/bound it with lifecycle-safe synchronization; do not add synchronous disk work to every frame.
8. Audit disabled HD/analog/HDR paths for residual tracking, allocation or synchronization at native SDR. A bypass must retain any tracking needed for native VRAM rendering, not just skip a function based on its name.
9. Benchmark rendering every frame as well as honoring YAGE skip requests. Current 41 ms averages already include skipped scanouts, so final full-video cost can be higher than a skipped baseline.

**Gate F:** Existing PIO/DMA generated-ROM matrix passes with identical relevant results, queue stress actually exercises worker execution, Vulkan validation is clean, and native display cadence improves on real drivers. Roughly 1–2 ms of observed inline GP0 cost alone cannot explain or eliminate the 40 ms heavy CPU bucket.

### Phase G — SPU/CD audio fidelity and streaming stalls (P2, mandatory verification)

**Files:** `mednafen/psx/{spu.c,cdc.c,mdec.c}`, `mednafen/cdrom/cdromif.c`, relevant image decoder only if profiled, `tools/multicore/spu.c`, host audio capture.

1. Keep 44.1 kHz stereo, native voice count, interpolation, ADSR, reverb, CDDA and XA behavior. Do not disable audio features or synthesize filler audio to improve an FPS counter.
2. Retain the existing worker batching (approximately eleven full 64-sample jobs plus a partial job for a 735-sample frame). Benchmark alternative bounded worker batch sizes only with register/DMA/IRQ equivalence.
3. Preserve the synchronous path while SPU IRQ timing requires it and all partial-batch drains on registers, DMA, save/load and frame boundaries.
4. The silent-voice optimization already excludes looping voices, IRQ-sensitive voices and other observable cases. Extend tests for ENDX, pitch modulation, capture RAM and restarting released voices before modifying it.
5. Instrument CD sector wait duration, read-ahead hits/misses, seek behavior and CHD decode time. The CD interface already has a read-ahead worker for non-memcached images; the option named `sync` is not proof that no background I/O exists.
6. Optimize bounded read-ahead/cache behavior only if storage explains p99 spikes. Test local flash, supported external storage and BIN/CUE versus CHD. Do not default to precaching an entire 600–700 MB disc on a low-memory TV.
7. Compare OpenBIOS and a legitimately supplied real BIOS in the same tests to separate BIOS compatibility differences; do not require the user to obtain proprietary firmware from an unauthorized source.
8. Validate captured PCM against baseline by sample count, hashes where deterministic, waveform continuity, pitch and duration. Use audible content; expected silent passages are not underruns.

**Gate G:** Core output stays correct, and unmodified YAGE runs at native speed without sustained elastic playback/hold/trim artifacts. A clean core PCM capture alone does not establish clean physical audio output through a TV's HDMI/OpenSL path.

### Phase H — Broad low-end TV capability coverage and software fallback (P2/P3)

**Files:** shared CPU/JIT paths above, `mednafen/psx/{gpu.c,gpu_polygon.c,gpu_sprite.c,mdec.c}` where present and implicated, `rhi/rhi_intf.c`, build files and existing test hosts. Confirm actual rasterizer source names before editing; preserve current layout.

1. Test capability cohorts, not brand names:
   - ARM32 userspace on quad-core ARMv8-class TVs with usable Vulkan.
   - Native ARM64 low-end TVs/boxes with usable Vulkan.
   - Older/slower ARMv7 TV/box CPUs within the declared instruction baseline.
   - Devices without usable Vulkan or with a declined GPU-presentation path.
   - Low-memory configurations around 1–2 GB where available.
   - At least one different Mali generation/vendor driver and an Adreno phone; homogeneous and big.LITTLE CPUs.
2. Run shared JIT/scheduler improvements across all cohorts; do not select performance defaults from RAM alone.
3. YAGE currently chooses software for PS1 when Vulkan is unavailable and avoids its problematic PS1 GL presentation path. Core-only work cannot assume a new GLES route will be selected automatically.
4. Keep the software renderer correct and operational; profile native software rasterization, scanout/copies and MDEC separately on this cohort. Improve existing scalar/SIMD kernels where measured, with pixel equivalence.
5. Do not just enable the existing GPU worker for software: scanline VRAM ownership currently forbids that. A parallel tile/rasterizer redesign would require ordered primitive/VRAM/scanout snapshots and substantial new tests. Treat it as a separately gated escalation, not an implied low-risk step in this phase.
6. Do not silently switch a live hardware session to software during context loss; current renderer code documents why that can corrupt uninitialized software state. Preserve supported lifecycle transitions.
7. Determine minimum supported hardware from sustained game-suite results, distinguishing **loads correctly**, **playable with compromises**, and **full-speed native**.

**Gate H:** No device-specific assumptions leak into the common core. A cohort that fails performance is explicitly marked unverified/unsupported for the full-speed target, not counted as a pass because audio is being slowed.

## 6. Experiment matrix and shipping policy

| Experiment | Baseline/control | Shipping rule |
|---|---|---|
| GPU worker | Current enabled vs disabled | Choose by measured overlap/cost; retain user override |
| GP0 staging | Current 1024 vs smaller bounded thresholds | No new ordering/readiness violations; p95/p99 benefit across cohorts |
| Compile workers | Current N−1 vs 1 and 2 | Capability-aware bounded policy only after cold/warm tests |
| Event quantum | 128 vs 256/512, optional 1024 | Diagnostic first; not universal unless observable timing equivalence is established |
| SPU scheduling | Sample update 1 vs experimental 4/16 | Do not ship larger update count merely for speed; prefer worker batching with unchanged sample clock |
| Host optimization | Current effective release flags vs O2/O3/optional LTO | Same semantics/ABI; measured native-device win; keep integer behavior/security flags |
| JIT memory | Existing guarded/direct/wrapper paths | Retain MMIO/unmapped/invalidation fallback and test both map layouts |
| CD behavior | Existing sync/read-ahead behavior vs bounded alternatives | Reduce stalls without RAM blowup or guest timing/data errors |
| Renderer | Current Vulkan; software where YAGE chooses it | No unsupported frontend API/path assumptions |

Do not change many options at once. Keep native CPU frequency 100%, GPU/GTE overclocks off, compatibility fixes on, PGXP off and native resolution fixed during baseline experiments. Do not globally switch to DMA-only invalidation, disable compatibility settings, or force fast PAL. Changes to option defaults must update the core's advertised table and parser fallback consistently, while honoring values the frontend explicitly supplies.

One existing consistency issue is the ARM Android no-variable fallback in `libretro.c:4676–4681`, which still disables JIT despite the repaired A32 path. It can be corrected after the ARM safety matrix, but the captured YAGE session explicitly supplies `execute`, so it is **not the cause of the measured slowdown**.

## 7. Files to modify

### Core integration, profiling and options
- `libretro.c`: effective-runtime report, bounded phase/event metrics, deadline-preserving scheduler work, consistent validated defaults and audio accounting.
- `libretro_core_options.h`: only validated option/default/diagnostic changes; preserve prefixes and accepted values.
- `beetle_psx_globals.h`, `mednafen/psx/cpu.h`, related headers: only if new internal statistics/control interfaces require them; no new public frontend ABI requirement.

### CPU/JIT
- `mednafen/psx/cpu.c`: engine/boundary metrics and verified execution improvements.
- `deps/lightrec/lightrec.c`, `emitter.c`, `optimizer.c`, `regcache.c`, `blockcache.c`, `recompiler.c`: profile-directed memory, compilation, invalidation and register-path improvements.
- `deps/lightning/lib/jit_arm.c`: only demonstrated A32 backend issues/capability detection, preserving interworking.

### Device scheduling and rendering
- `mednafen/psx/dma.c`, `gpu.c`, `gpu_timing.h`, `cdc.c`, `mdec.c`, `timer.c`: exact scheduling/idle-work/batching candidates.
- `rhi/rhi_intf.c`, `rhi/rhi_defer.c`, `rhi/rhi_lib_vulkan.c`, `rhi/rhi_lib_gl.c`, `rhi/rhi_tt.c`: measured command/scanout/resource costs, without weakening synchronization.
- Existing rasterizer sources: conditional software-fallback work only if profiles justify it.

### Audio, streaming, concurrency and builds
- `mednafen/psx/spu.c`, `mednafen/cdrom/cdromif.c`: bounded batching/read-ahead and sample/IRQ correctness.
- `mednafen/worker_affinity.c`, `.h`: permitted-mask discovery only if necessary for a tested worker policy.
- `Makefile`, `Makefile.common`, `jni/Android.mk`, `jni/Application.mk`: portable target detection and reproducible release variants.

### Tests and benchmarking
- Extend `tools/multicore/{Makefile,spu.c,workers.c,recompiler.c,gpu_timing.c,affinity.c,affinity_linux.c,make_gpu_test.py,check_gpu.py}`.
- Extend `tools/vkhost/{vkhost.c,Makefile}` and `tools/glhost/{glhost.c,Makefile}` rather than invent another frontend.
- Proposed new `tools/multicore/event_scheduler.c`: necessary focused coverage if the scheduler/DMA changes proceed. Add a new JIT harness only if generated-ROM/host coverage cannot test the required boundary.
- No changes under YAGE. Do not create extra prose change-summary documents; this plan file contains the implementation plan.

This is a conditional file map, not an instruction to modify every listed file. Each phase should produce the smallest patch supported by its measurements.

## 8. Verification

### Existing host commands to reuse
Run in a suitable POSIX C11/pthreads environment, from the core repository, after implementation approval:

```sh
make -C tools/multicore check

make -C tools/multicore check BUILD_DIR=/tmp/beetle-multicore-tsan \
  CFLAGS='-O1 -g -std=c11 -fwrapv -fsanitize=thread'

make -C tools/multicore check BUILD_DIR=/tmp/beetle-multicore-asan \
  CFLAGS='-O1 -g -std=c11 -fwrapv -fsanitize=address,undefined'

make -C tools/glhost
make -C tools/vkhost
python3 tools/multicore/check_gpu.py /absolute/path/to/hardware-core.so \
  --output /tmp/gpu-check
```

- Review and triage baseline sanitizer failures before attributing them to a change. TSan host support varies; it is not a replacement for ARM device testing.
- Existing generated GPU matrix covers 22 PIO/DMA cases and compares RAM/scratchpad/audio/video over 320 frames, including save/load/context recreation/2x/software framebuffer variants.
- Separately run the documented `BEETLE_GPU_QUEUE_STRESS_TEST` build with `--require-worker-queue`. Never ship it or compare its artificial tiny-queue performance to release.
- Use `VKHOST_VARS`, `VKHOST_HASHES`, `VKHOST_ROUNDTRIP_FRAME` and `VKHOST_RECREATE_FRAME` for targeted Vulkan/state checks. The current Vulkan host is a validation/readback tool, so its normal execution is not an end-to-end TV performance benchmark.
- Build/smoke-test ARM32, ARM64 and, if retained by packaging, x86_64. Validate exports/build ID/dependencies/16 KB LOAD alignment with the NDK ELF tools. Verify no undefined renderer/JIT symbols and no mismatched ABI.
- Existing Makefile has no general out-of-tree object isolation; use separate approved build trees for experimental variants. Do not overwrite/delete uncommitted work or rely on stale objects between ABIs.

### Required regression cases
- Event deadline/tie ordering, idle-to-active DMA, CPU halt/IRQ transitions, GPU FIFO readiness, MDEC and display-mode changes.
- RAM mirrors, mixed RAM/MMIO addresses, scratchpad/BIOS, unaligned guest accesses, code/page-boundary writes, self-modifying code, DMA invalidation, JIT/interpreter transition and allocation failures.
- GPU PIO/DMA uploads, texture/CLUT dependencies, masks, overlapping copies, polylines/quads, readback and interlaced/noninterlaced scanout.
- SPU key on/off, ADSR, reverb, noise, pitch modulation, ENDX, IRQ-sensitive decoding, DMA, CDDA/XA, partial worker jobs and mid-frame save/load.
- Reset; state save/load; menu/pause/resume; surface/context recreation; second game launch; process/thread teardown. Verify existing save data remains usable.
- Achievement memory-map and per-emulated-frame semantics remain intact; live service integration requires a separate available test environment.

### Real game/device suite
Use legitimate user-supplied content. Starting candidates, subject to availability:
- Tekken 3 USA: current baseline, menu/attract/active matches, multiple stages, transitions and repeated matches.
- Another native-60 3D title (for example a racing game) to avoid a Tekken-only optimization.
- A typical 30 fps 3D title: verify correct game rate without inventing frames.
- A 2D/VRAM-heavy title such as Symphony of the Night: sprites, transparency, texture/CLUT dependency coverage.
- An FMV/CD/XA/CDDA-heavy title and an SPU-streaming/IRQ-sensitive title; include the silent-loop/ENDX regression shape.
- At least one PAL title and one game that changes resolution/interlace mode.

For each representative device cohort, record SoC/CPU features, process ABI, available CPU mask, RAM, Android version, GPU/driver/API, refresh mode, audio output route and thermal state. Exact phone/other-TV inventory is still needed for execution; none beyond BRAVIA has been benchmarked in this session.

### Local environment status
- This session is on Windows. Python, make, CMake, adb and Android NDK directories `28.2.13676358` and `30.0.15729638` are present.
- `wsl --list --quiet` failed because the WSL service is unavailable/disabled. No service/configuration changes were made.
- Existing POSIX test commands therefore require a verified MSYS2/Linux/remote runner or user-approved environment setup. Do not silently enable WSL or install tools.
- No source edits, builds, tests or on-device performance captures have been executed during planning. All proposed speed gains remain unverified.

## 9. Risks, frontend constraints and escalation

1. **Hardware ceiling:** Some low-end TVs may not have enough CPU for this fork's fidelity target, particularly software-only devices. Measurement defines support; optimization cannot guarantee an arbitrary hardware floor.
2. **Frontend recovery threshold:** YAGE leaves skip mode only below 85% of the native period (`yage_frame_loop.c:1134–1138`). A core improvement from 22 ms to 16 ms may restore supply yet fail to recover full presentation immediately.
3. **Presentation is not identical to emulation rate:** `tv_logs.txt:499` already shows about 59.7 runs/s but only 43.3 presentations/s with snapshot misses. This may include transition/catch-up effects; it requires correlation, not an assumed core bug. If persistent at full core speed, report a frontend-scoped blocker and seek separate approval.
4. **Audio sink remains frontend-owned:** Correct PCM and full core speed are necessary, not sufficient, for clean HDMI/TV audio. Do not work around a frontend sink issue by falsifying core timing or sample counts.
5. **Non-Vulkan path:** Existing YAGE PS1 GL restrictions prevent assuming a universal renderer switch. Software coverage is part of the plan, with a potentially higher CPU requirement.
6. **Timing knobs:** EventCycles and SPU sample update size affect guest behavior. Small game suites cannot establish universal compatibility for a relaxed scheduler.
7. **Threading risks:** More parallelism can increase cache traffic, wakeups, priority contention and thermal load. Queue/barrier changes need deterministic tests and real weak-memory ARM testing.
8. **JIT memory safety:** Fast memory accesses must retain mapping guards and invalidation. Code-cache size increases can worsen low-memory pressure; do not trade paging stalls for an apparently faster warm microbenchmark.
9. **Existing diagnostics:** Sampling clocks/logging and input floods can distort profiles. Always verify with diagnostic-free release builds.
10. **Artifact integration:** Keep YAGE's manifest unchanged under this scope. Deliver the core artifacts and provenance; normal production packaging must explicitly select the approved revision/artifact in a separately authorized integration step, otherwise the old manifest pin can undo the work.

If Phases A–G cannot bring the minimum supported cohort under budget, stop and present the residual profile. A software-renderer architectural redesign, replacement JIT/core, compatibility-reducing profile or frontend changes are **new scope decisions**, not automatic next edits.

## 10. Completion checklist and delivery order

1. **Baseline/profiling milestone:** reproduce and label both 22 ms and sustained 41–44 ms regions; capture actual JIT/event/host-wait costs and build identity.
2. **First optimization milestone:** smallest verified build/JIT/event critical-path improvements, with targeted tests and before/after device data.
3. **Concurrency/rendering milestone:** only the worker/batching/resource changes whose measured gain survives correctness and thermal tests.
4. **Audio/streaming milestone:** prove sample/IRQ fidelity and normal-pitch physical playback at restored emulation speed.
5. **Broad-device milestone:** test the capability cohorts and game suite; publish measured minimum support and remaining limitations.
6. **Artifact milestone:** per-ABI release `.so`, matching symbols/build IDs/hashes, exact flags/options, regression results and machine-readable benchmark data. No commit/push/deployment without the relevant user authorization.

Success means **correct-speed emulation, appropriate native visual cadence, and clean normal-pitch audio on the tested support matrix**, not a changed FPS label, a silent log, or a build that merely compiles.

---

## Phase 1 handoff — profiling and build foundations (2026-09-08)

**Status: phase concluded; source implementation is ready for further validation, not a completed 60-fps optimization.** The original plan above is preserved unchanged. Its line references and baseline statements describe the planning snapshot; this handoff records the newer working tree.

**Latest user instruction:** conclude this phase, preserve the plan and progress in the repository, and do not generate further `.so` files. Both ongoing core-build processes were stopped. One intermediate ARM64 `.so` had already been produced; it is not a validated final artifact. Generated intermediates/test executables remain under `build/` and must not be committed as release binaries. No deployment, commit, push, YAGE modification, toolchain installation, or WSL configuration change was performed.

### Implemented in this phase

- [x] Added bounded frame-time histograms with explicitly labelled p50/p95/p99 upper bounds, deadline misses, SPU sample-frame totals, RHI prepare time, and Linux/Android emulation-thread CPU-time measurements.
- [x] Added one-time runtime metadata: core revision/dirty marker, ABI, pointer width, compiled NEON support, renderer, native AV timing, worker status, mapping mode, compiler-worker count, and NDK/compiler identity.
- [x] Added opt-in CPU/JIT diagnostics: quantum progress, exit flags, first-pass/interpreter activity, generic memory-wrapper calls, invalidation, compile requests/completions/failures/time, queue occupancy, and code-cache reclamation.
- [x] Added per-event counts and sampled inclusive timings; nested event-dispatch wall time is not summed twice. Added direct GPU-update/zero-elapsed and DMA-update counts.
- [x] Added opt-in Vulkan sync-index wait, frame-context, scanout, presentation flush, and pipeline-creation timings. These are CPU wall timings, not GPU timestamp measurements; nested categories are not additive.
- [x] Removed unconditional clock reads from the staged-inline GP0 path when command timing is disabled. Command order, worker threshold and emulated timing are unchanged. No device-level speed gain has been measured.
- [x] Added Vulkan-host benchmark mode with warmup exclusion, bounded timing storage, JSON output, native SDR/PGXP-off defaults, reported AV timing, audio-frame accounting, and no host hashing/readback/dumps. Its conservative per-frame queue-idle synchronization is explicitly reported and is not an asynchronous frontend-performance benchmark.
- [x] Added optional checked PCM16 stereo WAV capture to the Vulkan host, separate from benchmark mode, plus mock/helper regression tests.
- [x] Added Android compiler-target/ABI-based Makefile detection, Android/JNI release-flag parity, build IDs, static-C++ runtime selection, cached dirty-revision metadata, and optional `OBJECT_DIR` isolation. Explicit overrides remain supported.
- [x] Added a portable histogram self-test and expanded the recompiler test harness for profiling/reset/concurrency behavior.
- [x] Added an opt-in `BEETLE_PSX_PROFILE=1` compile define that changes the diagnostic option's default and missing-variable fallback to `timing`. Ordinary builds still default to `disabled`; explicit frontend choices still win. Use a distinct object directory when changing compile flags.
- [x] Preserved native clocks, EventCycles 128, SPU sample-update size 1, full invalidation, existing compiler-worker policy, GPU staging threshold, interpolation/reverb/CD audio behavior, save-state guest layouts and libretro-facing APIs.

### Verification actually performed

| Check | Result and limits |
|---|---|
| C histogram self-test | **Passed** natively on Windows using the existing NDK compiler/linker to build a freestanding test DLL, then calling its test entry with Python ctypes; return code 0 |
| Android build-policy regression script | **7 test methods passed** after repairing Windows mock-shell execution; this run began before the final isolated-object/static-runtime follow-ups |
| Latest focused build-policy follow-ups | **3 passed:** isolated object directories, shared-C++ runtime override, explicit build overrides |
| ARM64 SPU/worker/affinity/GPU-timing/recompiler/performance harnesses | **Cross-compilation passed** for all six executables; they were **not executed** |
| Vulkan host | An ARM64/API-24 snapshot **cross-compiled**, with warnings; subsequent AV-reporting/default refinements still require a fresh compile and runtime checks |
| Full core builds | Started ARM32/ARM64 compilation; **stopped on user request**. An intermediate ARM64 library exists, but no final-tree per-ABI build/export/dependency/page-alignment or device validation is claimed |
| `git diff --check` | **Passed** for the implementation tree before the documentation handoff; recheck before committing |
| Native POSIX/SPU/recompiler/Vulkan mock suites, sanitizers, real GPU matrix | **Not run**: no usable native POSIX runner was supplied; WSL is unavailable |
| TV/phone gameplay, clean audio, thermal endurance, FPS gains | **Not measured**: adb had no connected devices |

The GL-host sources were blocked by IDE ignore rules, so they were not modified. Do not bypass the ignore rules; obtain access if GL-host changes are still needed. The user selected an existing test runner but has not provided its path.

### Remaining work in every phase

| Phase | Current state | Next work |
|---|---|---|
| **A — Baseline/profiling** | Core/Vulkan-host instrumentation implemented; validation and Gate A remain open | Run the native regression suites; check final host/profile integration; obtain labelled cold/warm heavy-scene captures; correlate wall versus thread CPU time. Add finer compilation-lock, mapping, CD-wait or tracking detail only if the capture requires it. First-pass/block counters are not an exhaustive compiled-instruction percentage |
| **B — Android builds** | Build-system fixes and policy tests implemented; final per-ABI validation deferred | Re-run the complete latest policy matrix; when builds are requested again, verify all ABI variants, final flags, exports, dependencies, 16 KB alignment, and normal-versus-profile defaults. Test JNI builds and additional build hosts. O2/O3/LTO/ISA comparisons remain unperformed |
| **C — JIT/memory** | Diagnostics only; no new execution fast paths | Use the profile to choose guarded memory/helper/register/GTE work; add failing regression probes, then verify semantics on ARM32/ARM64 and full/fallback mappings before any optimization ships |
| **D — Event scheduler** | Counts/timings added; deadlines unchanged | Identify redundant/idle work; add scheduler/DMA equivalence coverage before an idle fast path or coalescing change. EventCycles and larger SPU update experiments have not been run and are not approved defaults |
| **E — Compiler concurrency** | Profiling and tests added; worker count unchanged | Measure queue contention and cold/warm behavior; compare one/two/current workers only after native tests and captures. Do not assume sleeping workers cause slow gameplay |
| **F — GPU/rendering** | Profiling added and disabled-diagnostics GP0 timer overhead removed | Compare actual inline/worker work, queue waits and finalization; add batch-reason/distribution instrumentation if needed; test bounded thresholds only with PIO/DMA/FIFO/VRAM equivalence and real Vulkan validation. Pipeline/cache/resource optimizations remain pending |
| **G — Audio/CD** | Output accounting/WAV tooling added; audio synthesis unchanged | Execute SPU/IRQ/ENDX regressions, validate WAV/sample totals and physical audio, correlate underproduction with core rate, profile bounded CD read-ahead/storage stalls. No SPU batching/quality/read-ahead defaults changed |
| **H — Broad low-end devices** | Capability matrix specified; no new devices tested | Test ARM32/ARM64 TVs across vendors, other Mali/Adreno drivers, phones, low-memory and non-Vulkan/software paths. Establish minimum supported capabilities with a varied game suite and thermal runs |
| **Artifact/integration milestone** | **Deferred by user** | Do not generate, deploy or package cores now. Resume only when requested, after final validation and an explicit artifact/revision choice; YAGE's stale manifest pin remains unchanged |

### Next input needed to resume

1. **Existing native runner:** provide the MSYS2/MinGW/Linux compiler or runner path so the SPU/recompiler/worker and Vulkan mock tests can actually run. A successful cross-compile is not a runtime pass.
2. **New TV logs after your own build/integration:** enable `beetle_psx_hw_gpu_diagnostics=timing` (or use the diagnostic-default compile define above). Confirm the capture contains `core_profile_runtime_v1`, `core_profile_v1`, `core_profile_cpu_v1`, `core_profile_jit_v1`, `core_profile_event_v1` and, for Vulkan, `core_profile_renderer_v1` lines.
3. Keep native resolution, PGXP off, native clocks, full invalidation, EventCycles 128 and SPU update size 1 unchanged. Capture title/menu, a repeatable active heavy match and loading/FMV transitions with scene labels. Collect several 300-frame diagnostic windows from the same heavy scene.
4. Capture the same scene with diagnostics disabled as the overhead control. If YAGE exposes GPU threading, optionally repeat with that setting disabled; otherwise do not change the frontend merely to obtain this comparison.
5. Include TV/phone model, SoC if known, Android version, process ABI, GPU/driver, audio route and whether the device was cold or warmed. A brief description or recording of the audible fault helps distinguish low pitch, repetition and crackle.

Example capture command, run after opening YAGE but before launching the game, using a fresh output filename:

```powershell
adb logcat -v threadtime -T 1 -s flutter > tv_phase1_profile.txt
```

Stop capture with Ctrl+C after the labelled scenes. Send that file plus the corresponding full logcat if it contains driver/system errors. Do not overwrite the original baseline logs.

**Resume gate:** first finish validation and interpret the new measurements; then choose the smallest justified Phase C/D/E/F change. Do not skip directly to relaxed timing or claim the 60-fps/clean-audio goal is achieved.
