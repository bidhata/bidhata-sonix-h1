#!/usr/bin/env bash
# Cross-build decprobe for the device: a cut-down static FFmpeg (video decoders
# and the demuxers around them, nothing else) and the probe on top of it. Ends
# with the same glibc-2.22 ABI gate as build.sh. Produces
# tools/video-probe/decprobe (a static MIPS binary).
#
# Everything downloaded or built along the way lives in tools/video-probe/.build
# and can be deleted at any time.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"   # .../sonix-player/sonix-player

FFMPEG_VER=9.0.2
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz"

TOOLCHAIN="$root/rockbox-toolchain"
TRIPLET=mipsel-rockbox-linux-gnu
CC="$TOOLCHAIN/bin/$TRIPLET-gcc"
READELF="$TOOLCHAIN/bin/$TRIPLET-readelf"
STRIP="$TOOLCHAIN/bin/$TRIPLET-strip"

if [ ! -x "$CC" ]; then
	echo "error: cross compiler not found at $CC" >&2
	echo "       build it first:  ./rockboxdev/rockboxdev.sh" >&2
	exit 1
fi

build="$here/.build"
src="$build/ffmpeg-$FFMPEG_VER"
obj="$build/ffmpeg-target-obj"
prefix="$build/ffmpeg-target"
mkdir -p "$build"

if [ ! -d "$src" ]; then
	[ -f "$build/ffmpeg-$FFMPEG_VER.tar.xz" ] || curl -fL -o "$build/ffmpeg-$FFMPEG_VER.tar.xz" "$FFMPEG_URL"
	tar -C "$build" -xf "$build/ffmpeg-$FFMPEG_VER.tar.xz"
fi

# The decoders worth asking the question of on a single 1 GHz MIPS core with no
# SIMD. HEVC, VP9 and AV1 are left out on purpose: several times the work of
# H.264, and H.264 is already the open question.
DECODERS="h264 mpeg4 msmpeg4v3 mpeg2video mpeg1video mjpeg vp8 flv h263"
DEMUXERS="mov matroska avi mpegps mpegts flv m4v h264 mpegvideo"
PARSERS="h264 mpeg4video mpegvideo mjpeg vp8 h263"

# -march=mips32r2 -mno-madd4 -mplt: as the player's own TARGET_CFLAGS (see the
# Makefile for why). FFmpeg's MIPS assembly is all for extensions the X1600
# does not have (its /proc/cpuinfo lists no ASE at all), so every one is off
# and the decoders are plain C.
if [ ! -f "$prefix/lib/libavcodec.a" ]; then
	rm -rf "$obj" && mkdir -p "$obj"
	en=()
	for d in $DECODERS; do en+=("--enable-decoder=$d"); done
	for d in $DEMUXERS; do en+=("--enable-demuxer=$d"); done
	for d in $PARSERS; do en+=("--enable-parser=$d"); done
	(
		cd "$obj"
		PATH="$TOOLCHAIN/bin:$PATH" "$src/configure" \
			--prefix="$prefix" \
			--enable-cross-compile --cross-prefix="$TRIPLET-" \
			--arch=mipsel --target-os=linux --cpu=mips32r2 \
			--extra-cflags="-mno-madd4 -mplt" \
			--disable-mipsdsp --disable-mipsdspr2 --disable-msa --disable-mmi --disable-mipsfpu \
			--disable-loongson2 --disable-loongson3 \
			--enable-static --disable-shared --disable-pic \
			--disable-everything --disable-autodetect --disable-programs --disable-doc \
			--disable-network --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
			--disable-debug \
			--enable-protocol=file \
			"${en[@]}"
		PATH="$TOOLCHAIN/bin:$PATH" make -j"$(nproc)"
		PATH="$TOOLCHAIN/bin:$PATH" make install
	)
fi

out="$here/decprobe"
"$CC" -O2 -march=mips32r2 -mno-madd4 -mplt -Wall -Wextra \
	-static -I"$prefix/include" \
	-o "$out" "$here/decprobe.c" \
	-L"$prefix/lib" -lavformat -lavcodec -lavutil -lpthread -lm
"$STRIP" "$out"
echo "built: $out"

# ---- the glibc-2.22 ABI gate, same logic as the Makefile's check-abi ----
bad="$("$READELF" --dyn-syms -W "$out" 2>/dev/null \
	| awk '{ for (i = 1; i <= NF; i++) if (match($i, /@GLIBC_[0-9.]+$/)) {
		ver = substr($i, RSTART + 7); n = split(ver, v, ".");
		if (v[1] > 2 || (v[1] == 2 && v[2] > 22)) print "  " $i;
	} }' | sort -u || true)"

if [ -n "$bad" ]; then
	echo "" >&2
	echo "*** ABI FAIL: asks for symbols newer than glibc 2.22:" >&2
	echo "$bad" >&2
	exit 1
fi
echo "check-abi: no symbol beyond glibc 2.22."

file "$out" 2>/dev/null || true
ls -l "$out"
echo ""
echo "Next: make test clips (tools/video-probe/make-clips.sh), copy decprobe and the"
echo "clips to the SD card, stop the player, and run:  ./decprobe clips/*.mkv"
