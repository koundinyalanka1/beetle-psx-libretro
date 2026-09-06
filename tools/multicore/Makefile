ROOT := ../..
BUILD_DIR ?= /tmp/beetle-multicore-tests
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -fwrapv
CPPFLAGS += -I$(ROOT)/libretro-common/include -I$(ROOT)
THREADS := $(ROOT)/libretro-common/rthreads/rthreads.c

.PHONY: check
check: $(BUILD_DIR)/spu $(BUILD_DIR)/workers
	$(BUILD_DIR)/spu
	$(BUILD_DIR)/workers

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/spu: spu.c thread_faults.h $(ROOT)/mednafen/psx/spu.c $(THREADS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) spu.c $(THREADS) -pthread -o $@

$(BUILD_DIR)/workers: workers.c thread_faults.h $(ROOT)/mednafen/rthreads_worker.c $(ROOT)/mednafen/rthreads_pool.c $(THREADS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) workers.c $(THREADS) -pthread -o $@
