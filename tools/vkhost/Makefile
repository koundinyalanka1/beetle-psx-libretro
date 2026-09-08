CC ?= cc
CFLAGS ?= -O1 -g -Wall
CPPFLAGS += -I../../libretro-common/include
LDLIBS += -ldl -lvulkan -pthread
PYTHON ?= python3
.PHONY: all check clean
all: vkhost
vkhost: vkhost.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -pthread $(LDFLAGS) -o $@ vkhost.c $(LDLIBS)
check:
	$(PYTHON) check_benchmark.py --cc "$(CC)"
clean:
	rm -f vkhost
