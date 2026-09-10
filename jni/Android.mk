LOCAL_PATH := $(call my-dir)

CORE_DIR := $(abspath $(LOCAL_PATH)/..)

DEBUG                    := $(if $(filter debug,$(APP_OPTIM)),1,0)
NEED_CD                  := 1
NEED_BPP                 := 32
NEED_DEINTERLACER        := 1
NEED_THREADING           := 1
GLES                     := 0
GLES3                    := 0
HAVE_OPENGL              := 0
HAVE_VULKAN              := 0
HAVE_CHD                 := 1
IS_X86                   := 0
IS_64BIT                 := 0
FLAGS                    := -DANDROID -DHAVE_MMAP
HAVE_LIGHTREC            := 1
THREADED_RECOMPILER      := 1

ifneq (,$(filter x86 x86_64,$(TARGET_ARCH) $(TARGET_ARCH_ABI)))
  IS_X86 := 1
endif

ifneq (,$(findstring 64,$(TARGET_ARCH)))
  IS_64BIT := 1
endif

ifeq ($(HAVE_HW),1)
  # GLES 3 capability is negotiated with the frontend at runtime; a
  # 32-bit process can use it too (including Android TVs).
  HAVE_OPENGL := 1
  GLES        := 1
  GLES3       := 1
  GL_LIB      := -lGLESv3

  ifneq ($(TARGET_ARCH_ABI),armeabi)
    HAVE_VULKAN := 1
    FLAGS       += -DHAVE_VULKAN
  endif
  FLAGS += -DHAVE_HW
endif

ifeq ($(HAVE_LIGHTREC),1)
  FLAGS += -DHAVE_ASHMEM
endif

include $(CORE_DIR)/Makefile.common

ifeq ($(HAVE_HW),1)
  INCFLAGS += -I$(CORE_DIR)/parallel-psx \
				  -I$(CORE_DIR)/parallel-psx/atlas \
				  -I$(CORE_DIR)/parallel-psx/vulkan \
				  -I$(CORE_DIR)/parallel-psx/renderer \
				  -I$(CORE_DIR)/parallel-psx/khronos/include \
				  -I$(CORE_DIR)/parallel-psx/glsl/prebuilt \
				  -I$(CORE_DIR)/parallel-psx/SPIRV-Cross \
				  -I$(CORE_DIR)/parallel-psx/vulkan/SPIRV-Cross \
				  -I$(CORE_DIR)/parallel-psx/vulkan/SPIRV-Cross/include \
				  -I$(CORE_DIR)/parallel-psx/util \
				  -I$(CORE_DIR)/parallel-psx/volk
endif

COREFLAGS := -funroll-loops $(INCFLAGS) -DMEDNAFEN_VERSION_NUMERIC=926 -D__LIBRETRO__ -D_LOW_ACCURACY_ -DINLINE="inline" $(FLAGS)
COREFLAGS += $(GLFLAGS) -fwrapv -fsigned-char
ifeq ($(IS_X86),1)
  COREFLAGS += -fomit-frame-pointer
endif
ifeq ($(DEBUG),1)
  COREFLAGS += -O0 -g -DDEBUG
else
  COREFLAGS += -O3 -DNDEBUG
endif

GIT_VERSION := " $(shell git rev-parse --short HEAD || echo unknown)"
ifneq ($(GIT_VERSION)," unknown")
  COREFLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
endif

include $(CLEAR_VARS)
LOCAL_MODULE       := retro
LOCAL_SRC_FILES    := $(SOURCES_CXX) $(SOURCES_C)
LOCAL_CFLAGS       := $(COREFLAGS)
LOCAL_CXXFLAGS     := $(COREFLAGS) -std=c++11
LOCAL_LDFLAGS      := -Wl,-version-script=$(CORE_DIR)/link.T -ldl -Wl,--build-id=sha1 -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384
LOCAL_LDLIBS       := -llog -landroid $(GL_LIB)
LOCAL_CPP_FEATURES := exceptions rtti
include $(BUILD_SHARED_LIBRARY)
