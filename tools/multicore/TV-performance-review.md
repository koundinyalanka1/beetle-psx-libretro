# Plan: native-speed PS1 with clear audio on Android TVs and phones

Updated 2026-09-08 from the supplied September 8 capture, after a full re-read of both logs. **This is the current plan; 60 FPS with clear audio is not yet achieved.** Implementation has now covered portable GPU arithmetic, an idle-DMA channel fast path, idle-DMA event-deadline sharing and an adaptive GP0 staging bypass. Earlier FIFO work and Phase 1 verification are recorded at the end as historical results; fresh results appear in the current implementation records below.

**Latest user constraint: do not profile any device; all logic must be device-neutral.** Continue through source analysis, deterministic differential tests, sanitizers and cross-compilation. Do not run the device capture procedures below or introduce model-specific settings, affinity changes or timing relaxations. Those earlier measurement procedures are retained as reference only. Actual TV FPS and physical audio remain unverified; that limitation does not block portable, semantics-preserving code improvements.

The implementation scope remains this core repository. YAGE source is a read-only integration reference. No release `.so` generation or deployment is part of this work.

Optimize this Beetle PSX fork as a drop-in libretro core using measured CPU/JIT, event scheduling, rendering, and streaming improvements, while preserving native game timing and leaving YAGE source unchanged.

## Summary

The target is **low-end Android TVs generally, across manufacturers**, plus low-end Android phones - not a Sony/BRAVIA-specific patch. BRAVIA BF1 is the supplied measured example, not a device whitelist or the assumed minimum hardware.

The current capture is **PID 22173, `56134fb0-dirty`, Tekken 3 (USA)**, and it contains **31 complete periodic core windows**, each carrying a `retro_run` split, a CPU/quanta/event line, a worker-cost line and a GPU sync-cause line. An earlier reading of this file treated the periodic profile as absent because the `core_profile_*_v1` record names do not appear; the equivalent human-readable records do, 31 times each, and they carry the numbers this plan now turns on. What is genuinely missing is only the **timing** inside those records: `event timing off` and `per-word timing off` mean event dispatch cost and inline GP0 cost are still unpriced.

Four structural facts come out of those 31 windows, and they reorder the plan:

1. **The emulation thread has no headroom anywhere in the capture.** The cheapest sampled window still costs **15.30 ms of CPU** per frame against a 16.683 ms NTSC period, with only 223 GP0 words and 11 GPUSTAT reads in it (`tv_logs.txt:546`). The heavy plateau costs **28.1-29.5 ms**. So GPU-side work alone cannot reach the target; the CPU/JIT/event path has to come down as well.
2. **Two independent 128-cycle housekeeping grids drive the frame.** Every window reports ~**9,966 dispatched events** and **8,033-9,304 lightrec quanta**. A 59.94 Hz frame is 565,053 guest cycles, so a single `EventCycles = 128` grid implies ~4,414 events; GPU and DMA each run one, and together with ~735 SPU/CDC deadlines and ~400 timer/FIO events they account for essentially the whole count. The quanta figure is the damaging one: it is close to **twice** the 4,414 a single grid would cost, so the JIT is entered and left roughly every **70 guest cycles (~35 instructions)** all frame, paying dispatcher entry/exit and an event-list walk each time.
3. **GP0 staging never engages in the scene that misses the budget.** Across the plateau windows the worker reports **busy 0.00 ms/frame**, queue depth 0, and **every one of ~14,357 GP0 words/frame executed inline**. The guest interleaves ~14 words with a readiness poll, so the 1,024-word publication threshold is never reached: ~**1,032 DMA-ready polls/frame of which ~98% found staged words and forced a resolve**, and only ~20 answered from the published snapshot. A later scene in the same session behaves the opposite way (13 GPUSTAT reads, 1,230 of 1,405 polls answered from publication), which shows this is guest behaviour, not a device property.
4. **Status polling is heavy and almost never has anything to wait for.** GPUSTAT reads run **1,485-1,955/frame** through the plateau and **5,384/frame** in the first window, with a **collapse rate of ~0.6%** - the worker essentially never has outstanding work when the guest asks. Every one of those reads currently pays a diagnostic predicate plus a full worker-sync path of acquire loads before computing the status word.

A sustained plateau section supplies about **34 emulated frames/s** and presents about **17 images/s**; one 300-frame window later reaches **39.05 ms**, and one frontend window falls to **19.9 runs/s with audio starvation**. Audio underproduction follows directly: `spf ~736` at 34.2 runs/s is ~25,171 stereo frames/s against 44,100, about **57%**, and the frontend logs a **500-permille** elastic rate change. There is no audio fix that is not a core-speed fix.

This is a staged optimization program, with correctness tests deciding which changes ship. It is **not a promise that every low-end TV can run every PS1 game at full speed**. If the measured hardware floor remains above some devices, report that limit rather than substitute slowed audio, inaccurate emulation, or skipped presentation for success.

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

## 2. Current capture: evidence and limits

### Identity and reproducibility

The two inputs are under `/Users/koundinya/flutter_projects/yage/`. Line references below use newline-delimited file lines; the full log contains non-UTF-8 bytes, so preserve the original bytes and decode with replacement only for analysis.

| Input | Lines | SHA-256 |
|---|---:|---|
| `tv_logs.txt` | 727 | `542086536269ef1c2fa6814dedeeaef9b64f187d439e5bdb274ebfa5fb686051` |
| `full_logcat_tv.txt` | 77,566 | `08400f182ba9ed28947c02cbc13cf914ab850f82a8b34dfc6573eaaf554d5bc9` |

- Reviewed checkout HEAD: `56134fb0ea675c4f38a6f9ae4dca107f760cdc0a`; working tree was clean before this documentation edit.
- Runtime identifies `0.9.44.1-GLES3 56134fb0-dirty` (`tv_logs.txt:152`, `full_logcat_tv.txt:74513`). The matching revision prefix does **not** identify the dirty patch, exact compiler flags or installed library hash. Obtain the APK/core hash, ELF build ID, source diff and build command for paired tests. The compiler version's `+pgo/+lto` text describes the compiler build; it does not prove that the core used PGO/LTO.
- PID **22173**, emulation TID **22776**; startup at **09-08 21:10:37–38**. The selected plateau health endpoints are **21:11:28.312–21:13:48.766** (`full_logcat_tv.txt:74988–76328`). These are log timestamps, not independently verified UTC times.
- The full log's early `init` SIGABRT is a different process. No same-session fatal signal or explicit Vulkan device-loss record was found. Handler installation, property-access warnings and unmapped guest-access messages are not evidence of a fatal core crash. The small log later records an orderly frame-loop stop (`tv_logs.txt:640–641`).
- YAGE's `scripts/libretro_cores.json:145–151` still pins `b0b900f8d324a4ae4976012849da366905b2f033`. Preserve the tested artifact independently; packaging can otherwise restore an older core. The capture identifies release mode; the exact YAGE revision/build configuration must also be recorded because frontend behavior is part of the comparison.

### Effective settings

Sources: `tv_logs.txt:152–266`, `289–318`, and `330–332`.

- BRAVIA BF1, launched in release mode (`tv_logs.txt:32`), four reported homogeneous CPUs, Mali-G52, ARM32 process and compiled NEON support. The CPU log prints `1530000 kHz` (approximately 1.53 GHz); record actual runtime frequency/DVFS separately from this reported capability.
- Vulkan/GPU presentation, native 1x, PGXP off, software framebuffer off; texture tracking/dumping/replacement disabled. HD cache budget option values are not proof that those amounts were allocated.
- Lightrec `execute`, full invalidation, opcode cycles 2, SP/GP off; `EventCycles=128`, `spu_samples=1`; GPU and SPU workers on. Startup reports three compiler workers and `mapping=4`.
- In this source, `mapping` is `psx_mmap`, a successful mapping count, not a generic fallback enum. `lightrec_init_mmap()` and `CPU` initialization use 4 for the complete mirrored mapping. **Do not prioritize a missing-mapping repair for this capture.** Generic accesses can still exist and need counters/sampling.
- Runtime video reports 59.94 Hz initially; AV updates can change the period. Audio is 44,100 Hz stereo; OpenSL ES uses six 512-frame buffers and reports a 500–2000 permille rate range.
- YAGE first logs a full mask, then **pins the emulation thread to CPU 3** (`tv_logs.txt:296`, full log `74665`) and sets nice=-8. Core worker startup reports four eligible CPUs. A startup eligibility count does not establish every thread's effective runtime mask, scheduling priority or residency. Inspect emulation, SPU, GPU and compiler threads separately. Preserve the current frontend policy during the first paired comparisons.

### Measurements: the complete periodic record

All 31 periodic windows from `tv_logs.txt`, in order. Each covers 300 emulated frames; the windows are not equal in wall time and must not be averaged into one FPS number. `ms` columns are window means, not frame percentiles. GPUSTAT/FIFO/DMA-ready are per frame; the parenthesised figures are the sub-counts the core already reports.

| line | CPU ms | fin ms | quanta | us/q | events | GP0 words | GPUSTAT (collapsed) | FIFO exact | DMA-ready (collapsed) | SPU busy |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 341 | 26.01 | 0.98 | 8342 | 3.12 | 9908 | 16 | 5384 (0) | 0 | 0 (0) | 1.35 |
| 347 | 15.77 | 0.10 | 8791 | 1.79 | 9879 | 2 | 652 (0) | 0 | 0 (0) | 1.11 |
| 360 | 17.88 | 2.68 | 8747 | 2.04 | 9946 | 524 | 368 (0) | 173 | 217 (43) | 0.99 |
| 367 | 27.31 | 0.57 | 8234 | 3.32 | 9966 | 12207 | 2116 (9) | 30 | 899 (868) | 1.00 |
| 372 | 27.97 | 0.29 | 8172 | 3.42 | 9966 | 14357 | 1710 (11) | 14 | 1031 (1017) | 1.01 |
| 378 | 28.42 | 0.28 | 8131 | 3.50 | 9966 | 14357 | 1941 (10) | 18 | 1032 (1014) | 1.00 |
| 384 | 28.45 | 0.28 | 8087 | 3.52 | 9965 | 14458 | 1955 (9) | 33 | 1040 (1006) | 1.01 |
| 391 | 28.41 | 0.29 | 8107 | 3.50 | 9966 | 14354 | 1928 (11) | 19 | 1032 (1012) | 1.02 |
| 398 | 28.10 | 0.30 | 8125 | 3.46 | 9963 | 14364 | 1722 (10) | 22 | 1033 (1010) | 1.00 |
| 405 | 29.02 | 0.32 | 8140 | 3.57 | 9965 | 14350 | 1766 (10) | 23 | 1031 (1008) | 1.05 |
| 412 | 28.49 | 0.30 | 8073 | 3.53 | 9964 | 14402 | 1898 (9) | 19 | 1036 (1017) | 1.00 |
| 418 | 28.46 | 0.30 | 8043 | 3.54 | 9964 | 14357 | 1800 (11) | 22 | 1031 (1009) | 0.99 |
| 425 | 28.06 | 0.30 | 8125 | 3.45 | 9965 | 14357 | 1770 (11) | 24 | 1031 (1007) | 1.01 |
| 432 | 28.02 | 0.30 | 8175 | 3.43 | 9965 | 14357 | 1641 (9) | 21 | 1032 (1010) | 1.01 |
| 438 | 28.41 | 0.28 | 8094 | 3.51 | 9965 | 14357 | 1804 (9) | 22 | 1033 (1010) | 1.01 |
| 445 | 28.20 | 0.29 | 8033 | 3.51 | 9964 | 14357 | 1762 (10) | 23 | 1032 (1009) | 1.00 |
| 452 | 28.22 | 0.31 | 8110 | 3.48 | 9964 | 14357 | 1706 (11) | 21 | 1031 (1010) | 1.02 |
| 459 | 28.14 | 0.31 | 8123 | 3.46 | 9963 | 14456 | 1485 (10) | 15 | 1039 (1024) | 0.99 |
| 465 | 28.26 | 0.30 | 8099 | 3.49 | 9963 | 14357 | 1764 (9) | 23 | 1032 (1009) | 1.00 |
| 472 | 28.80 | 0.30 | 8094 | 3.56 | 9965 | 14357 | 1875 (10) | 23 | 1032 (1009) | 1.02 |
| 479 | 28.76 | 0.31 | 8108 | 3.55 | 9961 | 14357 | 1675 (10) | 29 | 1032 (1003) | 1.02 |
| 486 | 28.40 | 0.30 | 8133 | 3.49 | 9964 | 14402 | 1822 (10) | 20 | 1034 (1014) | 1.01 |
| 494 | 23.72 | 2.76 | 8358 | 2.84 | 9966 | 8754 | 1201 (5) | 309 | 943 (633) | 0.99 |
| 506 | 17.17 | 0.70 | 8548 | 2.01 | 9967 | 445 | 15 (0) | 438 | 549 (111) | 1.04 |
| 515 | 26.58 | 4.92 | 8614 | 3.09 | 9966 | 676 | 16 (0) | 797 | 913 (115) | 1.04 |
| 521 | 19.36 | 0.86 | 8669 | 2.23 | 9966 | 740 | 13 (0) | 1231 | 1406 (174) | 1.03 |
| 528 | 19.26 | 0.85 | 8670 | 2.22 | 9966 | 740 | 13 (0) | 1230 | 1405 (174) | 1.02 |
| 534 | 19.23 | 0.85 | 8669 | 2.22 | 9966 | 739 | 13 (0) | 1230 | 1405 (174) | 1.01 |
| 540 | 18.48 | 0.67 | 8828 | 2.09 | 9967 | 604 | 12 (0) | 940 | 1074 (134) | 1.03 |
| 546 | 15.30 | 0.04 | 9304 | 1.64 | 9966 | 223 | 11 (0) | 12 | 27 (15) | 1.02 |
| 552 | 35.28 | 3.61 | 8216 | 4.29 | 9967 | 5067 | 24 (0) | 1649 | 2293 (630) | 1.09 |

Frontend health windows that pair with them: 29 windows in the plateau report run rate 32.9-34.8 Hz (median **34.2**) and presentations 16.2-17.4 Hz (median **17.2**); a later section reaches 47.5-47.8 runs/s at ~20.4 ms; one 6 s-labelled window collapses to **19.9 runs/s, run 49.7 ms, hold=1, underrun=28064** (`tv_logs.txt:510`); two windows report `healthy` (57.6 and 60.0 runs/s) with `iters/frame` of 31.4 and 34.4 and presentations of only 55.6 and 38.3 Hz.

### What the record establishes, and what it still cannot

**Established from counts alone, no timing required:**

- *Event cadence.* 9,966 events/frame is `2 x 4,414 + ~735 + ~400`. `DMA_Update()` re-arms on its own `EventCycles` grid unconditionally (`dma.c`, `CalcNextEvent`) and `GPU_Update()` re-arms on its own, clamped to `EventCycles` but shortened at every scanline boundary (`gpu_timing.h`, `GPU_NextEventDelay`), so the two grids drift out of phase and the CPU loop is interrupted by both. That is the 8,033-9,304 quanta.
- *Nested GPU updates.* `DMA_Update()` calls `GPU_Update()` first, so `GPU_Update()` runs roughly 8,800 times per frame for ~4,400 grid points of actual work.
- *Staging is inert in the heavy scene.* Worker busy 0.00 ms/frame, depth 0, 100% inline, 1,032 readiness polls with ~1,010 collapses. Fourteen words per batch against a 1,024 threshold.
- *Status polling is nearly free of real work.* 1,485-5,384 GPUSTAT reads/frame at a ~0.6% collapse rate.
- *Two distinct guest regimes exist in one session.* Plateau: GPUSTAT-heavy, publication useless. Later scene: 12-16 GPUSTAT reads, 1,230/1,405 polls answered from publication, 740 words/frame. Any staging policy must handle both without a device or title check.
- *Scanout image churn.* `img_create_seen` advances by **0.95-1.01 per presented frame** across 35 consecutive gameplay windows, and by ~0 in the two windows where the loop spins without producing frames. The core creates a Vulkan image per presented frame; `img_create_patched=0` says the frontend is not the one doing it.

**Still unmeasured, and not to be guessed at:**

- The split of `CPU_Run` between compiled execution, memory wrappers, GTE helpers, event dispatch and inline GP0. `event timing off` and `per-word timing off` in every window.
- Wall versus emulation-thread CPU time, so scheduler/preemption effects are invisible.
- Frame p95/p99. The health windows carry means only.
- The cost per CPU-loop exit, which is what prices the event-cadence work. Estimating it needs one paired capture with `beetle_psx_hw_gpu_diagnostics=timing`, which the no-profiling instruction defers.

Because of the last point, **no ms figure is claimed for any change in this plan**. Structural results are reported in exits, events, polls and words, which the host harnesses measure exactly.

### Sizing the gap

The 28.89 ms example needs **42.3% less time (1.73x throughput)** to reach the 16.683 ms native period, or **51.5% less (2.06x)** to reach 14 ms. The late 39.05 ms window needs **64.1% less (2.79x)** to reach 14 ms. The 15.30 ms floor window needs no throughput gain but has only **8% headroom**, which is the number that says the fixed per-frame cost is itself part of the problem. These describe the gap; they are not predicted savings, and both plateau measurements already include skipped scanouts, so final acceptance must remeasure with full video.

For a candidate affecting fraction `f` of serial frame cost with component speedup `s`, estimate `T_new = T_old x ((1 - f) + f/s)` before implementing it. Use only non-overlapping measured fractions.

### Audio diagnosis

- `spf_in≈spf_out≈736` is stereo **sample frames per emulated frame**, rounded in the health log. At the plateau median, supply is `34.2 × 736 ≈ 25,171` stereo frames/s, about **57.1%** of 44,100. The sustained deficit is approximately 18,929 frames/s. Correct samples per guest frame do not mean correct samples per wall second.
- A successful **500-permille** rate change is explicitly logged at `tv_logs.txt:355` / full log `:74852` (21:10:55.836). In the inspected frontend, proactive rate changes can happen without that message: `native/yage_audio.c:653–700` logs successful changes only when the elastic hold state is active. Do not assume 500 persists for every later window, or that silence in the log means the sink returned to normal.
- At 19.9 runs/s, supply is about **14,646 frames/s**, below even the half-speed sink's 22,050. Hold/repetition and starvation are therefore consistent with underproduction. Six 512-frame buffers hold only about 69.7 ms at normal speed; extra buffering cannot repair sustained missing throughput.
- `underrun` resets on each real stereo frame (`yage_audio.c:704–727`); 28064 is not a cumulative count of distinct audible dropouts. `audio=N/65535` is ring occupancy in scalar int16 samples, not stereo frames or an FPS measure. Trim/hold fields also have their own window/reset semantics.
- Preserve full SPU behavior. Worker busy time near 1 ms with generally small waits, and callback cost around 0.01 ms, do not support removing audio fidelity or adding another audio thread. Correct core PCM and normal physical sink playback are separate acceptance checks.

### Rendering and scheduling diagnosis

- GPU worker busy time, queue depth and queued words are **zero in all 31 windows**. Threading is enabled and doing nothing; every GP0 word runs on the emulation thread. Do not infer a renderer-thread speedup from `gpu_worker=1`.
- The reason is visible in the sync-cause line, not in the worker line: the guest drains between short bursts. ~1,032 readiness polls/frame with ~1,010 collapses, ~1,800 GPUSTAT reads/frame, ~14 words per batch. The 1,024-word publication threshold is unreachable in that regime, so the staging buffer is a copy in and a copy out with no handover at the end of it.
- Because `gpu_stage_count` is almost never zero, the published-readiness fast path in `GPU_DMACanWrite()` is also disabled almost always in the plateau (20 of 1,032), while in the later scene it answers 1,230 of 1,405. The same code, opposite outcome, decided entirely by what the guest is doing.
- Typical sustained finalize is 0.28-0.32 ms with half the scanouts skipped. Its complete removal would leave the core far over budget: even zeroing all non-CPU time from the 28.89 ms example leaves 28.42 ms. Finalize spikes to 2.68/2.76/3.61/4.92 ms in transition and loading windows and deserves separate tail analysis, not averaging.
- `readback=0`, `fence=0` and typical `present_wait` near 1 ms do not identify a frontend readback bottleneck. The initial `present_wait` maximum of 61.1 ms still warrants tail analysis. Frontend fences do not cover every internal Vulkan wait.
- **One Vulkan image is created per presented frame.** `img_create_seen` tracks `present_hz x window` at a ratio of 0.95-1.01 across 35 consecutive gameplay windows and stops advancing in the two windows where the loop produces no new frames. `img_create_patched=0`, so these are the core's own `vkCreateImage` calls. Allocation and memory bind per frame on a Mali driver is worth removing on its own terms; size it before assuming it explains the finalize spikes, since steady finalize is only 0.3 ms.
- Steady health windows show `slept=0.0 ms` and `iters/frame=1.0`. The old 3 ms sleep hypothesis does not explain this plateau. The two `healthy` windows have 31.4 and 34.4 iterations/frame with presentations well under the run rate; correlate them with frontend skip-mode hysteresis before reading them as core behaviour.
- CPU time is inclusive wall time; emulation, SPU and compiler worker times can overlap. Never sum them as serial costs, subtract differently aligned windows, or treat `to_set_image` as additive GPU-only work. In every window `to_set_image` tracks `run` within a millisecond or two, which says the frontend is not adding latency of its own - the core is the bottleneck.

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

- `cpu.c` re-enters Lightrec around event handling; code already copies COP0 primarily at frame entry/exit, not every quantum. Do not propose that already-implemented optimization again.
- `PSX_EventHandler()` in `libretro.c` dispatches and reorders due events. Existing diagnostics report inclusive event time and sampled per-type estimates.
- `dma.c` has its own `EventCycles` cadence, calls `GPU_Update`, runs MDEC, and scans seven channels. **As of the current implementation an idle controller now re-arms at the GPU's deadline instead**, so the two events coincide; an active channel and any CPU overclock keep the original grid. Do not re-propose that edit, and do not extend it to active channels without a differential harness - `RunChannel` discards leftover credit when a device blocks and is not subdivision-invariant.
- `gpu.c` has a separate GPU deadline and resolves staged commands on clock updates. The GPU deadline shortens at every scanline boundary, which is what used to drive the two grids out of phase.
- `cdc.c` also schedules SPU sample deadlines; `spu.c` uses 768 guest cycles per sample at the native setting.
- The arithmetic that ties this together: a 59.94 Hz frame is 565,053 guest cycles, so one 128-cycle grid is ~4,414 events. The capture's 9,966 events/frame is two such grids plus ~735 SPU/CDC deadlines and ~400 timer/FIO events. About 8,100 Lightrec quanta and 9,966 dispatched events are still not evidence of 9,966 SPU mixes; per-type counts remain the way to attribute cost.
- `EventCycles` also controls the drawing-credit limit (`gpu.c`). Raising it is not a purely host-side scheduling optimization.
- The current `gpu_timing.h` already avoids common ARM software divides while preserving the previous calculation. Optimize remaining measured work, not a stale version of this code.

### GPU staging detail

- `GPU_STAGE_MAX` is 1024; `GPU_Stage_GP0` publishes only at this threshold. In the plateau the guest interleaves ~14 words with a poll, so the threshold is unreachable and the buffer is a copy in and a copy out.
- `GPU_Stage_Resolve` executes short staged batches inline when the worker is idle (`gpu.c`). GPU clock updates and status/DMA synchronization repeatedly force resolution.
- Merely turning threading on therefore does not make this workload parallel. Conversely, forcing every short batch onto a worker can increase wakeups and immediate waits.
- **As of the current implementation** the staging decision is adaptive and lives in `gpu_stage_policy.h`: a run of short inline resolves stops staging, which also restores the published-readiness fast path, and one uninterrupted threshold-length run re-arms the worker. Do not re-propose a fixed threshold change; extend the policy and its replay harness instead.
- The worker can park on a published read-back with an **empty** ring while reporting itself idle. Any fast path that keys off "queue empty and worker idle" must still service the deferred read-back.
- Phase 1 already removed the unconditional staged-inline clock reads when timing is disabled. Verify its disabled-path overhead in paired captures; do not propose the same edit again.
- Software rendering currently disallows this worker because emulation-thread scanout reads live VRAM. GL uses deferred command recording; Vulkan can execute through the worker with queue locking. PGXP excludes the worker. Preserve these ownership constraints (`gpu.c`).

### JIT/build detail

- `deps/lightrec/recompiler.c` currently creates `logical_cpu_count - 1` compile workers, minimum one. On the captured four-core CPU this means three possible compiler workers in addition to emulation, SPU, renderer, frontend and system activity. Idle sleeping workers are not themselves a bottleneck; measure when they are runnable and contending.
- First-pass execution and compilation paths are in `lightrec.c:get_next_block_func`; cancellation can wait for a compile in flight. Code-cache exhaustion triggers reclamation.
- Memory emitters already include runtime guards. The fallback map can route dynamic accesses through generic C wrappers (`deps/lightrec/lightrec.c`, `deps/lightrec/emitter.c`). The current runtime reports complete mapping; profile generic traffic before treating missing mappings as a bottleneck.
- Android A32 emission/interworking is already explicitly selected in `deps/lightning/lib/jit_arm.c`. Preserve it. The captured SIGBUS handler installation does not prove faults occurred; profile actual fixups before blaming them.
- YAGE builds via the **root Makefile with `platform=unix` and NDK compilers**, not `jni/Android.mk`. Root release builds already append `-O3 -DNDEBUG` and preserve `-fwrapv -fsigned-char`. Adding these only to JNI would miss the captured deployment path.
- Phase 1 added compiler-target/ABI detection to the root Makefile. Verify that implementation with the existing build-policy matrix and effective final flags on each build host; do not repeat the completed edit.

## 4. Acceptance targets and measurement rules

### Proposed engineering gates
These are goals to validate, not measured results or guaranteed savings.

| Metric | Acceptance target |
|---|---|
| Emulation speed | Native reported rate within approximately ±0.5% over a stable multi-minute workload; no growing lag |
| Core frame-time headroom | Warm full-video p95 ≤14 ms, p99 below the current native period; deadline misses <1%, and no consecutive-miss burst that causes audio hold/dropout; report median, max and longest burst |
| Recovery from frameskip | Sustained core EWMA below `0.85 × native frame period` (about 14.2 ms here) so unmodified YAGE can leave its skip state |
| Native-60 visual output | Over each stable 5-minute scene, valid image submissions ≥99% of expected native-60 updates, no sustained skip mode or growing backlog; separately verify physical display cadence and account for known guest duplicates/refresh mismatch |
| Audio | Normal pitch and duration; effective sink rate within the frontend normal DRC band (approximately 995–1005 permille), no elastic slowdown/hold or audible dropout during steady acceptance; exact PCM counts checked separately from rounded health fields |
| Thermal endurance | Repeat acceptance after 20–30 minutes; no progressive performance or memory collapse |
| Compatibility | No new state/audio/IRQ/render mismatches in differential tests; representative games and lifecycle cases pass |
| Production instrumentation | No per-word/per-event clock reads or disk writes unless diagnostics are explicitly enabled; quantify overhead of the disabled path |

The current 28.89 ms example needs **42.3% less time (1.73× throughput)** to reach the initial 16.683 ms native period, or **51.5% less time (2.06× throughput)** to reach 14 ms. The late 39.05 ms window needs **64.1% less time (2.79× throughput)** to reach 14 ms. These describe the gap; they are not predicted savings. Because both measurements include skipped scanouts, final acceptance must remeasure with full video.

For a candidate affecting fraction `f` of serial frame cost with component speedup `s`, estimate `T_new = T_old × ((1 − f) + f/s)` before implementing it. Use only non-overlapping measured fractions; report uncertain or overlapping buckets separately. A faster microbenchmark is not enough when its maximum possible saving cannot materially close the device gap.

### Benchmark protocol

1. Use identical core, frontend, content, BIOS choice, options, input sequence, scene and starting state for paired tests.
2. Label boot, title/menu, attract/demo, active match, character/stage changes, loading, FMV, and return-to-menu separately. Do not average them into one FPS number.
3. Measure cold launch/JIT/shader warmup and warmed repeatable gameplay separately. Use at least 60 seconds of warmup and continue until compile/pipeline activity stabilizes; label any scene that never stabilizes. Record compile/cache state and temperature; a saved state is not automatically a warmed JIT cache.
4. Run baseline/candidate/baseline in alternating order, with at least three repeats, so DVFS/thermal variance is visible.
5. Use ordinary release builds for final speed comparisons. Diagnostic, sanitizer, validation-layer, per-frame hashing and readback builds are correctness/profiling tools, not shipping-performance evidence.
6. Collect monotonic timestamps and align core counters with YAGE health windows. Record both wall time and emulation-thread CPU time where available; use CPU sampling plus scheduler traces for preemption, futex waits, memory faults and driver time.
7. Use application/process-filtered captures and bounded log volume. The full log and later TV log contain extensive input/system logging; its impact is a measurement confounder, not an established cause of the CPU bottleneck.
8. Save machine-readable benchmark output outside source code. Keep raw captures and symbolized binaries associated by build ID and SHA-256.
9. Use ≥5 minutes per steady acceptance scene and repeat after a 20–30 minute thermal soak. Keep boot/loading/stall tests separate and retain them in the report; do not discard their misses as outliers. Record three paired repeats, individual run results, spread and aggregate statistics. Promote only gains exceeding observed baseline-to-baseline variation with no material p99/audio regression.
10. The core histogram uses 250 us bins (`mednafen/performance.h`); its percentiles are upper bounds. Do not infer frame p99 from p99 of 300-frame averages, or average window percentiles into a session percentile. Aggregate histogram bins or use bounded raw timing data. For renderer/event/compile `_us` counters, check units: they are totals over the report interval, not automatically ms/frame. `compile_wall_us` can overlap across workers; per-event sample totals require sample counts and remain inclusive.
11. For steady PCM accounting, compare emitted stereo frames with elapsed **guest cycles/time** at 44,100 Hz, carrying fractional sample phase across boundaries. Do not multiply the rounded 736 by frame count and require equality. When only frontend totals are available, allow rounding/boundary error and report that limit. A 5-minute wall interval at native speed should contain about 13,230,000 stereo frames, subject to measured clock drift; it must not be padded to reach that number.

## 5. Implementation steps, priorities and decision gates

### Immediate execution order

Phases below are a conditional backlog, not eight mandatory rewrites. **The device-measurement tasks in the original order are suspended by the latest user instruction.** The active order is native validation -> portable code simplification -> differential/sanitizer tests -> cross-compilation and code-generation checks -> update results. Do not infer a TV speedup from host checks.

With the periodic record now read in full, the ordering is driven by what the counts prove rather than by what is easiest to reach:

| Order | Work | Evidence it rests on | State |
|---|---|---|---|
| 1 | **Event cadence (D).** Stop the idle DMA controller from adding a second 128-cycle grid, so the CPU loop exits once per quantum instead of twice. | 9,966 events and ~8,100 quanta per frame against the ~4,414 a single grid implies | **Done** - see the implementation record. 41.3% fewer exits in the scheduler harness, emulated outcome identical |
| 2 | **GPU staging (F).** Stop staging when the guest's own polling makes a handover impossible, which also restores the published-readiness fast path. | Worker busy 0.00 ms/frame, 100% inline, ~14 words per batch, 20 of 1,032 polls answered from publication | **Done** - see the implementation record |
| 3 | **Status/readiness path (F).** Remove the duplicated barrier predicate and the worker-sync walk from polls that provably have nothing to wait for. | 1,485-5,384 GPUSTAT reads/frame at a 0.6% collapse rate | **Done** - folded into item 2's invariant |
| 4 | **Price the remaining CPU path (A/C).** The split of `CPU_Run` is still unknown, and every further JIT decision needs it. Build it on the host, not the device: replay the recorded quantum/event/word mix through the real sources and report the split. | `event timing off`, `per-word timing off` in all 31 windows | Next |
| 5 | **Scanout image reuse (F).** One `vkCreateImage` per presented frame. | `img_create_seen` tracks presentations at ratio 0.95-1.01 over 35 windows | Next |
| 6 | **JIT memory/GTE work (C).** Only after item 4 says which of compiled execution, memory wrappers or GTE helpers dominates. | Not yet attributable | Blocked on 4 |
| 7 | **Compiler concurrency (E), audio/CD (G), cohort coverage (H).** | Worker busy ~1.0 ms/frame with small waits; no compile churn evidence | Unchanged priority |

Decision rules for item 6: large due-event/self time -> exact idle/redundant scheduling work; large compiled RAM/helper/GTE cost -> guarded JIT/helper work; repeated compile/reclaim/first-pass activity -> compile/cache policy; large wall-minus-thread-time gap -> scheduler/locks/driver/storage trace. Sampling must distinguish CPU executing in the kernel from off-CPU waits.

**No fixed savings are assigned to unmeasured candidates.** After each accepted patch, update the residual full-video frame budget and choose the next largest measured cost. If useful gains plateau above the target, publish the measured hardware limit and remaining profile.

### Phase A — Reproducible baseline and honest profiling (P0)

**Files:** `libretro.c`, `mednafen/psx/cpu.c`, relevant Lightrec sources, `tools/vkhost/vkhost.c`, `tools/glhost/glhost.c`, `tools/multicore/Makefile`.

1. Validate the implemented once-per-session revision/dirty, NDK/compiler, ABI/SIMD, renderer/mapping, worker-count and option metadata against the installed artifact; add missing target/build provenance to the run manifest.
2. Validate the implemented bounded histograms and skip/duplicate counters. Add only missing valid-image, deadline-burst or interval correlation fields; use the existing diagnostics rather than a separate always-on profiler.
3. Exercise the implemented event/JIT counts, guest-cycle progress, exits, first-pass/interpreter calls, compile queues, invalidation and reclamation records. Add finer attribution only for unresolved costs; first-pass block counts are not a compiled-instruction percentage.
4. Measure direct and nested GPU/DMA calls without double-counting. Existing per-type event estimates are inclusive: DMA calls GPU; document this in the report format.
5. Break down RHI prepare/finalize into CPU work, resource reuse waits, pipeline creation, scanout, flush/queue-lock time and optional texture-tracker work.
6. Validate the implemented Vulkan-host WAV capture and benchmark mode. Benchmark mode disables dumping/validation/hashing/readback but still uses a conservative per-frame queue-idle barrier; report that limitation. It does not play through the real TV audio sink or reproduce YAGE pacing.
7. Compare diagnostics off/on overhead, including the now-guarded staged GP0 timers. Keep heavy timing/counters compile-time or runtime opt-in as appropriate; do not call frontend logs from JIT inner loops or audio workers.

**Gate A:** A reproducible heavy scene has a defensible cost breakdown. The next code change must name the measured component it targets. If hardware/profiling access is unavailable, complete instrumentation/tests but do not claim an optimization win.

### Phase B — Portable, consistent Android release builds (P0/P1)

**Files:** `Makefile`, `Makefile.common`, `jni/Android.mk`, `jni/Application.mk`; tests under existing tools where appropriate.

1. Validate Phase 1 compiler/ABI-based Android architecture decisions and explicit overrides, on both the `platform=unix` NDK route and `platform=android`. Fix demonstrated gaps rather than repeating target-detection work.
2. Verify final effective C/C++/link flags rather than editing flags blindly. Preserve release optimization and integer behavior flags; compare `-O2`/`-O3`, host Thumb/A32 choices and optional ThinLTO only with measurements and differential tests.
3. Separate host compilation instruction-set choices from the JIT emitter's Android A32 requirement. Do not re-enable JIT Thumb merely because host Thumb is compact.
4. Confirm ARM NEON paths are compiled on supported targets. For any declared legacy non-NEON support, retain scalar baseline or dispatch only separately compiled SIMD code after a reliable feature check; do not make a supposedly portable ARMv7 binary execute NEON unconditionally on unsupported CPUs.
5. Keep ARM64 baseline instructions portable; runtime-gate optional extensions. Do not use `-march=native` or tune every TV binary to BRAVIA's SoC.
6. Bring JNI builds into semantic parity where necessary, but do not mistake JNI-only changes for fixes to the captured root-Makefile build.
7. Emit build ID and verify exports, architecture, dependencies, stack budget and 16 KB LOAD alignment. Keep unstripped symbols separately.

**Gate B:** Same source/options behave consistently across build hosts and ABIs. Existing `-O3`, enabled JIT and known interworking repairs are baseline facts, not claimed new FPS gains.

### Phase C — JIT execution and memory-access critical path (P1, gated on attribution)

**Files:** `mednafen/psx/cpu.c`; `deps/lightrec/{lightrec.c,emitter.c,optimizer.c,regcache.c,blockcache.c,recompiler.c}` as indicated by the profile; `deps/lightning/lib/jit_arm.c` only for demonstrated backend defects.

The capture says two things about JIT execution and stops. First, the loop is *entered and left* about 8,100 times per frame, roughly every 70 guest cycles or 35 instructions, which is what Phase D item 1 attacks - `lightrec_plugin_execute()`'s per-quantum body is already lean (COP0 is synchronized once per `CPU_Run`, not per quantum), so the cost per exit is the dispatcher prologue/epilogue and the register-cache reload inside `lightrec_execute()`, not the C around it. Second, **nothing in the capture attributes the remaining time**, because `event timing off` covers the whole dispatch path and `per-word timing off` covers inline GP0. Per-quantum cost ranges 1.64-4.29 us across the 31 windows while the quantum count barely moves, so the variation is real work, not fixed overhead - but which work is unknown.

Do not start rewriting emitters on that basis. The next JIT step is attribution:

1. **Build the attribution on the host.** Replay the recorded mix - ~8,100 quanta, ~9,966 events, 223 to 14,458 GP0 words per frame - through the real sources and report the split between compiled execution, memory wrappers, GTE helpers, event dispatch and inline GP0. Relative splits transfer across hosts far better than absolute times, and this is allowed where device profiling is not. `tools/vkhost` already runs the real core; extend it or add a focused harness rather than inventing a second frontend.
2. Only then: if fallback mappings cause generic RAM traffic, extend the existing **guarded** RAM path to cover proven common accesses on both mapping layouts, retaining an exact slow path for MMIO, BIOS, scratchpad, cache-isolated RAM and unmapped regions. The runtime reports `mapping=4`, the complete mirrored mapping, so do not prioritize a missing-mapping repair for this capture.
3. Preserve load/store width, sign extension, alignment, LWL/LWR/SWL/SWR behavior, wrap/mirror semantics, COP2 effects, and the baseline load-delay model. Validate a register whose address changes from RAM to MMIO across executions; never specialize from a single observed address without guards.
4. Keep self-modifying-code invalidation correct for CPU stores, DMA, memory clears, aliases and writes crossing code/page boundaries. Do not set DMA-only invalidation globally.
5. Reduce redundant work at JIT/event/helper boundaries only where state ownership is clear. Frame-level COP0 synchronization must remain correct under IRQs, syscall exits, save/load and interpreter fallback.
6. Preserve completed unmapped-access recovery. The capture logs four `Guest access to unmapped address at PC 0x00003774` warnings before self-silencing; the *logging* is silenced, the exits are not, and their frequency is uncounted. Add a counter for `LIGHTREC_EXIT_SEGFAULT` exits to the existing opt-in CPU diagnostics so the next capture answers this instead of leaving it open.
7. If GTE helpers dominate, optimize their existing integer math/SIMD/register transfer with equivalence tests. Do not remove GTE latency or alter guest clocks. If the attribution does not implicate GTE, defer it.
8. Fix any demonstrated host unaligned accesses rather than relying on a signal handler as normal execution. The capture reports the SIGBUS handler installed and kernel fixups unavailable (`errno=22`), which is a reason to check, not evidence that faults occurred. Require real ARM32 tests.

**Tests:** Extend generated test ROM probes and host state/hash comparisons; add a focused new harness only where existing tools cannot exercise the relevant memory/JIT boundary. Validate both ARM32 and ARM64, full/fallback mapping paths, cold/warm execution, reset and save/load.

**Gate C:** Every fast path has a conservative fallback and a failing-before/passing-after targeted test. Ship only measured improvements with no new guest-state/audio/IRQ mismatch.

### Phase D — Reduce event overhead without relaxing device timing (P1)

**Files:** `libretro.c`, `mednafen/psx/{cpu.c,dma.c,gpu.c,gpu_timing.h,cdc.c,mdec.c,timer.c}`.

The capture makes this the first place to look rather than the third: 9,966 events and ~8,100 CPU-loop exits per frame, of which ~8,800 events are the GPU and DMA housekeeping grids and nothing else.

1. **Done: idle-DMA deadline sharing.** An idle DMA controller re-arms at the GPU's own next deadline instead of its own grid point, so the two events coincide and `PSX_EventHandler()` retires both in one CPU exit. Safe because an idle update has no channel work left - the fast path only clamps accumulated credit to zero, which is idempotent - and both remaining callees are invariant to how the interval is subdivided (`MDEC_Run` saturates its clock counter at `EventCycles`; `GPU_AdvanceDrawing` saturates drawing credit at `2 x EventCycles`, and `min` composes). An active channel keeps the original grid, because `RunChannel` discards leftover credit when a device blocks and is therefore *not* subdivision-invariant. A CPU overclock keeps the original grid too, because the two clock domains round per call.
   - The residual semantic delta is that `GPU_Update()`'s call sites move onto the GPU's grid alone where they used to land on the union of both grids. The gap stays bounded by `EventCycles` either way - the granularity this scheduler already declares - but dot clocks reach `TIMER` on a slightly coarser schedule, so a game leaning on timer 0/1 IRQ latency is the shape to watch in game testing.
2. **Next: measure before touching the GPU grid.** The GPU's own 128-cycle tick cannot simply be lengthened: `GPU_AdvanceDrawing`'s credit is consumed by `ProcessFIFO` at arbitrary times between updates, so a longer tick would make the guest observe a stale idle/FIFO-ready bit. Lengthening it requires making every observer (`GPU_Read`, `GPU_Write`, `GPU_DMACanWrite`, `TIMER_Read`) bring the GPU current on demand, and giving `TIMER_Update` a dot-clock-aware deadline. That is a real design, not an edit; do not attempt it before item 4 of the execution order prices what an exit actually costs.
3. Keep optimizing equivalent work first: zero-elapsed updates (`GPU_Update` now returns before any display-mode arithmetic when nothing elapsed, which now covers about half its calls), repeated clock/mode computations, event-list selection, and inactive-channel checks whose state can be preserved exactly.
4. Consider shared/coalesced GPU/DMA bookkeeping only where it preserves original observable deadlines and tie ordering. Do not silently align previously distinct device events or delay status updates.
5. `tools/multicore/event_scheduler.c` is the harness for all of this: it runs a full NTSC frame through the same event list `libretro.c` uses, with the production `DMA_Update()` and the production `GPU_NextEventDelay()` arithmetic. Extend it rather than reasoning about schedules on paper. Its baseline exit count (8,859) lands inside the device's measured 8,033-9,304 quanta/frame, which is what makes it a usable model.
6. An **experimental** sweep of `EventCycles` 128/256/512 remains diagnosis, not a preapproved default. `EventCycles` also bounds the drawing-credit limit, so raising it is not purely host-side.
7. Keep `spu_samples=1` in the production fidelity baseline. Worker batching is the safer amortization lever.

**Gate D:** Prefer equivalent scheduling/idle-work savings, each with a differential harness that compares serialized state, RAM and the ordered device/IRQ trace, not just throughput. Any candidate that changes guest-observable timing is not promoted as a universal low-end fix merely because Tekken boots.

### Phase E — Bounded compiler concurrency and work queues (P1/P2)

**Files:** `deps/lightrec/recompiler.c`, `mednafen/worker_affinity.{c,h}`, `tools/multicore/{recompiler.c,affinity.c,affinity_linux.c,workers.c}`.

1. Compare one, two and current automatic compile-worker counts, plus synchronous compilation as a diagnostic control, on cold launches and warm heavy scenes.
2. Base worker limits on CPUs actually available to the app and measured contention, not just total logical CPUs or TV model. Preserve intentional frontend cluster masks and Android cpusets.
3. Reserve CPU headroom for emulation/audio/frontend by bounding background compilation only when the matrix demonstrates a win. Do not impose one universal cap without warmup/throughput measurements on dual-core, quad-core and asymmetric systems.
4. Profile the linked-list request scan, allocator lock, cancellation waits and code-cache flushes. Optimize only if queue depth makes them material; keep existing highest-request priority and lifecycle correctness.
5. Preserve the recent predicate-before-sleep fix and allocation-failure cleanup. Ensure pause, flush, reset, shutdown and second launch cannot strand work or wait forever.
6. Do not persist raw generated native code across sessions as a shortcut; ASLR, mappings, ABI, build and invalidation make this a separate design problem.

**Gate E:** Reduced p95/p99 launch and gameplay cost with no excessive time interpreted, deadlocks, memory growth or thermal regressions. If workers sleep through warm gameplay, do not claim their count explains warm CPU cost.

### Phase F — GPU command batching and native renderer overhead (P2, promoted)

**Files:** `mednafen/psx/{gpu.c,gpu_stage_policy.h}`, `rhi/{rhi_intf.c,rhi_defer.c,rhi_lib_vulkan.c,rhi_lib_gl.c,rhi_tt.c}`, existing GPU test/host tools.

1. **Done: adaptive staging bypass.** The staging buffer assumes a batch will eventually be long enough to hand over; in the plateau it never is. After a run of short inline resolves the policy stops staging and executes GP0 where it would have ended up anyway, which makes "nothing is outstanding" a standing invariant - so status reads and readiness polls answer from the decoder without a barrier, and the published-snapshot path stops being blocked by a non-empty staging buffer. One uninterrupted run reaching the publication threshold re-arms the worker, so a streaming scene still gets it. The decision lives in `mednafen/psx/gpu_stage_policy.h`, separate from the ordering and barrier rules it feeds, and is replayed against recorded batch shapes by `tools/multicore/gpu_stage.c`.
2. **Done: the duplicated barrier predicate.** `GPU_Read()` evaluated `GPU_Sync_Would_Collapse()` - three acquire loads - and then `GPU_Worker_Sync()` redid the same tests, on every one of 1,485-5,384 status reads per frame. The predicate now answers from the bypass invariant.
3. **Next: scanout image reuse.** One `vkCreateImage` per presented frame (`img_create_seen` ratio 0.95-1.01 over 35 windows). Find the allocation in the Vulkan RHI scanout path, establish whether it can be pooled across frames within the existing lifecycle rules, and measure it with the Vulkan host rather than assuming it explains the finalize spikes.
4. Record batch-size distribution and why batches resolve: clock tick, status read, DMA readiness, readback, queue threshold or end-of-frame. The bypass counters (`bypass_words`, `bypass_polls`, `bypass_entries`, `bypass_exits`, reported per interval) are the first instalment of this; add the resolve-reason breakdown if the next capture still cannot explain a scene.
5. Establish equivalent diagnostic baselines with GPU threading enabled and disabled. With the bypass in place the enabled case should now converge on the disabled case in poll-heavy scenes by construction; confirm that rather than assume it.
6. Preserve authoritative FIFO readiness, retirement checks, IRQ delivery, VRAM readbacks, texture/CLUT dependencies, quad/polyline continuation and GL context affinity. Note in particular that the worker can park on a published read-back with an *empty* ring and report itself idle, which is exactly the state the bypass engages in - the bypass path therefore still services deferred read-backs.
7. Pipeline caching already exists in the Vulkan renderer, including on-disk validation. Verify hits/misses and write placement before proposing a new cache.
8. Audit disabled HD/analog/HDR paths for residual tracking, allocation or synchronization at native SDR.
9. Benchmark rendering every frame as well as honoring YAGE skip requests. The 28-29 ms and 39.05 ms averages already include skipped scanouts, so full-video cost can be higher.

**Gate F:** Existing PIO/DMA generated-ROM matrix passes with identical relevant results, queue stress actually exercises worker execution, Vulkan validation is clean, and native display cadence improves on real drivers. The staging-policy unit tests cover when the bypass engages; ordering and barrier correctness for that path still needs the generated matrix against a built core, which has not been run in this session.

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

One existing consistency issue is the ARM Android no-variable fallback for `cpu_dynarec` in `libretro.c`, which still disables JIT despite the repaired A32 path. It can be corrected after the ARM safety matrix, but the captured YAGE session explicitly supplies `execute`, so it is **not the cause of the measured slowdown**.

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

- `mednafen/psx/dma.c`, `gpu.c`, `gpu_timing.h`, `gpu_stage_policy.h`, `cdc.c`, `mdec.c`, `timer.c`: exact scheduling/idle-work/batching candidates. `psx_events.h` carries `PSX_PeekEventNT()`, which lets a device land on an existing deadline instead of adding one.
- `rhi/rhi_intf.c`, `rhi/rhi_defer.c`, `rhi/rhi_lib_vulkan.c`, `rhi/rhi_lib_gl.c`, `rhi/rhi_tt.c`: measured command/scanout/resource costs, without weakening synchronization.
- Existing rasterizer sources: conditional software-fallback work only if profiles justify it.

### Audio, streaming, concurrency and builds

- `mednafen/psx/spu.c`, `mednafen/cdrom/cdromif.c`: bounded batching/read-ahead and sample/IRQ correctness.
- `mednafen/worker_affinity.c`, `.h`: permitted-mask discovery only if necessary for a tested worker policy.
- `Makefile`, `Makefile.common`, `jni/Android.mk`, `jni/Application.mk`: portable target detection and reproducible release variants.

### Tests and benchmarking

- Extend `tools/multicore/{Makefile,spu.c,workers.c,recompiler.c,gpu_timing.c,dma.c,event_scheduler.c,gpu_stage.c,affinity.c,affinity_linux.c,make_gpu_test.py,check_gpu.py}`.
- Extend `tools/vkhost/{vkhost.c,Makefile}` and `tools/glhost/{glhost.c,Makefile}` rather than invent another frontend.
- Proposed new `tools/multicore/summarize_tv_log.py`: bounded offline parser for profile/health records, PID and scene/window separation, units, missing-field flags and JSON/CSV output. Test mixed sessions, malformed/non-UTF-8 input, missing profiles and rounded window durations; do not treat missing counters as zero or sum resettable underruns. This is a proposed tool, not present yet.
- `tools/multicore/event_scheduler.c` **exists now**: a full-NTSC-frame differential of the production `DMA_Update()` and `GPU_NextEventDelay()` through the same event list `libretro.c` uses, reporting CPU-loop exits and asserting the emulated outcome is unchanged. Extend it for any further scheduler work.
- `tools/multicore/gpu_stage.c` **exists now**: replays recorded guest batching shapes through `mednafen/psx/gpu_stage_policy.h`. Extend it for any further staging-policy work; ordering and barrier correctness still belong to the generated GPU matrix.
- Add a new JIT harness only if generated-ROM/host coverage cannot test the required boundary. The host attribution work in Phase C item 1 is the exception: it needs somewhere to run the recorded mix, and `tools/vkhost` is the place to put it.
- No changes under YAGE. Do not create extra prose change-summary documents; this plan file contains the implementation plan.

This is a conditional file map, not an instruction to modify every listed file. Each phase should produce the smallest patch supported by its measurements.

## 8. Verification

### Existing host commands to reuse
Run these existing checks from the core repository when resuming implementation. They are proposed commands here, not results of this documentation review:

```sh
make -C tools/multicore check          # 9 suites, including event-scheduler and gpu-stage
python3 tools/multicore/check_android_build.py
make -C tools/vkhost check

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
- Phase 1 added `OBJECT_DIR` isolation. Use a unique object directory per ABI/compiler/flag/profile variant and verify its coverage with the build-policy tests. Do not reuse stale objects across variants; external generated dependencies may still need isolated build trees.

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

### Environment and validation scope

- This review is on **macOS / Darwin arm64**, not the Windows session described in the historical handoff. Python and shell reads were used to inspect both logs, source, test commands and git state. The old WSL/service restriction is not a blocker on this host.
- The initial review changed documentation only. Implementation now includes the portable source/test changes and fresh checks listed below. No device profiling, deployment or FPS improvement is claimed.
- On implementation resume, discover the existing compiler/NDK/adb/device availability locally and try the existing native checks. Request an external runner only if a required check actually cannot execute here. macOS host tests cannot substitute for Android ARM32, Linux affinity or physical Vulkan/audio validation.
- The existing capture partially validates Phase 1 metadata but not detailed profiling or end-to-end audio/display. Device evidence remains unavailable under the latest no-profiling instruction. Use source-level correctness gates for the current patch; retain the full-speed goal as unverified.

## 9. Risks, frontend constraints and escalation

1. **Hardware ceiling:** Some low-end TVs may not have enough CPU for this fork's fidelity target, particularly software-only devices. Measurement defines support; optimization cannot guarantee an arbitrary hardware floor.
2. **Frontend recovery threshold:** YAGE leaves skip mode only below 85% of the native period (`native/yage_frame_loop.c`, skip-mode EWMA logic). A core improvement from 29 ms to 16 ms may restore supply yet fail to recover full presentation immediately.
3. **Presentation is not identical to emulation rate:** current `tv_logs.txt:548` shows 60.0 runs/s but only 38.3 presentations/s with snapshot misses. This may include transition/catch-up effects; it requires correlation, not an assumed core bug. If persistent at full core speed, report a frontend-scoped blocker and seek separate approval.
4. **Audio sink remains frontend-owned:** Correct PCM and full core speed are necessary, not sufficient, for clean HDMI/TV audio. Do not work around a frontend sink issue by falsifying core timing or sample counts.
5. **Non-Vulkan path:** Existing YAGE PS1 GL restrictions prevent assuming a universal renderer switch. Software coverage is part of the plan, with a potentially higher CPU requirement.
6. **Timing knobs:** EventCycles and SPU sample update size affect guest behavior. Small game suites cannot establish universal compatibility for a relaxed scheduler.
7. **Threading risks:** More parallelism can increase cache traffic, wakeups, priority contention and thermal load. Queue/barrier changes need deterministic tests and real weak-memory ARM testing.
8. **JIT memory safety:** Fast memory accesses must retain mapping guards and invalidation. Code-cache size increases can worsen low-memory pressure; do not trade paging stalls for an apparently faster warm microbenchmark.
9. **Existing diagnostics:** Sampling clocks/logging and input floods can distort profiles. Always verify with diagnostic-free release builds.
10. **Artifact integration:** Keep YAGE's manifest unchanged under this scope. Deliver the core artifacts and provenance; normal production packaging must explicitly select the approved revision/artifact in a separately authorized integration step, otherwise the old manifest pin can undo the work.

If the measured candidates cannot bring the minimum supported cohort under budget, present the residual profile and a clear pass/fail support matrix. A software-renderer architectural redesign, replacement JIT/core, compatibility-reducing profile or frontend changes are **new scope decisions**, not automatic next edits.

## 10. Completion checklist and delivery order

1. **Baseline/profiling milestone:** reproduce and label the current 28–29 ms plateau, later ~20 ms section, short stall and late 39.05 ms window; capture actual JIT/event/host-wait costs and build identity.
2. **First optimization milestone:** smallest verified build/JIT/event critical-path improvements, with targeted tests and before/after device data.
3. **Concurrency/rendering milestone:** only the worker/batching/resource changes whose measured gain survives correctness and thermal tests.
4. **Audio/streaming milestone:** prove sample/IRQ fidelity and normal-pitch physical playback at restored emulation speed.
5. **Broad-device milestone:** test the capability cohorts and game suite; publish measured minimum support and remaining limitations.
6. **Artifact milestone:** per-ABI release `.so`, matching symbols/build IDs/hashes, exact flags/options, regression results and machine-readable benchmark data. No commit/push/deployment without the relevant user authorization.

Success means **correct-speed emulation, appropriate native visual cadence, and clean normal-pitch audio on the tested support matrix**, not a changed FPS label, a silent log, or a build that merely compiles.

---

## Current implementation record — portable first patch (2026-09-08)

- **GPU arithmetic:** `GPU_DisplayWidth()` replaces the runtime-divisor width calculation in scanout and display-overlap checks. `GPU_DisplayClocks()` uses constant division for horizontal bounds/offsets and light-gun pixel-clock metadata. It covers the fifth bit-6 `/7` scanout mode while preserving the existing four-mode timer calculation and unsigned conversions. No event deadline, native clock, renderer ownership, worker threshold or SPU behavior changes.
- **Build-test portability:** the Android build-policy harness now uses an executable POSIX shell adapter on hosts where GNU Make 3.81 ignores `.SHELLFLAGS`. It includes the current `LIBS`/`SYS_LIBS` link inputs and tests Android system libraries surviving explicit link-flag overrides. The Windows interpreter path remains intact; it was not rerun on Windows.
- **Native checks:** all six SPU/worker/affinity/GPU-timing/recompiler/performance suites passed on macOS arm64. The expanded GPU test compares all 256 display-mode byte values over horizontal ranges and boundary values, plus one million random inputs across all five scanout divisors. Existing event-deadline/timer equivalence checks also pass.
- **Sanitizers:** the GPU timing/equivalence suite passed with AddressSanitizer and UBSan.
- **Build policy:** all 10 test methods pass with GNU Make 3.81, including simulated Android ABI/build-host combinations, JNI/root builds, object isolation and system-library overrides. These are configuration tests, not completed Android links.
- **Vulkan host:** the 72 compiled helper/config/audio/mock-Vulkan checks passed; no real GPU was used.
- **Android compile checks:** the changed production `gpu.c` passes NDK 30 ARM32/API21 and ARM64/API21 syntax checks with hardware, GLES3, Vulkan and Lightrec build flags. An ARM32 generated-code probe shows the reference table division calls `__aeabi_uidiv`, while the new helper emits no integer-divide instruction/helper. This confirms the intended code-generation change, not an FPS gain.
- **Remaining limits:** no device profiling, full-core link, physical audio/video verification or thermal test; no `.so` generated. The next work must remain portable and semantics-preserving. Actual 60-FPS/clear-audio acceptance remains open.

## Current implementation record — idle DMA optimization (2026-09-08)

- `DMA_Update()` now bypasses transfer setup for a channel only when both its busy bit and remaining word count are zero. Idle clock debt/credit is updated exactly as in `RunChannel()`. Active transfers and partially consumed blocks retain the original transfer path, including forced stops with busy already cleared.
- GPU/MDEC updates still happen first, all seven channels retain their original order, and DMA deadlines, halt/cycle-steal calculations, IRQs and invalidation behavior remain unchanged. The fast path reads current state each update; there is no active-mask cache to become stale after register writes or state loads.
- This removes transfer-loop setup and bookkeeping from each idle channel visit. Generated ARM32/ARM64 code has a short idle branch before transfer setup; the compiler may inline `RunChannel`, so this is not a claim of seven saved function calls. No model checks, affinity changes, device profiling or guest-timing changes were introduced.
- New `tools/multicore/dma.c` executes the production source against the old unconditional channel walk. **1,659 differential updates pass**, comparing serialized state, RAM, deadlines and ordered device/IRQ/invalidation calls. Coverage includes all 128 active-channel combinations, ready/blocked devices, read/write directions, retained negative debt, zero elapsed time, partial forced stops, register activation, timestamp resets and linked-list end/bus-error cases. Device endpoints are deterministic stubs, not full GPU/SPU/CD emulators.
- All **seven native multicore suites pass**. The DMA suite also passes AddressSanitizer/UBSan, and production DMA code passes NDK 30 ARM32/API21 and ARM64/API21 syntax/assembly compilation with hardware/GLES3/Vulkan/Lightrec flags. No release `.so` was generated.
- TV throughput and physical audio remain unmeasured. Continue portable improvements with differential tests; do not resume device profiling.

## Current implementation record — event cadence, JIT exits and GPU staging (2026-09-08)

Three changes, each with a differential or replay harness, all device-neutral and semantics-preserving in the cases they apply to. **No FPS or millisecond gain is claimed:** the capture has `event timing off` and `per-word timing off`, so nothing in it prices a CPU-loop exit or a GP0 word, and no device was profiled. What is reported below is structural - exits, events, polls, words - measured exactly by host harnesses.

### 1. Event handling: idle-DMA deadline sharing

- `DMA_Update()` now re-arms an idle controller at the GPU's existing next deadline instead of its own `EventCycles` grid point, so the GPU and DMA events coincide and `PSX_EventHandler()` retires both in one CPU exit. The deadline it adopts is the armed GPU entry while that is still in the future, and otherwise the deadline `GPU_Update()` just returned - which is what makes the two events converge after a single update instead of oscillating when DMA sorts ahead of the GPU at equal timestamps.
- Applies only when no channel is busy and none holds a partially consumed block, re-tested after `RunChannel` so a transfer that finished during the update counts as idle. An active channel keeps the original grid, because `RunChannel` discards leftover credit when a device blocks and is therefore not invariant to how the interval is subdivided. A CPU overclock keeps the original grid too, because `overclock_cpu_to_device` rounds per call and a differently subdivided interval would not accumulate to the same device-clock total.
- Adoption is bounded by `EventCycles`, so an unscheduled or stale GPU entry (`PSX_EVENT_MAXTS`) can never defer the controller past its own grid; it falls back to the grid instead.
- `PSX_PeekEventNT()` was added to `psx_events.h`/`libretro.c` to read an armed deadline without dispatching or re-arming it.
- `GPU_Update()` now returns before any display-mode arithmetic when nothing has elapsed. That path was already reached often; with the deadlines shared it now covers about half of all calls (4,465 of 8,928 in the harness).
- **New `tools/multicore/event_scheduler.c`** runs a full NTSC frame (565,053 guest cycles) through the same event list `libretro.c` uses, driving the production `DMA_Update()` and the production `GPU_NextEventDelay()` arithmetic over a scanline model built from `gpu.c`'s constants. Result: **CPU-loop exits 8,859 -> 5,196, a 41.3% reduction**, with events 9,612 -> 9,661 and DMA updates 4,415 -> 4,463 - the controller is serviced slightly *more* often, just never on a separate exit. Serialized DMA state, RAM and the ordered device/IRQ trace are identical, and at every original grid point the controller had been serviced within one quantum. The harness's baseline exit count sits inside the device's measured 8,033-9,304 quanta/frame, which is what makes it a usable model of the real schedule.
- The same harness asserts that an active channel, a CPU overclock (128/256/384/512) and quanta of 128/256/512/1024 all reproduce the original schedule exactly.
- **`tools/multicore/dma.c` extended** by 42 sharing cases: fourteen peeked deadlines including `INT32_MIN`, `INT32_MAX`, `PSX_EVENT_MAXTS` and both quantum boundaries, at two timestamp bases, asserting serialized state, RAM and trace are independent of the peek and the deadline never exceeds one quantum; plus all seven channels in both busy and forced-stop-with-partial-block states, asserting they stay on the grid. Its original 1,659 differential updates are unchanged and still pass.
- **Residual semantic delta, stated plainly:** `GPU_Update()`'s call sites move from the union of both grids onto the GPU's grid alone. The gap between GPU updates stays bounded by `EventCycles`, which is the granularity this scheduler already declares, but dot clocks reach `TIMER` on a slightly coarser schedule. A game leaning on timer 0 (dot clock) or timer 1 (hblank) IRQ latency is the shape to test first when game testing resumes.

### 2. GPU staging: adaptive bypass

- After a run of inline resolves too short to have paid for a worker handover, `gpu.c` stops staging GP0 and executes it where `GPU_Stage_Resolve()` would have executed it anyway - same words, same order, same thread. One uninterrupted run reaching the publication threshold re-arms the worker, so a guest that starts streaming still gets it.
- While bypassing, "nothing is outstanding" is a standing invariant: the staging buffer is empty, the ring is empty and the worker is parked. That is exactly what every barrier in the file is testing for, so `GPU_DMACanWrite()` answers from the decoder without a barrier or a published snapshot, and `GPU_Worker_Sync()` returns after servicing any deferred read-back and applying IRQ state. Anything reaching the ring ends the bypass.
- The read-back case is handled explicitly rather than assumed away: the worker publishes a read-back rectangle *after* retiring its batch, so it can park with an empty ring and report itself idle - which is the state the bypass engages in. The bypass path therefore still calls `GPU_FBRead_Service()`.
- `GPU_Read()` no longer evaluates `GPU_Sync_Would_Collapse()`'s three acquire loads and then repeats the same tests inside `GPU_Worker_Sync()`. On the plateau that predicate ran 1,485-1,955 times per frame, and 5,384 times in the first window, to record a counter that was 0.6% non-zero.
- The decision lives in **new `mednafen/psx/gpu_stage_policy.h`**, separate from the ordering and barrier rules it feeds, so it can be replayed without a renderer or a GPU. Hysteresis both ways: 64 consecutive short batches to engage, one full-threshold uninterrupted run to leave.
- Four counters are reported per interval (`bypass_words`, `bypass_polls`, `bypass_entries`, `bypass_exits`), as subsets of the existing inline and DMA-ready figures rather than additional work, so the next capture says whether the policy matched the scene.
- Reset, state load and worker teardown all drop the learned policy and refresh the published readiness byte, because a load replaces the FIFO and `InCmd` state the bypass answers from.
- **New `tools/multicore/gpu_stage.c`** replays recorded batch shapes: the plateau shape (14,357 words / 1,032 polls per frame) engages after exactly 64 short batches and then answers 3,032 of 3,096 polls without a barrier, with one entry and no re-arms; the streaming shape never engages at all over 4,000 batches; a single publishable batch inside a poll-heavy stretch resets the streak; a threshold-length uninterrupted run re-arms the worker; drains end a run, so `STAGE_MAX` words split across polls do *not*; reset/end are idempotent; and the `BEETLE_GPU_QUEUE_STRESS_TEST` threshold of 4 is covered alongside the shipping 1,024. 5,904 policy transitions checked.
- **Not covered here:** ordering and barrier correctness for the bypass path against real command streams. That needs the generated PIO/DMA matrix (`tools/multicore/check_gpu.py`) against a built core, plus the queue-stress build, and neither was run in this session - no `.so` was produced.

### 3. Verification actually performed

| Check | Result |
|---|---|
| `make -C tools/multicore check` | **9/9 suites pass** on macOS arm64, including the two new ones |
| Same suite under `-fsanitize=address,undefined -fno-sanitize-recover=undefined` | **Pass** |
| Same suite under `-fsanitize=thread` | **Pass** |
| `python3 tools/multicore/check_android_build.py` | **10/10 pass** |
| NDK 30 ARM32/API21 and ARM64/API21 compile of `libretro.c`, `mednafen/psx/gpu.c`, `mednafen/psx/dma.c` through the real root Makefile with release flags | **Clean, both ABIs** |
| Device FPS, physical audio, thermal endurance, full-core link, `.so` artifact | **Not done.** No device profiling, no deployment |

### 4. What the next session should do

1. **Price a CPU-loop exit and a GP0 word on the host** (execution-order item 4). Until that exists, the 41.3% exit reduction is a structural result with no millisecond attached, and the plan cannot say how much of the 28.4 ms it removes. This is the single highest-value missing number.
2. **Scanout image reuse** (execution-order item 5): one `vkCreateImage` per presented frame, confirmed over 35 consecutive windows.
3. **Add a `LIGHTREC_EXIT_SEGFAULT` counter** to the opt-in CPU diagnostics, so the unmapped-access exits at PC 0x00003774 stop being unbounded-and-unsilenced.
4. **Run the generated GPU matrix and the queue-stress build against a built core** to close the ordering gap the staging bypass leaves open, whenever building is authorized again.
5. When device measurement is authorized again, the first capture should carry `beetle_psx_hw_gpu_diagnostics=timing` on the same labelled scene, paired against a `disabled` control - that is the only thing that converts any of this into a millisecond figure.

## Historical FIFO implementation and verification

The earlier review covered a different PID 10375 session. Its performance figures and GPU-worker default are superseded by the current capture. It reported an atomic byte publishing FIFO readiness and the compatibility threshold, usable only when worker retirement covers submissions, staging is empty and no pending IRQ/setting mismatch requires a barrier. Batched queue submission uses up to two contiguous copies with one publication/wake check. This is not a complete emulation-thread shadow decoder; command-cycle progress and continuation semantics still require authoritative synchronization.

That review reported a 22-case / 1,734-word PIO/DMA GPU workload, threaded-versus-inline RAM/scratchpad/audio/video comparisons over 320 frames, and a tiny-queue stress matrix with 1,693 queue-full stalls and zero validation mismatches. It also reported SPU/worker/allocation tests and per-ABI Android alignment/export checks. These historical passes are **not validation of the current dirty device binary or current final tree**. A GL line-pixel probe failed equally with threading on/off; retained polyline coverage checks decoder termination, not general native line rasterization. No historical result established TV FPS gains, physical Vulkan correctness or live achievement integration.

## Historical Phase 1 handoff — profiling and build foundations (2026-09-08)

**Historical status: phase concluded with implementation pending validation.** The sections above supersede the old baseline/plan. The records below retain what the earlier session reported; they were not rerun during this review.

**Prior phase instruction:** conclude that phase, preserve its progress, and do not generate further `.so` files. Both ongoing core-build processes were stopped. One intermediate ARM64 `.so` had already been produced; it is not a validated final artifact. Generated intermediates/test executables remain under `build/` and must not be committed as release binaries. No deployment, commit, push, YAGE modification, toolchain installation, or WSL configuration change was performed.

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

### Verification reported by the earlier session

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

The prior session reported IDE access restrictions for GL-host sources and no usable native POSIX runner. Those are historical environment limits; verify access/tool availability in the current workspace before treating them as pending user inputs.

## Current phase status and remaining work

| Phase | Current state | Next work |
|---|---|---|
| **A — Baseline/profiling** | The periodic record turned out to be present and complete for counts (31 windows); only the *timing* inside it is off. Native suites and mock checks pass; device profiling suspended | Price a CPU-loop exit and a GP0 word on the host, since that is what every remaining decision needs. Add a `LIGHTREC_EXIT_SEGFAULT` counter. When device capture resumes, require `gpu_diagnostics=timing` paired against a `disabled` control on one labelled scene |
| **B — Android builds** | Build-system fixes and policy tests implemented; the three changed production files compile clean on ARM32 and ARM64 through the real root Makefile | Re-run the full policy matrix; when builds are requested again, verify all ABI variants, final flags, exports, dependencies, 16 KB alignment, and normal-versus-profile defaults. O2/O3/LTO/ISA comparisons remain unperformed |
| **C — JIT/memory** | The exit-count problem is now addressed from the scheduler side (Phase D item 1). No JIT fast paths added, and none should be until attribution exists | Build the host attribution; only then choose guarded memory/helper/register/GTE work, with failing-before/passing-after probes on ARM32 and ARM64 and both mapping paths |
| **D — Event scheduler** | Idle-channel fast path, idle-DMA deadline sharing and the zero-elapsed `GPU_Update` early-out implemented; differential DMA harness and a new full-frame scheduler harness cover them. 41.3% fewer CPU-loop exits with an identical emulated outcome | Watch dot-clock timer IRQ latency in game testing. Lengthening the GPU's own grid is a design (on-demand device updates plus a dot-clock-aware timer deadline), not an edit - do not start it before the exit cost is priced |
| **E — Compiler concurrency** | Profiling and tests added; worker count unchanged. Nothing in the capture implicates compile churn in warm gameplay | Measure queue contention and cold/warm behavior before comparing worker counts |
| **F — GPU/rendering** | Portable five-mode scanout division implemented; adaptive staging bypass and the folded barrier predicate implemented, with a policy replay harness. Worker busy time was 0.00 ms/frame in every captured window | Scanout image reuse (one `vkCreateImage` per presented frame). Run the generated PIO/DMA matrix and the queue-stress build against a built core to close the ordering gap the bypass leaves open. Pipeline/cache/resource work still pending |
| **G — Audio/CD** | Output accounting/WAV tooling added; the capture shows underproduction (~57% of 44.1 kHz) and a 500-permille elastic rate change; audio gate failed | Nothing here is fixable in the audio path: supply follows core speed. Execute SPU/IRQ/ENDX regressions and validate WAV/sample totals, but treat the audio gate as blocked on emulation speed |
| **H — Broad low-end devices** | Capability matrix specified; no new devices tested | Test ARM32/ARM64 TVs across vendors, other Mali/Adreno drivers, phones, low-memory and non-Vulkan/software paths |
| **Artifact/integration milestone** | **Deferred by user** | Do not generate, deploy or package cores now. YAGE's stale manifest pin (`scripts/libretro_cores.json:145-151`) remains unchanged and would silently undo this work if packaging ran today |

## Historical device capture runbook — suspended; do not execute

1. **Identify the run.** Keep the supplied logs unchanged. Save a manifest containing core/frontend revisions and dirty diffs, core SHA-256/build ID, APK build mode, ABI, options, content/BIOS hashes, scene/save/input sequence, device/OS/GPU driver, audio route, refresh mode and thermal state. Native timestamps belong to a PID/session; label scene start/end times in a sidecar text file. Record app restarts and pause/menu intervals explicitly.
2. **Enable the existing profiler.** Before gameplay, set `beetle_psx_hw_gpu_diagnostics=timing` (UI label “GPU Diagnostics → Command Timing”) using the frontend's existing core-option facility. Keep native resolution/clocks, PGXP off, full invalidation, EventCycles 128 and SPU update size 1. Confirm `GET_VARIABLE` actually resolves to `timing`. Do not use FIFO Validation/`all` for a speed comparison.
3. **Fail fast on missing diagnostics.** Startup runtime/build/engine metadata is insufficient. After at least 300 emulated frames, require `core_profile_v1`, `core_profile_cpu_v1`, `core_profile_jit_v1`, per-type `core_profile_event_v1` and Vulkan `core_profile_renderer_v1` output. Capture at least three complete diagnostic windows per labelled scene. At 34 runs/s, each 300-frame window takes about 8.8 seconds; at 20 runs/s, about 15 seconds. If output is absent, check effective option, log level and deployed binary before collecting minutes of unusable profiling.
4. **Capture scenes separately.** Reproduce title/attract, a fixed active match/stage, loading/transitions and the late expensive region. The current logs do not identify the characters/stage or prove which region is active gameplay. Start with a deterministic attract sequence if exact input replay is unavailable, then validate active matches separately. Keep cold and warmed measurements distinct.
5. **Profile the measured thread.** On-device, pair the core counters with a short supported CPU/scheduler trace of emulation, SPU, compiler and renderer activity in the heavy region. Record effective thread masks, priorities, runnable/sleep time, CPU frequency and thermal state; sampling availability/permissions vary. Record inaccessible metrics as unknown. Do not change affinity or priority to manufacture a result. If sampling cannot resolve generated code, use its executable memory ranges and JIT counters, then add targeted counters only for remaining uncertainties.
6. **Run controls.** Repeat identical scenes with `gpu_diagnostics=disabled` to measure actual release throughput and timing overhead. Compare GPU threading off/on if exposed by the existing frontend. Keep SPU on for the initial control. Require an explicit effective-option change; a `BEETLE_PSX_PROFILE=1` build changes defaults only, so explicit `disabled` still wins. If no existing facility can select timing, first document that concrete integration limitation; a new option route is separate frontend work.
7. **Measure the output.** Use the existing host's PCM/WAV mode for deterministic sample/pitch/continuity checks, separately from benchmark mode. Also listen to/record the physical TV or HDMI output during the same labelled scene. Record low pitch, buzzing repetition, isolated clicks or gaps with timestamps. Current logs expose emergency rate changes only; if normal sink rate cannot be observed through existing facilities, physical duration/pitch evidence is required and direct sink-rate telemetry remains an explicitly identified frontend gap.
8. **Return a decision package.** Raw full and filtered logs, manifest, scene markers, symbols, profile table with units/overlap notes, diagnostics-off paired results, audio observations and remaining gap to the native deadline. The next patch proposal must identify one measured cost, expected upper-bound saving, precise files, correctness tests and rollback criterion. Update this plan's phase table after the result.

Example capture from macOS/Linux, after opening the app and before game launch. Run in a new capture directory; select the intended device if adb lists more than one:

```sh
adb devices -l
adb shell pidof com.yourmateapps.retropal
adb logcat -v threadtime -T 1 > tv_phase1_full.txt
```

Stop with Ctrl+C after the labelled scenes. Preserve the full capture and derive a filtered file afterward using the PID from **that run**, never hardcoding 22173 for a future session. For example, replacing `CURRENT_PID` with that numeric PID:

```sh
rg ' CURRENT_PID +[0-9]+ [VDIWEF] flutter *:' tv_phase1_full.txt > tv_phase1_flutter.txt
rg 'core_profile(_[a-z]+)?_v1:|gpu_diagnostics|retro_run over|Frame loop health|Elastic:' tv_phase1_flutter.txt
```

The last command checks presence, not correctness or performance. System/driver events remain in the full capture. Do not clear log buffers, enable validation layers for release comparisons, or assume host benchmark results include YAGE/Android presentation and audio.

**Completion condition:** every required steady scene in the declared device/game matrix passes native guest speed, full visual cadence, normal-pitch clear audio and correctness/thermal gates. A built library, startup metadata, “healthy” health label, or one short 60 Hz window does not complete the target.
