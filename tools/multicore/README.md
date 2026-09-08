Run `make -C tools/multicore check` from the repository root. These tests run
the production SPU and worker sources with real libretro-common threads.
They need a C11 compiler and POSIX threads; no BIOS or game files are required.

The SPU test compares serialized SPU state, generated audio, CD consumption,
and timestamped interrupt traces with threading enabled and disabled. It covers
DMA, register reads/writes, IRQ mode changes, state restore, worker restarts,
sample update sizes, and partial batches. It also checks that CDC and CPU IRQ
callbacks only run on the emulation thread, and that 735 samples use eleven
full worker batches plus a partial batch (which can run inline).

The worker test covers a full FIFO, ordering, draining during destruction,
multiple callers dispatching to a shared pool, and creation failures. Both
tests inject failure at each thread/lock/condition allocation and check cleanup.

For race detection, use a separate build directory:

```
make -C tools/multicore check BUILD_DIR=/tmp/beetle-multicore-tsan \
  CFLAGS='-O1 -g -std=c11 -fwrapv -fsanitize=thread'
```

These tests establish deterministic behavior for synthetic SPU workloads.
Full BIOS/game boot, frontend callbacks, and achievement runtime integration
still require testing the built core in a frontend.

The GPU test generator also covers mask handling, wrapped/overlapping copies,
and texture/CLUT dependencies across frame boundaries (including VRAM edges).
It can generate a minimal test-only BIOS that jumps to the core's EXE loader:

```
python3 tools/multicore/make_gpu_test.py /tmp/gpu-test.exe --bios-dir /tmp/gpu-bios
GLHOST_SYSTEM_DIR=/tmp/gpu-bios GLHOST_SAVE_DIR=/tmp/gpu-save \
  GLHOST_TEST_REPORT=1 \
  GLHOST_VARS='beetle_psx_hw_renderer_software_fb=disabled;beetle_psx_hw_cpu_dynarec=disabled' \
  tools/glhost/glhost /path/to/core /tmp/gpu-test.exe - 40 /tmp/gpu-output
```

Repeat at 2x resolution, with `GLHOST_RECREATE_AT=3`, and with the software
framebuffer enabled. The report must pass all 22 cases (1,734 words).
Use `--unmapped-probes 1024` to exercise Lightrec's completed unmapped loads;
test both `execute` and `run_interpreter` and check that no "until reset"
fallback is logged. Software cores use the `beetle_psx_cpu_dynarec` option key.
These BIOS files are only for this test, not game compatibility testing.

Use `--dma` to send drawing commands through DMA2 linked lists. The streamed
upload crosses packet boundaries; quad and polyline checks exercise continuation
decoding. Readback verifies every transferred word. The polyline check verifies
that its terminator releases the decoder for the next primitive, without relying
on native GL line pixel coverage.

The automated hardware-core matrix runs both PIO and DMA, compares threading
on/off RAM, scratchpad, audio and video hashes for 320 frames, and exercises
diagnostics, save/load on every frame, context recreation, 2x rendering and the
software framebuffer:

```
python3 tools/multicore/check_gpu.py /path/to/hardware-core \
  --output /tmp/gpu-check
```

The harness needs a working local graphics context. FIFO validation must exercise
at least one exact snapshot and report zero mismatches. Normal tests can remain
entirely inline because the production staging threshold is 1,024 words.
For queue coverage, build a separate core with
`EXTRA_FLAGS=-DBEETLE_GPU_QUEUE_STRESS_TEST` and run the matrix with
`--require-worker-queue`. This uses the production queue implementation with a
16-entry ring and four-word staging buffer, forcing handoffs and frequent
wraparound. Never ship this stress build or use its speed to choose defaults.
