ROOT := ../..
BUILD_DIR ?= /tmp/beetle-multicore-tests
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -fwrapv
CPPFLAGS += -I$(ROOT)/libretro-common/include -I$(ROOT)
THREADS := $(ROOT)/libretro-common/rthreads/rthreads.c
AFFINITY := $(ROOT)/mednafen/worker_affinity.c

.PHONY: check
check: $(BUILD_DIR)/spu $(BUILD_DIR)/workers $(BUILD_DIR)/affinity
	$(BUILD_DIR)/spu
	$(BUILD_DIR)/workers
	$(BUILD_DIR)/affinity

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/spu: spu.c thread_faults.h $(ROOT)/mednafen/psx/spu.c $(THREADS) $(AFFINITY) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) spu.c $(THREADS) $(AFFINITY) -pthread -o $@

$(BUILD_DIR)/affinity: affinity.c $(AFFINITY) $(ROOT)/mednafen/worker_affinity.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) affinity.c -o $@

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
