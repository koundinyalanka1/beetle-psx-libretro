Run `make -C tools/multicore check` from the repository root. These tests run
the production SPU and worker sources with real libretro-common threads.
They need a C11 compiler and POSIX threads; no BIOS or game files are required.

The SPU test compares serialized SPU state, generated audio, CD consumption,
and timestamped interrupt traces with threading enabled and disabled. It covers
DMA, register reads/writes, IRQ mode changes, state restore, worker restarts,
sample update sizes, and partial batches. It also checks that CDC and CPU IRQ
callbacks only run on the emulation thread, and that 735 samples require 23
worker jobs when there are no intervening synchronization points.

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
