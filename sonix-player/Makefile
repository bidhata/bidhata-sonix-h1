# Makefile for sonix_player

# Target executables
HOST_BIN = sonix_player_host
TARGET_BIN = sonix_player
# Built from launcher/, see "sonix_launch" below.
LAUNCH_BIN = sonix_launch

# The unstripped copy of the target binary, kept beside the stripped one.
#
# It is what a backtrace, addr2line or gdb need: the stripped binary that goes
# on the device carries no symbol table and no DWARF, so an address out of a
# crash log means nothing without this file. The two are the same build, so an
# address from one resolves in the other.
TARGET_DEBUG_BIN = sonix_player_debug

LVGL_DIR = $(CURDIR)/lvgl
ROCKBOX_TOOLCHAIN_DIR = $(CURDIR)/rockbox-toolchain

CC = gcc
CXX = g++
CROSS_CC ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-gcc
CROSS_CXX ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-g++

# The interface uses the per-flag object API (lv_obj_set_hidden() and the
# rest) and lv_text_ap_strlen(), neither of which exists before 9.6. It also
# includes some of LVGL's private headers (lv_timer_private.h,
# lv_event_private.h, lv_roller_private.h and others) from src/.
LVGL_VER = 9.6.0

# Self-bootstrap: clone LVGL if it doesn't exist yet before evaluating variables
ifeq ($(wildcard $(LVGL_DIR)),)
$(info Cloning LVGL v$(LVGL_VER)...)
$(shell git clone --depth 1 -b v$(LVGL_VER) https://github.com/lvgl/lvgl.git)
endif

# Read out of the headers, so a checkout with no git history still answers.
# All three places are tried: the macros sit in lvgl.h up to 9.1, in
# lv_version.h from 9.2 and in include/lvgl/lv_version.h from 9.6.
LVGL_VER_FOUND := $(shell sed -n 's/^\#define LVGL_VERSION_\(MAJOR\|MINOR\|PATCH\) *\([0-9]*\).*/\2/p' \
					$(LVGL_DIR)/include/lvgl/lv_version.h $(LVGL_DIR)/lv_version.h $(LVGL_DIR)/lvgl.h \
					2>/dev/null | head -3 | paste -sd. -)
ifneq ($(LVGL_VER_FOUND),)
ifneq ($(LVGL_VER_FOUND),$(LVGL_VER))
$(warning lvgl/ holds $(LVGL_VER_FOUND) and this player needs $(LVGL_VER).)
$(warning Remove the lvgl directory and run make again to fetch the right one.)
$(error wrong LVGL version)
endif
endif

# Compile flags.
#
#   -DLV_CONF_INCLUDE_SIMPLE=1  lets lv_conf.h be included as "lv_conf.h".
#   -MMD -MP                    write a .d per object, pulled back in at the
#                               bottom of this file so a header edit rebuilds.
#   -D_FILE_OFFSET_BITS=64      required on the 32-bit target: without it
#                               readdir()/stat() fail with EOVERFLOW on an inode
#                               or offset wider than 32 bits, which an exFAT
#                               card produces routinely.
CFLAGS = -O3 -g -Wall -MMD -MP -D_FILE_OFFSET_BITS=64 -I. -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE=1 -DGB_CORE=1
# On the host the audio libraries are the system ones (libopusfile-dev,
# libwavpack-dev); on the target they are the ones this Makefile builds below.
# opusfile.h includes <opus_multistream.h> unprefixed, so the opus/ directory
# has to be among the -I in both cases.
HOST_CFLAGS = $(CFLAGS) -DHOST_BUILD=1 $(shell sdl2-config --cflags) $(shell pkg-config --cflags freetype2) \
			  $(shell pkg-config --cflags opusfile wavpack)
# -include glibc_compat.h must enter EVERY translation unit of the target
# build: it carries the .symver lines that keep five maths functions on the
# names the device's glibc 2.22 knows, and a .symver applies only to its own
# unit. Target only.
#
# -march=mips32r2: the toolchain is configured without --with-arch, and for the
# o32 ABI GCC then defaults to MIPS I. The X1600's core runs MIPS32r2 (the
# device's kernel modules and bluealsa are built for it), which gives mul, madd,
# conditional moves and no load delay slots.
#
# -mno-madd4: no madd.s/msub.s. Every float multiply and add stays a separate,
# separately rounded instruction, as in a MIPS I build.
#
# -mplt: the player is an executable at a fixed address, so its code need not
# be position-independent. Without this GCC compiles it as if it were a shared
# library: every function that calls another sets up $gp, every call loads its
# target from the GOT and jumps through a register, $gp is reloaded after each
# call, and every global is two loads. With it a call is a plain jal and a
# global is lui+lw; calls into the shared libraries go through a PLT the linker
# builds, and their data (stdout, stderr) is copied into the executable. The
# arithmetic is untouched: only how code and data are reached. Needs a dynamic
# linker with MIPS PLT support, which glibc has had since 2.9 (the device has
# 2.22). Ignored for anything built with -fPIC.
#
# -ffunction-sections -fdata-sections: every function and every object in a
# section of its own, so that --gc-sections (TARGET_LDFLAGS) can leave out the
# ones nothing calls -- the LVGL widgets the interface never creates, the parts
# of SQLite it never asks for. The player locks its code into RAM at startup
# (lock_code_pages in main.c), so code that is never run is memory that is
# never given back. What stays is compiled exactly as before.
TARGET_CFLAGS = $(CFLAGS) -march=mips32r2 -mno-madd4 -mplt -ffunction-sections -fdata-sections \
				-I$(LVGL_DIR)/src -I$(FREETYPE_PREFIX)/include/freetype2 \
				-I$(AUDIO_PREFIX)/include -I$(AUDIO_PREFIX)/include/opus \
				-DVIDEO_CORE=1 -I$(VIDEO_PREFIX)/include \
				-include src/system/core/glibc_compat.h

# How hard each target object is optimised, on top of TARGET_CFLAGS (the last
# -O on the command line is the one that counts).
#
# -O3 inlines and unrolls, and the code grows with it: worth it where the
# player spends its time with music playing or a game running, and nowhere
# else. Locked into RAM as all the code is (see above), the rest at -O2 is
# about half a megabyte given back, and on a core with a small instruction
# cache, smaller code is seldom slower code.
#
# The hot paths: the audio chain and its effects, the decoders, the Game Boy
# core, the video player's colour conversion, and LVGL's software renderer and
# colour mixing. SQLite is at -O2, the level it is normally built at.
#
# TARGET_OPT is per object: the default level everywhere, the hot one on the
# objects matching TARGET_HOT_PATTERNS.
TARGET_DEFAULT_OPT = -O2
TARGET_HOT_OPT = -O3
TARGET_HOT_PATTERNS = build_target/system/audio/% build_target/system/decode/% build_target/gb/% \
					  build_target/system/video/% \
					  build_target/lvgl/src/draw/sw/% build_target/lvgl/src/misc/lv_color%
TARGET_OPT = $(TARGET_DEFAULT_OPT)
$(TARGET_HOT_PATTERNS): TARGET_OPT = $(TARGET_HOT_OPT)

# The C++ in this project is the Game Boy core (src/gb, Gearboy) plus the facade
# that makes it callable from the player, which is C.
#
#   -DGEARBOY_DISABLE_DISASSEMBLER  drops the disassembler and the breakpoints:
#       they are for Gearboy's own debugger, which is not here.
#   -fno-exceptions -fno-rtti       the core uses neither, and without them a
#       few tables and some dead code go away.
#   The -I are needed because Gearboy's sources include each other by short name
#       ("Video.h"), as if they were in their own directory -- which is how they
#       sit upstream, and they are not touched so that the next update does not
#       have to be merged by hand.
#   -Wno-attributes                 Gearboy declares InvalidOPCode() noinline
#       first and inline afterwards. Upstream code, left alone on purpose (see
#       src/gb/README.md).
GB_CFLAGS = -DGEARBOY_DISABLE_DISASSEMBLER -fno-exceptions -fno-rtti -Wno-attributes \
			-Isrc/gb/core -Isrc/gb/miniz
HOST_CXXFLAGS = $(HOST_CFLAGS) $(GB_CFLAGS)
TARGET_CXXFLAGS = $(TARGET_CFLAGS) $(GB_CFLAGS)

# Link flags
#
# -static-libstdc++ -static-libgcc are not optional on the target: the device
# has libstdc++.so.6.0.21, that is GLIBCXX_3.4.21 (the GCC 5 ABI), and the
# toolchain is GCC 9, which emits references up to 3.4.26. Linked dynamically
# the binary does not start at all.
#
# Target only. On the host they ask for a static libstdc++ that most
# distributions do not ship, and the link fails with "cannot find -lstdc++".
GB_LDFLAGS = -static-libstdc++ -static-libgcc
HOST_LDFLAGS = $(shell sdl2-config --libs) -lpthread -lm -ldl -lasound $(shell pkg-config --libs freetype2) \
			   $(shell pkg-config --libs opusfile wavpack)
#
# --gc-sections: the other half of -ffunction-sections (see TARGET_CFLAGS).
TARGET_LDFLAGS = $(FREETYPE_PREFIX)/lib/libfreetype.a $(AUDIO_LIBS) $(VIDEO_LIBS) -lpthread -lm -ldl -lasound \
				 $(GB_LDFLAGS) -Wl,--gc-sections

# FreeType for the target, built static: the rootfs has no libfreetype.so. The
# host uses the system one. The device's fonts are plain OTF/TTF, so zlib,
# bzip2, png, harfbuzz and brotli are all off.
FREETYPE_VER = 2.13.3
FREETYPE_TARBALL = freetype-$(FREETYPE_VER).tar.xz
FREETYPE_URL = https://download.savannah.gnu.org/releases/freetype/$(FREETYPE_TARBALL)
FREETYPE_URL_MIRROR = https://downloads.sourceforge.net/project/freetype/freetype2/$(FREETYPE_VER)/$(FREETYPE_TARBALL)
FREETYPE_SHA256 = 0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
FREETYPE_SRC = $(CURDIR)/freetype-$(FREETYPE_VER)
FREETYPE_PREFIX = $(CURDIR)/freetype-target

# The audio libraries carried inside the binary, static like FreeType. The
# firmware's /usr/lib holds a libopus.so, but it is the raw codec with no Ogg
# demuxer, so linking against it is not an option.
#
#   libogg + libopus + opusfile -> .opus (src/system/decode/opusdec.c)
#   libwavpack                  -> .wv   (src/system/decode/wavpackdec.c)
#
# The configure options below were settled against these versions.
# ALAC is not here: its decoder is a single file under src/system/decode/alac/.
AUDIO_PREFIX = $(CURDIR)/audio-target

OGG_VER = 1.3.5
OGG_TARBALL = libogg-$(OGG_VER).tar.gz
OGG_URL = https://downloads.xiph.org/releases/ogg/$(OGG_TARBALL)
OGG_URL_MIRROR = https://ftp.osuosl.org/pub/xiph/releases/ogg/$(OGG_TARBALL)
OGG_SHA256 = 0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664
OGG_SRC = $(CURDIR)/libogg-$(OGG_VER)
OGG_LIB = $(AUDIO_PREFIX)/lib/libogg.a

OPUS_VER = 1.4
OPUS_TARBALL = opus-$(OPUS_VER).tar.gz
OPUS_URL = https://downloads.xiph.org/releases/opus/$(OPUS_TARBALL)
OPUS_URL_MIRROR = https://ftp.osuosl.org/pub/xiph/releases/opus/$(OPUS_TARBALL)
OPUS_SHA256 = c9b32b4253be5ae63d1ff16eea06b94b5f0f2951b7a02aceef58e3a3ce49c51f
OPUS_SRC = $(CURDIR)/opus-$(OPUS_VER)
OPUS_LIB = $(AUDIO_PREFIX)/lib/libopus.a

OPUSFILE_VER = 0.12
OPUSFILE_TARBALL = opusfile-$(OPUSFILE_VER).tar.gz
OPUSFILE_URL = https://downloads.xiph.org/releases/opus/$(OPUSFILE_TARBALL)
OPUSFILE_URL_MIRROR = https://ftp.osuosl.org/pub/xiph/releases/opus/$(OPUSFILE_TARBALL)
OPUSFILE_SHA256 = 118d8601c12dd6a44f52423e68ca9083cc9f2bfe72da7a8c1acb22a80ae3550b
OPUSFILE_SRC = $(CURDIR)/opusfile-$(OPUSFILE_VER)
OPUSFILE_LIB = $(AUDIO_PREFIX)/lib/libopusfile.a

WAVPACK_VER = 5.6.0
WAVPACK_TARBALL = wavpack-$(WAVPACK_VER).tar.bz2
WAVPACK_URL = https://www.wavpack.com/$(WAVPACK_TARBALL)
WAVPACK_URL_MIRROR = https://github.com/dbry/WavPack/releases/download/$(WAVPACK_VER)/$(WAVPACK_TARBALL)
WAVPACK_SHA256 = 8cbfa15927d29bcf953db35c0cfca7424344ff43ebe4083daf161577fb839cc1
WAVPACK_SRC = $(CURDIR)/wavpack-$(WAVPACK_VER)
WAVPACK_LIB = $(AUDIO_PREFIX)/lib/libwavpack.a

# The order matters: with static libraries the linker resolves left to right,
# and opusfile calls opus which calls ogg.
AUDIO_LIBS = $(OPUSFILE_LIB) $(OPUS_LIB) $(OGG_LIB) $(WAVPACK_LIB)

# Video (src/system/video): FFmpeg, static like the rest, and cut down to what
# this device can play -- see tools/video-probe for the measurements behind the
# list. MPEG-4 is the codec files are converted to (tools/video-convert.sh);
# H.264 is there for files small enough for it, MPEG-1/2 because they cost
# almost nothing. The sound decoders are the fixed-point ones.
#
# The host build has video only when the system has the FFmpeg development
# packages (libavformat-dev libavcodec-dev libavutil-dev); without them the
# page is simply absent from the simulator.
VIDEO_PREFIX = $(CURDIR)/video-target

FFMPEG_VER = 9.0.2
FFMPEG_TARBALL = ffmpeg-$(FFMPEG_VER).tar.xz
FFMPEG_URL = https://ffmpeg.org/releases/$(FFMPEG_TARBALL)
FFMPEG_SRC = $(CURDIR)/ffmpeg-$(FFMPEG_VER)
FFMPEG_LIB = $(VIDEO_PREFIX)/lib/libavcodec.a

FFMPEG_DECODERS = mpeg4 h264 mpeg2video mpeg1video aac_fixed mp3 mp2 ac3_fixed pcm_s16le
FFMPEG_DEMUXERS = mov matroska avi mpegps
FFMPEG_PARSERS = mpeg4video h264 mpegvideo aac mpegaudio ac3

# The order matters, as above: avformat calls avcodec calls avutil.
VIDEO_LIBS = $(VIDEO_PREFIX)/lib/libavformat.a $(VIDEO_PREFIX)/lib/libavcodec.a $(VIDEO_PREFIX)/lib/libavutil.a

HOST_VIDEO := $(shell pkg-config --exists libavformat libavcodec libavutil 2>/dev/null && echo 1)
ifeq ($(HOST_VIDEO),1)
HOST_CFLAGS += -DVIDEO_CORE=1 $(shell pkg-config --cflags libavformat libavcodec libavutil)
HOST_LDFLAGS += $(shell pkg-config --libs libavformat libavcodec libavutil)
endif


# Source files. src/gb holds a .c as well (the miniz shell) alongside the C++
# core, so both searches reach into it.
APP_SRCS := $(shell find src -name '*.c')
LVGL_SRCS = $(shell find $(LVGL_DIR)/src -type f -name '*.c')

# The C++ here is the Game Boy core under src/gb/core plus its facade. Separate
# list because it takes different flags, and the link goes through the C++
# compiler so that libstdc++ comes along.
APP_CXX_SRCS := $(shell find src/gb -name '*.cpp')
HOST_LD = $(CXX)
TARGET_LD = $(CROSS_CXX)

# Object files
HOST_OBJS = $(APP_SRCS:src/%.c=build_host/%.o) $(APP_CXX_SRCS:src/%.cpp=build_host/%.opp) \
			$(LVGL_SRCS:$(LVGL_DIR)/%.c=build_host/lvgl/%.o)
TARGET_OBJS = $(APP_SRCS:src/%.c=build_target/%.o) $(APP_CXX_SRCS:src/%.cpp=build_target/%.opp) \
			  $(LVGL_SRCS:$(LVGL_DIR)/%.c=build_target/lvgl/%.o)

.PHONY: all host target clean compile_commands.json test-radio-volume

all: host compile_commands.json

test-radio-volume: tools/radio_volume_test.c src/system/streaming/radio_volume.h src/system/streaming/radio.c
	gcc -std=c11 -Wall -Wextra -Werror -I. tools/radio_volume_test.c -o /tmp/radio_volume_test
	/tmp/radio_volume_test

host: $(HOST_BIN) compile_commands.json

$(HOST_BIN): $(HOST_OBJS)
	$(HOST_LD) -o $@ $(HOST_OBJS) $(HOST_LDFLAGS)
	@echo "Host build complete: Run './$(HOST_BIN)' to start the simulator."

# The key that opens streaming-keys.bin (src/system/streaming/streamkeys.h),
# kept outside git and compiled into streamkeys.c. The header is rewritten only
# when the key changes. Without the key file the build still works and the
# player reads only a plain streaming-keys.ini.
STREAMKEYS_KEY ?= streaming-keys.key
STREAMKEYS_KEY_H = build_gen/streamkeys_key.h
$(STREAMKEYS_KEY_H): FORCE
	@mkdir -p $(dir $@)
	@{ echo '// Generated by the Makefile from $(STREAMKEYS_KEY). Not in git.'; \
	  if [ -f "$(STREAMKEYS_KEY)" ]; then \
		hex=$$(tr -dc '0-9a-fA-F' < "$(STREAMKEYS_KEY)"); \
		[ $${#hex} -eq 64 ] || { echo "$(STREAMKEYS_KEY) is not 32 bytes of hex" >&2; exit 1; }; \
		echo '#define STREAMKEYS_KEY_LEN 32'; \
		echo "static const unsigned char streamkeys_key[STREAMKEYS_KEY_LEN] = {$$(echo $$hex | sed 's/../0x&,/g')};"; \
	  else \
		echo '#define STREAMKEYS_KEY_LEN 0'; \
	  fi; } > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; \
		if [ -f "$(STREAMKEYS_KEY)" ]; then echo "streamkeys: key from $(STREAMKEYS_KEY)"; \
		else echo "streamkeys: no $(STREAMKEYS_KEY), this build reads only a plain streaming-keys.ini"; fi; fi
build_host/system/streaming/streamkeys.o build_target/system/streaming/streamkeys.o: $(STREAMKEYS_KEY_H)

build_host/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

build_host/%.opp: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_CXXFLAGS) -c $< -o $@

build_host/lvgl/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

target: $(TARGET_BIN) $(LAUNCH_BIN) compile_commands.json

$(TARGET_BIN): $(TARGET_OBJS)
	$(TARGET_LD) -o $@ $(TARGET_OBJS) $(TARGET_LDFLAGS)
	@$(MAKE) --no-print-directory check-abi
	@$(MAKE) --no-print-directory strip-target
	@echo "Target build complete: File ready at '$(TARGET_BIN)'"

# ---------------------------------------------------------------------------
# sonix_launch
#
# The process sonix_player.sh hands itself over to: it starts the player and
# reboots the device when the player exits (launcher/sonix_launch.c). No libc
# and no loader, so it is built on its own: static, freestanding, non-PIC, and
# with the loop idioms GCC would turn into memset or strlen calls left as loops.
# The packer takes it from next to itself, like sonix_player.
# ---------------------------------------------------------------------------

LAUNCH_CFLAGS = -Os -Wall -Wextra -march=mips32r2 -mno-abicalls -fno-pic -G0 -static -nostdlib \
				-ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-stack-protector \
				-fno-asynchronous-unwind-tables -Wl,-e,__start

$(LAUNCH_BIN): launcher/sonix_launch.c $(CROSS_CC)
	$(CROSS_CC) $(LAUNCH_CFLAGS) -o $@ $<
	@if [ -x "$(CROSS_STRIP)" ]; then $(CROSS_STRIP) $@; fi
	@echo "sonix_launch: $$(wc -c < $@) bytes"

# ---------------------------------------------------------------------------
# Splitting the link output in two
#
# Runs after check-abi, which deletes both files when it fails: there is then
# nothing to copy, and no debug binary left claiming to match a build that was
# refused.
#
# The cross strip and not the host one. /usr/bin/strip is built for x86-64 and
# answers "file format not recognized" on a MIPS binary -- and it says so on
# stderr while exiting non-zero, which would take the whole build down.
# ---------------------------------------------------------------------------

CROSS_STRIP ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-strip

.PHONY: strip-target
strip-target:
	@if [ ! -f "$(TARGET_BIN)" ]; then \
		exit 0; \
	fi; \
	if [ ! -x "$(CROSS_STRIP)" ]; then \
		echo "strip-target: $(CROSS_STRIP) is not there, '$(TARGET_BIN)' left unstripped"; \
		exit 0; \
	fi; \
	cp -f $(TARGET_BIN) $(TARGET_DEBUG_BIN); \
	$(CROSS_STRIP) $(TARGET_BIN); \
	echo "strip-target: symbols in '$(TARGET_DEBUG_BIN)' ($$(du -h $(TARGET_DEBUG_BIN) | cut -f1)), stripped '$(TARGET_BIN)' ($$(du -h $(TARGET_BIN) | cut -f1))"

# ---------------------------------------------------------------------------
# The check the linker does not do
#
# The toolchain compiles against glibc 2.27, the device has 2.22. A binary
# asking for a newer versioned symbol links without a word and then refuses to
# start, and on this device that means a boot loop: sonix_launch runs
# `sleep 1; reboot` as soon as the player exits.
#
# glibc_compat.h holds the known functions down; this catches the next ones.
# ---------------------------------------------------------------------------

CROSS_READELF ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-readelf

# The device's glibc, as major.minor.
DEVICE_GLIBC_MAJOR = 2
DEVICE_GLIBC_MINOR = 22

.PHONY: check-abi
check-abi:
	@if [ ! -x "$(CROSS_READELF)" ]; then \
		echo "*** check-abi: $(CROSS_READELF) is not there, so '$(TARGET_BIN)' cannot be checked."; \
		echo "*** Set CROSS_READELF to the readelf of the toolchain in use."; \
		rm -f $(TARGET_BIN) $(TARGET_DEBUG_BIN); \
		exit 1; \
	fi; \
	syms=$$($(CROSS_READELF) --dyn-syms -W $(TARGET_BIN)) || { \
		echo "*** check-abi: $(CROSS_READELF) could not read '$(TARGET_BIN)'."; \
		rm -f $(TARGET_BIN) $(TARGET_DEBUG_BIN); \
		exit 1; \
	}; \
	bad=$$(echo "$$syms" \
		| awk '{ for (i = 1; i <= NF; i++) if (match($$i, /@GLIBC_[0-9.]+$$/)) { \
			ver = substr($$i, RSTART + 7); n = split(ver, v, "."); \
			if (v[1] > $(DEVICE_GLIBC_MAJOR) || (v[1] == $(DEVICE_GLIBC_MAJOR) && v[2] > $(DEVICE_GLIBC_MINOR))) print "  " $$i; \
		} }' | sort -u); \
	if [ -n "$$bad" ]; then \
		echo ""; \
		echo "*** ERROR: the binary asks for symbols the device does not have."; \
		echo "*** The device has glibc $(DEVICE_GLIBC_MAJOR).$(DEVICE_GLIBC_MINOR); these want more:"; \
		echo "$$bad"; \
		echo ""; \
		echo "*** As it stands the binary DOES NOT START, and the device boot loops."; \
		echo "*** Add a .symver in src/system/core/glibc_compat.h, which explains how."; \
		echo ""; \
		rm -f $(TARGET_BIN) $(TARGET_DEBUG_BIN); \
		exit 1; \
	fi; \
	echo "check-abi: no symbol beyond glibc $(DEVICE_GLIBC_MAJOR).$(DEVICE_GLIBC_MINOR)."

# The flags every target object was compiled with, the optimisation levels
# included. Rewritten only when they differ, so a change of flags rebuilds
# everything and nothing else does.
TARGET_FLAGS_STAMP = build_target/.flags
TARGET_FLAGS_LINE = $(TARGET_CFLAGS) | $(TARGET_CXXFLAGS) | $(TARGET_DEFAULT_OPT) $(TARGET_HOT_OPT) $(TARGET_HOT_PATTERNS)
$(TARGET_FLAGS_STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo '$(TARGET_FLAGS_LINE)' | cmp -s - $@ || \
		echo '$(TARGET_FLAGS_LINE)' > $@
FORCE:

build_target/%.o: src/%.c $(TARGET_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) $(TARGET_OPT) -c $< -o $@

build_target/%.opp: src/%.cpp $(TARGET_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CROSS_CXX) $(TARGET_CXXFLAGS) $(TARGET_OPT) -c $< -o $@

build_target/lvgl/%.o: $(LVGL_DIR)/%.c $(TARGET_FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) $(TARGET_OPT) -c $< -o $@

# For the Zed/clangd LSP: autofill and hover popups.
compile_commands.json:
	@python3 generate_compile_commands.py

# ---------------------------------------------------------------------------
# The accent's colour maths, on the host. Pure functions over lv_color_t, with
# no LVGL library and no display: theme_color.c is the same file the player
# builds. Test-only: nothing here is part of the host or target builds.
# ---------------------------------------------------------------------------
THEME_COLOR_TEST_BIN = build_host/test_theme_color
.PHONY: test-theme-color
test-theme-color: $(THEME_COLOR_TEST_BIN)
	$(THEME_COLOR_TEST_BIN)

$(THEME_COLOR_TEST_BIN): tools/test_theme_color.c src/gui/shell/theme_color.c src/gui/shell/theme_color.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DHOST_BUILD=1 -o $@ tools/test_theme_color.c src/gui/shell/theme_color.c -lm

clean:
	rm -rf build_host build_target build_gen $(HOST_BIN) $(TARGET_BIN) $(TARGET_DEBUG_BIN) $(LAUNCH_BIN) compile_commands.json compile_flags.txt

# ---------------------------------------------------------------------------
# The AXP2101 charge state machine, on the host, against the register map
# axpcharge.c keeps under HOST_BUILD. Test-only: nothing here is part of the
# host or target builds.
# ---------------------------------------------------------------------------
AXPCHARGE_TEST_BIN = build_host/test_axpcharge
.PHONY: test-axpcharge
test-axpcharge: $(AXPCHARGE_TEST_BIN)
	$(AXPCHARGE_TEST_BIN)

$(AXPCHARGE_TEST_BIN): tools/test_axpcharge.c src/system/device/axpcharge.c \
		src/system/device/axpcharge_test.h src/system/device/axpcharge.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DHOST_BUILD=1 -DAXPCHARGE_TEST=1 -o $@ \
		tools/test_axpcharge.c src/system/device/axpcharge.c -lpthread

# The bundled libraries and their extracted sources. The downloaded tarballs stay.
.PHONY: libs-clean
libs-clean:
	rm -rf $(FREETYPE_PREFIX) $(AUDIO_PREFIX) $(FREETYPE_SRC) $(OGG_SRC) $(OPUS_SRC) $(OPUSFILE_SRC) $(WAVPACK_SRC)
	rm -rf $(VIDEO_PREFIX) $(FFMPEG_SRC)

# Make sure the cross compiler is built before trying to make any of the target
# objects with it.
$(TARGET_OBJS): $(CROSS_CC) $(FREETYPE_PREFIX)/lib/libfreetype.a $(AUDIO_LIBS) $(FFMPEG_LIB)
$(TARGET_BIN): $(FREETYPE_PREFIX)/lib/libfreetype.a $(AUDIO_LIBS) $(FFMPEG_LIB)

$(CROSS_CC):
	@echo "Building Rockbox toolchain..."
	./rockboxdev/rockboxdev.sh

# rockboxdev.sh keeps this file in the toolchain directory from its start to
# its successful end. The compiler is installed long before the end (GCC stage
# 1, with no C library), so while the file is there the compiler being there
# does not mean the toolchain is: the script runs again.
ifneq ($(wildcard $(ROCKBOX_TOOLCHAIN_DIR)/.rbdev-incomplete),)
$(CROSS_CC): FORCE
endif

# $(call fetch,TARBALL,URL,MIRROR,SHA256): downloads a tarball that is not
# there yet. It is written as TARBALL.part and takes its name only once it is
# whole and its sha256 is the pinned one, so a download that fails or is cut
# short leaves nothing the next run would take for the tarball.
define fetch
	@if [ ! -f $(1) ]; then \
		{ wget -O $(1).part $(2) || wget -O $(1).part $(3); } && \
		sum=$$({ sha256sum $(1).part 2>/dev/null || shasum -a 256 $(1).part; } | cut -d' ' -f1) && \
		{ [ "$$sum" = "$(4)" ] || { echo "$(1): sha256 is $$sum, not $(4)" >&2; exit 1; }; } && \
		mv $(1).part $(1); \
	fi
endef

# The flags FreeType and the audio libraries are built with. -march and -mplt as
# in TARGET_CFLAGS: they are static, so they end up in the same executable. They
# are built once: `make libs-clean` removes them, with their extracted sources,
# so the next `make target` builds them again.
LIB_CFLAGS = -O2 -march=mips32r2 -mplt

# Cross-build FreeType, once. configure needs the toolchain on PATH for
# ranlib/ar to be found under their host-triplet names.
$(FREETYPE_PREFIX)/lib/libfreetype.a: $(CROSS_CC)
	@echo "Building static FreeType $(FREETYPE_VER) for the target..."
	$(call fetch,$(FREETYPE_TARBALL),$(FREETYPE_URL),$(FREETYPE_URL_MIRROR),$(FREETYPE_SHA256))
	rm -rf $(FREETYPE_SRC) && tar xf $(FREETYPE_TARBALL)
	cd $(FREETYPE_SRC) && PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH" ./configure \
		--host=mipsel-rockbox-linux-gnu \
		--prefix=$(FREETYPE_PREFIX) \
		--disable-shared --enable-static \
		--with-zlib=no --with-bzip2=no --with-png=no \
		--with-harfbuzz=no --with-brotli=no \
		CC=$(CROSS_CC) CFLAGS="$(LIB_CFLAGS)" \
		AR=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ar \
		RANLIB=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ranlib
	PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH" $(MAKE) -C $(FREETYPE_SRC)
	PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH" $(MAKE) -C $(FREETYPE_SRC) install

# ---------------------------------------------------------------------------
# Downloaded, configured with the toolchain on PATH (configure looks for ar and
# ranlib under the triplet name) and installed into AUDIO_PREFIX. The .a is the
# make target, so each is built once.
# ---------------------------------------------------------------------------

CROSS_PATH = PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH"
CROSS_TOOLS = CC=$(CROSS_CC) CFLAGS="$(LIB_CFLAGS)" \
			  AR=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ar \
			  RANLIB=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ranlib
CROSS_COMMON = --host=mipsel-rockbox-linux-gnu --prefix=$(AUDIO_PREFIX) --disable-shared --enable-static

$(OGG_LIB): $(CROSS_CC)
	@echo "Building static libogg $(OGG_VER) for the target..."
	$(call fetch,$(OGG_TARBALL),$(OGG_URL),$(OGG_URL_MIRROR),$(OGG_SHA256))
	rm -rf $(OGG_SRC) && tar xf $(OGG_TARBALL)
	cd $(OGG_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) $(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(OGG_SRC)
	$(CROSS_PATH) $(MAKE) -C $(OGG_SRC) install

# --enable-fixed-point: integer decoder, bit-exact across platforms. The float
# API stays compiled in -- removing it would take --disable-float-api.
$(OPUS_LIB): $(CROSS_CC)
	@echo "Building static libopus $(OPUS_VER) for the target..."
	$(call fetch,$(OPUS_TARBALL),$(OPUS_URL),$(OPUS_URL_MIRROR),$(OPUS_SHA256))
	rm -rf $(OPUS_SRC) && tar xf $(OPUS_TARBALL)
	cd $(OPUS_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) \
		--enable-fixed-point --disable-doc --disable-extra-programs \
		$(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(OPUS_SRC)
	$(CROSS_PATH) $(MAKE) -C $(OPUS_SRC) install

# DEPS_CFLAGS/DEPS_LIBS by hand: opusfile's configure finds ogg and opus with
# PKG_CHECK_MODULES, and no pkg-config here knows the cross sysroot. Passing
# them skips the search. --disable-http drops the stream support that would
# pull in OpenSSL.
$(OPUSFILE_LIB): $(CROSS_CC) $(OGG_LIB) $(OPUS_LIB)
	@echo "Building static opusfile $(OPUSFILE_VER) for the target..."
	$(call fetch,$(OPUSFILE_TARBALL),$(OPUSFILE_URL),$(OPUSFILE_URL_MIRROR),$(OPUSFILE_SHA256))
	rm -rf $(OPUSFILE_SRC) && tar xf $(OPUSFILE_TARBALL)
	cd $(OPUSFILE_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) \
		--disable-http --disable-examples --disable-doc \
		DEPS_CFLAGS="-I$(AUDIO_PREFIX)/include -I$(AUDIO_PREFIX)/include/opus" \
		DEPS_LIBS="-L$(AUDIO_PREFIX)/lib -lopus -logg" \
		$(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(OPUSFILE_SRC)
	$(CROSS_PATH) $(MAKE) -C $(OPUSFILE_SRC) install

# --enable-legacy: also reads the .wv written by WavPack 4.0 and earlier, which
# costs a few kilobytes. --enable-dsd keeps DSD inside WavPack, which the
# wrapper opens with OPEN_DSD_AS_PCM. --disable-asm because the assembly wavpack
# ships is for x86 and ARM.
$(WAVPACK_LIB): $(CROSS_CC)
	@echo "Building static libwavpack $(WAVPACK_VER) for the target..."
	$(call fetch,$(WAVPACK_TARBALL),$(WAVPACK_URL),$(WAVPACK_URL_MIRROR),$(WAVPACK_SHA256))
	rm -rf $(WAVPACK_SRC) && tar xf $(WAVPACK_TARBALL)
	cd $(WAVPACK_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) \
		--disable-apps --disable-man --disable-asm \
		--enable-dsd --enable-legacy --without-libiconv-prefix \
		$(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(WAVPACK_SRC)
	$(CROSS_PATH) $(MAKE) -C $(WAVPACK_SRC) install

# FFmpeg has its own configure, not autoconf's, so none of CROSS_COMMON applies.
#
# --disable-everything, then the lists above: the whole of FFmpeg is fifteen
# megabytes of code and this is under two.
#
# Every MIPS extension is off. FFmpeg's assembly is for DSP, MSA and Loongson
# cores, and the X1600 has none of them (/proc/cpuinfo lists no ASE at all), so
# the decoders are plain C.
#
# -ffunction-sections -fdata-sections, as in TARGET_CFLAGS and for the same
# reason: the player's code is locked into RAM, and --gc-sections can only
# leave out what sits in a section of its own. Without them the binary grows by
# 3.3 MB for video.
#
# -include glibc_compat.h: FFmpeg calls powf, expf and the rest, and the .a
# ends up inside the player, where check-abi would refuse them. LIB_CFLAGS'
# -march and -mplt come from --cpu and --extra-cflags; -mno-madd4 as in
# TARGET_CFLAGS.
$(FFMPEG_LIB): $(CROSS_CC)
	@echo "Building static FFmpeg $(FFMPEG_VER) for the target..."
	@if [ ! -f $(FFMPEG_TARBALL) ]; then \
		wget -O $(FFMPEG_TARBALL) $(FFMPEG_URL); \
	fi
	rm -rf $(FFMPEG_SRC) && tar xf $(FFMPEG_TARBALL)
	cd $(FFMPEG_SRC) && $(CROSS_PATH) ./configure \
		--prefix=$(VIDEO_PREFIX) \
		--enable-cross-compile --cross-prefix=mipsel-rockbox-linux-gnu- \
		--arch=mipsel --target-os=linux --cpu=mips32r2 \
		--extra-cflags="-mno-madd4 -mplt -ffunction-sections -fdata-sections -include $(CURDIR)/src/system/core/glibc_compat.h" \
		--disable-mipsdsp --disable-mipsdspr2 --disable-msa --disable-mmi --disable-mipsfpu \
		--disable-loongson2 --disable-loongson3 \
		--enable-static --disable-shared --disable-pic \
		--disable-everything --disable-autodetect --disable-programs --disable-doc \
		--disable-network --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
		--disable-debug \
		--enable-protocol=file \
		$(addprefix --enable-decoder=,$(FFMPEG_DECODERS)) \
		$(addprefix --enable-demuxer=,$(FFMPEG_DEMUXERS)) \
		$(addprefix --enable-parser=,$(FFMPEG_PARSERS))
	$(CROSS_PATH) $(MAKE) -C $(FFMPEG_SRC)
	$(CROSS_PATH) $(MAKE) -C $(FFMPEG_SRC) install

# Header dependencies recorded by -MMD (see CFLAGS). The dashes keep make quiet
# on the first build, when no .d files exist yet. The C++ objects end in .opp
# so the two compile rules cannot overlap, which is why both suffixes are
# substituted -- otherwise make reads an object file as a makefile.
-include $(patsubst %.opp,%.d,$(patsubst %.o,%.d,$(HOST_OBJS)))
-include $(patsubst %.opp,%.d,$(patsubst %.o,%.d,$(TARGET_OBJS)))
