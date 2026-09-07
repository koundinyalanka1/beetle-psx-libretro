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
framebuffer enabled. The report must pass all 19 cases (701 words).
Use `--unmapped-probes 1024` to exercise Lightrec's completed unmapped loads;
test both `execute` and `run_interpreter` and check that no "until reset"
fallback is logged. Software cores use the `beetle_psx_cpu_dynarec` option key.
These BIOS files are only for this test, not game compatibility testing.
