ROOT := ../..
BUILD_DIR ?= /tmp/beetle-multicore-tests
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -fwrapv
CPPFLAGS += -I$(ROOT)/libretro-common/include -I$(ROOT)
THREADS := $(ROOT)/libretro-common/rthreads/rthreads.c
AFFINITY := $(ROOT)/mednafen/worker_affinity.c

.PHONY: check
check: $(BUILD_DIR)/spu $(BUILD_DIR)/workers $(BUILD_DIR)/affinity $(BUILD_DIR)/gpu-timing $(BUILD_DIR)/recompiler $(BUILD_DIR)/performance $(BUILD_DIR)/dma $(BUILD_DIR)/event-scheduler $(BUILD_DIR)/gpu-stage
	$(BUILD_DIR)/spu
	$(BUILD_DIR)/workers
	$(BUILD_DIR)/affinity
	$(BUILD_DIR)/gpu-timing
	$(BUILD_DIR)/recompiler
	$(BUILD_DIR)/performance
	$(BUILD_DIR)/dma
	$(BUILD_DIR)/event-scheduler
	$(BUILD_DIR)/gpu-stage

$(BUILD_DIR)/performance: performance.c $(ROOT)/mednafen/performance.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DBEETLE_PERF_STANDALONE performance.c -o $@

$(BUILD_DIR)/recompiler: recompiler.c thread_faults.h $(ROOT)/deps/lightrec/recompiler.c $(THREADS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -D_GNU_SOURCE -D__LIBRETRO__ -DENABLE_THREADED_COMPILER=1 -DLOG_LEVEL=0 -I$(ROOT)/include -I$(ROOT)/deps/lightning/include recompiler.c $(THREADS) -pthread -o $@

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/spu: spu.c thread_faults.h $(ROOT)/mednafen/psx/spu.c $(THREADS) $(AFFINITY) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) spu.c $(THREADS) $(AFFINITY) -pthread -o $@

$(BUILD_DIR)/affinity: affinity.c $(AFFINITY) $(ROOT)/mednafen/worker_affinity.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) affinity.c -o $@

$(BUILD_DIR)/gpu-timing: gpu_timing.c $(ROOT)/mednafen/psx/gpu_timing.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) gpu_timing.c -o $@

ifeq ($(shell uname -s),Linux)
check: check-affinity-linux
.PHONY: check-affinity-linux
check-affinity-linux: $(BUILD_DIR)/affinity-linux
	$(BUILD_DIR)/affinity-linux
$(BUILD_DIR)/affinity-linux: affinity_linux.c $(AFFINITY) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) affinity_linux.c $(AFFINITY) -pthread -o $@
endif

$(BUILD_DIR)/workers: workers.c thread_faults.h $(ROOT)/mednafen/rthreads_worker.c $(ROOT)/mednafen/rthreads_pool.c $(THREADS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) workers.c $(THREADS) -pthread -o $@

$(BUILD_DIR)/dma: dma.c $(ROOT)/mednafen/psx/dma.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DHAVE_LIGHTREC -I$(ROOT)/include -I$(ROOT)/deps/lightrec -I$(ROOT)/deps/lightning/include dma.c -o $@

$(BUILD_DIR)/event-scheduler: event_scheduler.c $(ROOT)/mednafen/psx/dma.c $(ROOT)/mednafen/psx/gpu_timing.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DHAVE_LIGHTREC -I$(ROOT)/include -I$(ROOT)/deps/lightrec -I$(ROOT)/deps/lightning/include event_scheduler.c -o $@

$(BUILD_DIR)/gpu-stage: gpu_stage.c $(ROOT)/mednafen/psx/gpu_stage_policy.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) gpu_stage.c -o $@
