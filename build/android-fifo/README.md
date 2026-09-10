Android FIFO/batching build — 2026-09-08

Use `libmednafen_psx_hw_libretro_android.so` from the folder matching the
frontend process ABI: armeabi-v7a, arm64-v8a or x86_64.

Built with NDK 28.2.13676358, API 21, Vulkan + GLES3 + Lightrec.
All three libraries have 16 KB LOAD alignment and the same exported symbols
as the preceding android-performance build. SHA256SUMS identifies these files.

This build adds coherent GPU FIFO readiness publication, a fast DMA query
when all preceding work has retired, and bulk worker-queue submission.
Pending commands keep their required synchronization. It does not ship the
speculative shadow decoder or remove all DMA barriers.

GPU threading retains its disabled default. SPU threading stays enabled.
No device-specific affinity or extra presentation readback was added.
The core option `beetle_psx_hw_gpu_diagnostics` accepts:

- `disabled`: normal play, no per-word timing or snapshot comparisons.
- `timing`: measure inline GP0 command cost, including with threading disabled.
- `fifo`: validate eligible FIFO snapshots against the synchronized decoder.
- `all`: both diagnostics.

For a useful TV capture, select Command Timing on the same real-game scene
with GPU threading disabled first. Compare threading on/off with diagnostics
disabled to measure the actual performance effect. Validation itself adds
synchronization and is unsuitable for speed measurements.

Local checks cover 22 GPU cases / 1,734 words through PIO and DMA2, threaded
versus inline RAM/scratchpad/audio/video hashes, save/load, graphics context
recreation, 2x rendering and the software framebuffer. A separate small-queue
stress build exercised worker execution, wraparound and backpressure; that
test-only configuration is not included in these Android libraries.
SPU and generic worker lifecycle/determinism suites also passed.

Libretro memory exports, achievement memory maps and serialized guest state
layout are unchanged. No physical-device Vulkan or live RetroAchievements
test was run. A TV FPS increase has not been measured.

See tools/multicore/TV-performance-review.md for the log analysis and scope.
