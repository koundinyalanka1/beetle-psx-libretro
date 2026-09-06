CC ?= cc
CFLAGS ?= -O1 -g -Wall
CFLAGS += -I../../libretro-common/include $(CFLAGS_EXTRA)
GLES ?= 0
ifeq ($(GLES),1)
CFLAGS += -DGLHOST_GLES
LDLIBS = -lEGL -lGLESv3 -ldl $(LDFLAGS_EXTRA)
else
LDLIBS = -lEGL -lGL -ldl $(LDFLAGS_EXTRA)
endif

glhost: glhost.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f glhost
