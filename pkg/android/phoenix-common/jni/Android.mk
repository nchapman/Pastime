LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

RARCH_DIR := ../../../..

HAVE_NEON   := 1
HAVE_LOGGER := 0
HAVE_VULKAN := 1
HAVE_CHEEVOS := 1
HAVE_FILE_LOGGER := 1
HAVE_GFX_WIDGETS := 1
HAVE_SAF := 1
HAVE_BUILTINSMBCLIENT := 1
# PASTIME: matches the Makefile.common toggle so an `HAVE_PASTIME=0` ndk-build invocation drops the menu driver entirely.
HAVE_PASTIME ?= 1

INCFLAGS    :=
DEFINES     :=

LIBRETRO_COMM_DIR := $(RARCH_DIR)/libretro-common
DEPS_DIR          := $(RARCH_DIR)/deps

GIT_VERSION := $(shell git rev-parse --short HEAD 2>/dev/null)
ifneq ($(GIT_VERSION),)
   DEFINES += -DHAVE_GIT_VERSION -DGIT_VERSION=$(GIT_VERSION)
endif

include $(CLEAR_VARS)
ifeq ($(TARGET_ARCH),arm)
   DEFINES += -DANDROID_ARM -marm
   LOCAL_ARM_MODE := arm
endif

ifeq ($(TARGET_ARCH),x86)
   DEFINES += -DANDROID_X86 -DHAVE_SSSE3
endif

ifeq ($(TARGET_ARCH),x86_64)
   DEFINES += -DANDROID_X64
endif

ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)

ifeq ($(HAVE_NEON),1)
	DEFINES += -D__ARM_NEON__ -DHAVE_NEON
endif
DEFINES += -DANDROID_ARM_V7
endif

ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
   DEFINES += -DANDROID_AARCH64
endif

ifeq ($(TARGET_ARCH),mips)
   DEFINES += -DANDROID_MIPS -D__mips__ -D__MIPSEL__
endif

LOCAL_MODULE := retroarch-activity

LOCAL_SRC_FILES  +=	$(RARCH_DIR)/griffin/griffin.c \
							$(RARCH_DIR)/griffin/griffin_cpp.cpp

# PASTIME: compile pastime menu driver + helper modules alongside griffin.
ifeq ($(HAVE_PASTIME),1)
LOCAL_SRC_FILES  +=	$(RARCH_DIR)/menu/drivers/pastime.c \
							$(RARCH_DIR)/pastime/pastime_cores.c \
							$(RARCH_DIR)/pastime/pastime_cores_extras.c \
							$(RARCH_DIR)/pastime/pastime_defaults.c \
							$(RARCH_DIR)/pastime/pastime_bootstrap.c \
							$(RARCH_DIR)/pastime/pastime_setup.c \
							$(RARCH_DIR)/pastime/pastime_nav.c \
							$(RARCH_DIR)/pastime/pastime_metadata_disambig.c \
							$(RARCH_DIR)/pastime/pastime_display_name.c \
							$(RARCH_DIR)/pastime/pastime_external.c \
							$(RARCH_DIR)/pastime/pastime_external_android.c \
							$(RARCH_DIR)/pastime/pastime_thumbs.c \
							$(RARCH_DIR)/pastime/pastime_thumbs_index.c \
							$(RARCH_DIR)/pastime/pastime_thumbhash.c \
							$(RARCH_DIR)/pastime/pastime_webp.c
# Vendored libwebp decoder.  Decoder-only subset; arch-specific
# (neon/sse2/sse41/mips/msa) variants are gated internally on compiler
# defines — including all of them is safe across ABIs.
LOCAL_SRC_FILES  +=	$(RARCH_DIR)/deps/libwebp/src/dec/alpha_dec.c \
							$(RARCH_DIR)/deps/libwebp/src/dec/buffer_dec.c \
							$(RARCH_DIR)/deps/libwebp/src/dec/frame_dec.c \
							$(RARCH_DIR)/deps/libwebp/src/dec/idec_dec.c \
							$(RARCH_DIR)/deps/libwebp/src/dec/io_dec.c \
							$(RARCH_DIR)/deps/libwebp/src/dec/quant_dec.c \
							$(RARCH_DIR)/deps/libwebp/src/dec/tree_dec.c \
							$(RARCH_DIR)/deps/libwebp/src/dec/vp8_dec.c \
							$(RARCH_DIR)/deps/libwebp/src/dec/vp8l_dec.c \
							$(RARCH_DIR)/deps/libwebp/src/dec/webp_dec.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/alpha_processing.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/alpha_processing_mips_dsp_r2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/alpha_processing_neon.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/alpha_processing_sse2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/alpha_processing_sse41.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/cpu.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/dec.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/dec_clip_tables.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/dec_mips32.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/dec_mips_dsp_r2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/dec_msa.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/dec_neon.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/dec_sse2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/dec_sse41.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/filters.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/filters_mips_dsp_r2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/filters_msa.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/filters_neon.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/filters_sse2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/lossless.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/lossless_mips_dsp_r2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/lossless_msa.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/lossless_neon.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/lossless_sse2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/lossless_sse41.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/rescaler.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/rescaler_mips32.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/rescaler_mips_dsp_r2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/rescaler_msa.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/rescaler_neon.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/rescaler_sse2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/upsampling.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/upsampling_mips_dsp_r2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/upsampling_msa.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/upsampling_neon.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/upsampling_sse2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/upsampling_sse41.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/yuv.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/yuv_mips32.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/yuv_mips_dsp_r2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/yuv_neon.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/yuv_sse2.c \
							$(RARCH_DIR)/deps/libwebp/src/dsp/yuv_sse41.c \
							$(RARCH_DIR)/deps/libwebp/src/utils/bit_reader_utils.c \
							$(RARCH_DIR)/deps/libwebp/src/utils/color_cache_utils.c \
							$(RARCH_DIR)/deps/libwebp/src/utils/filters_utils.c \
							$(RARCH_DIR)/deps/libwebp/src/utils/huffman_utils.c \
							$(RARCH_DIR)/deps/libwebp/src/utils/palette.c \
							$(RARCH_DIR)/deps/libwebp/src/utils/quant_levels_dec_utils.c \
							$(RARCH_DIR)/deps/libwebp/src/utils/random_utils.c \
							$(RARCH_DIR)/deps/libwebp/src/utils/rescaler_utils.c \
							$(RARCH_DIR)/deps/libwebp/src/utils/thread_utils.c \
							$(RARCH_DIR)/deps/libwebp/src/utils/utils.c
DEFINES          += -DHAVE_PASTIME_WEBP
endif

ifeq ($(HAVE_BUILTINSMBCLIENT),1)
   DEFINES += -DHAVE_BUILTINSMBCLIENT
   DEFINES += "-D_U_=__attribute__((unused))"
   DEFINES += -DHAVE_TIME_H -DHAVE_FCNTL_H -DHAVE_UNISTD_H
   DEFINES += -DHAVE_STDLIB_H -DSTDC_HEADERS
   DEFINES += -DHAVE_STRING_H
   DEFINES += -DHAVE_LINGER
   DEFINES += -DHAVE_SYS_UIO_H
   DEFINES += -DHAVE_POLL_H -DHAVE_NETDB_H
   DEFINES += -DHAVE_NETINET_TCP_H -DHAVE_NETINET_IN_H
   DEFINES += -DHAVE_SYS_SOCKET_H -DHAVE_ARPA_INET_H
   DEFINES += -DHAVE_SMBCLIENT
endif

ifeq ($(HAVE_LOGGER), 1)
   DEFINES += -DHAVE_LOGGER
endif
LOGGER_LDLIBS := -llog

ifeq ($(GLES),3)
   GLES_LIB := -lGLESv3
   DEFINES += -DHAVE_OPENGLES3
else
   GLES_LIB := -lGLESv2
   DEFINES += -DHAVE_OPENGLES2
endif

DEFINES += -DRARCH_MOBILE \
	   -DHAVE_GRIFFIN \
	   -DHAVE_STB_VORBIS \
	   -DHAVE_LANGEXTRA \
	   -DANDROID \
	   -DHAVE_DYNAMIC \
	   -DHAVE_OPENGL \
	   -DHAVE_OVERLAY \
	   -DHAVE_OPENGLES \
	   -DGLSL_DEBUG \
	   -DHAVE_DYLIB \
	   -DHAVE_EGL \
	   -DHAVE_GLSL \
	   -DHAVE_MENU \
	   -DHAVE_CONFIGFILE \
	   -DHAVE_PATCH \
	   -DHAVE_DSP_FILTER \
	   -DHAVE_VIDEO_FILTER \
	   -DHAVE_SCREENSHOTS \
	   -DHAVE_REWIND \
	   -DHAVE_CHEATS \
	   -DHAVE_BSV_MOVIE \
	   -DHAVE_ZLIB \
	   -DHAVE_NO_BUILTINZLIB \
	   -DHAVE_ZSTD \
	   -DZSTD_DISABLE_ASM \
	   -DHAVE_CHEEVOS_RVZ \
	   -DHAVE_RPNG \
	   -DHAVE_RJPEG \
	   -DHAVE_RBMP \
	   -DHAVE_RTGA \
	   -DHAVE_RWEBP \
	   -DINLINE=inline \
	   -DHAVE_THREADS \
	   -D__LIBRETRO__ \
	   -DHAVE_RSOUND \
	   -DHAVE_NETWORKGAMEPAD \
	   -DHAVE_NETWORKING \
	   -DHAVE_NETWORK_CMD \
	   -DHAVE_COMMAND \
	   -DHAVE_CLOUDSYNC \
	   -DHAVE_IFINFO \
	   -DHAVE_NETPLAYDISCOVERY \
	   -DRARCH_INTERNAL \
	   -DHAVE_FILTERS_BUILTIN \
	   -DHAVE_RGUI \
	   -DHAVE_MATERIALUI \
	   -DHAVE_XMB \
	   -DHAVE_OZONE \
	   -DHAVE_SHADERPIPELINE \
	   -DHAVE_LIBRETRODB \
	   -DHAVE_STB_FONT \
	   -DHAVE_IMAGEVIEWER \
	   -DHAVE_ONLINE_UPDATER \
	   -DHAVE_UPDATE_ASSETS \
	   -DHAVE_UPDATE_CORES \
	   -DHAVE_UPDATE_CORE_INFO \
	   -DHAVE_CC_RESAMPLER \
	   -DHAVE_KEYMAPPER \
	   -DHAVE_NETWORKGAMEPAD \
	   -DHAVE_FLAC \
	   -DHAVE_DR_FLAC \
	   -DHAVE_DR_MP3 \
	   -DHAVE_CHD \
	   -DWANT_SUBCODE \
	   -DWANT_RAW_DATA_SECTOR \
	   -DHAVE_RUNAHEAD \
	   -DHAVE_AUDIOMIXER \
	   -DHAVE_RWAV \
	   -DHAVE_ACCESSIBILITY \
	   -DHAVE_TRANSLATE \
	   -DWANT_IFADDRS \
	   -DHAVE_XDELTA \
	   -DHAVE_CORE_INFO_CACHE \
	   -DHAVE_BUILTINMBEDTLS -DHAVE_SSL

# PASTIME: enable the Pastime menu driver in the Android build.
ifeq ($(HAVE_PASTIME),1)
DEFINES += -DHAVE_PASTIME
endif

ifeq ($(HAVE_GFX_WIDGETS),1)
DEFINES += -DHAVE_GFX_WIDGETS
endif

ifeq ($(HAVE_VULKAN),1)
DEFINES += -DHAVE_VULKAN \
	   -DHAVE_SLANG \
	   -DHAVE_GLSLANG \
	   -DHAVE_BUILTINGLSLANG \
	   -DHAVE_SPIRV_CROSS \
	   -DWANT_GLSLANG \
	   -D__STDC_LIMIT_MACROS
endif
DEFINES += -DHAVE_7ZIP \
	   -D_7ZIP_ST \
	   -DHAVE_SL

ifeq ($(HAVE_CHEEVOS),1)
DEFINES += -DHAVE_CHEEVOS \
	   -DRC_DISABLE_LUA
endif

ifeq ($(HAVE_SAF),1)
   DEFINES += -DHAVE_SAF
endif

ifeq ($(HAVE_BUILTINSMBCLIENT),1)
   DEFINES += -DHAVE_SMBCLIENT
endif

DEFINES += -DFLAC_PACKAGE_VERSION="\"retroarch\"" \
	   -DHAVE_LROUND \
	   -DFLAC__HAS_OGG=0

LOCAL_CFLAGS   += -Wall -std=gnu99 -pthread -Wno-unused-function -fno-stack-protector -funroll-loops $(DEFINES)
LOCAL_CPPFLAGS := -fexceptions -fpermissive -std=gnu++11 -fno-rtti -Wno-reorder $(DEFINES)

# Let ndk-build set the optimization flags but remove -O3 like in cf3c3
LOCAL_CFLAGS := $(subst -O3,-O2,$(LOCAL_CFLAGS))

LOCAL_LDLIBS	 := -landroid -lEGL $(GLES_LIB) $(LOGGER_LDLIBS) -ldl
LOCAL_C_INCLUDES := \
		    $(LOCAL_PATH)/$(RARCH_DIR)/libretro-common/include \
		    $(LOCAL_PATH)/$(RARCH_DIR)/deps \
		    $(LOCAL_PATH)/$(RARCH_DIR)/deps/stb \
		    $(LOCAL_PATH)/$(RARCH_DIR)/deps/7zip \
		    $(LOCAL_PATH)/$(RARCH_DIR)/deps/zstd/lib

INCLUDE_DIRS     := \
		    -I$(LOCAL_PATH)/$(DEPS_DIR)/stb/ \
		    -I$(LOCAL_PATH)/$(DEPS_DIR)/7zip/ \
		    -I$(LOCAL_PATH)/$(DEPS_DIR)/zstd/lib/ \
		    -I$(LOCAL_PATH)/$(DEPS_DIR)/libFLAC/include

# PASTIME: vendored libwebp decoder include path (must come after the
# INCLUDE_DIRS := reset above).  libwebp internal sources use
# `#include "src/webp/decode.h"` style paths that resolve against this.
ifeq ($(HAVE_PASTIME),1)
INCLUDE_DIRS += -I$(LOCAL_PATH)/$(DEPS_DIR)/libwebp
endif

ifeq ($(HAVE_CHEEVOS),1)
INCLUDE_DIRS += -I$(LOCAL_PATH)/$(DEPS_DIR)/rcheevos/include
endif

ifeq ($(HAVE_BUILTINSMBCLIENT),1)
   INCLUDE_DIRS += \
      -I$(LOCAL_PATH)/$(DEPS_DIR)/libsmb2/include \
      -I$(LOCAL_PATH)/$(DEPS_DIR)/libsmb2/include/smb2
endif

LOCAL_CFLAGS     += $(INCLUDE_DIRS)
LOCAL_CPPFLAGS   += $(INCLUDE_DIRS)
LOCAL_CXXFLAGS   += $(INCLUDE_DIRS)

ifeq ($(HAVE_VULKAN),1)
INCFLAGS         += $(LOCAL_PATH)/$(RARCH_DIR)/gfx/include

LOCAL_C_INCLUDES += $(INCFLAGS)
LOCAL_CPPFLAGS   += -I$(LOCAL_PATH)/$(DEPS_DIR)/glslang \
		    -I$(LOCAL_PATH)/$(DEPS_DIR)/glslang/glslang/glslang/Public \
		    -I$(LOCAL_PATH)/$(DEPS_DIR)/glslang/glslang/glslang/MachineIndependent \
		    -I$(LOCAL_PATH)/$(DEPS_DIR)/glslang/glslang/SPIRV \
		    -I$(LOCAL_PATH)/$(DEPS_DIR)/SPIRV-Cross

LOCAL_CFLAGS    += -Wno-sign-compare -Wno-unused-variable -Wno-parentheses
LOCAL_SRC_FILES += $(RARCH_DIR)/griffin/griffin_glslang.cpp
endif

LOCAL_LDLIBS += -lOpenSLES -lz

ifneq ($(SANITIZER),)
   LOCAL_CFLAGS   += -g -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
   LOCAL_CPPFLAGS += -g -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
   LOCAL_LDFLAGS  += -fsanitize=$(SANITIZER)
endif

include $(BUILD_SHARED_LIBRARY)
