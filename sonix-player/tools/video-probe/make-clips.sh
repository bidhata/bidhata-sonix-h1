#!/usr/bin/env bash
# Make the test clips decprobe is run against: one 10-second source, encoded as
# every codec worth trying at every size worth trying. Runs on the build
# machine, not on the device.
#
#   tools/video-probe/make-clips.sh [source-video]
#
# With no argument it downloads a 10-second 720p Big Buck Bunny excerpt. Give it
# any video of your own to test with that instead (the first 10 seconds are used).
#
# No ffmpeg is assumed on the machine: a small one, with x264, is built from
# source into tools/video-probe/.build the first time. The clips land in
# tools/video-probe/.build/clips.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

FFMPEG_VER=9.0.2
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz"
X264_URL="https://code.videolan.org/videolan/x264/-/archive/stable/x264-stable.tar.bz2"
SAMPLE_URL="https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/720/Big_Buck_Bunny_720_10s_5MB.mp4"

build="$here/.build"
host="$build/host"
clips="$build/clips"
mkdir -p "$build" "$clips"

# ---- a host ffmpeg, once ----
ffmpeg="$host/bin/ffmpeg"
if [ ! -x "$ffmpeg" ]; then
	if [ ! -f "$host/lib/libx264.a" ]; then
		[ -f "$build/x264-stable.tar.bz2" ] || curl -fL -o "$build/x264-stable.tar.bz2" "$X264_URL"
		rm -rf "$build/x264-stable" && tar -C "$build" -xf "$build/x264-stable.tar.bz2"
		(
			cd "$build/x264-stable"
			# --disable-asm: no nasm needed; 10-second clips do not need the speed.
			./configure --prefix="$host" --enable-static --disable-cli --disable-asm --disable-opencl
			make -j"$(nproc)"
			make install
		)
	fi

	src="$build/ffmpeg-$FFMPEG_VER"
	if [ ! -d "$src" ]; then
		[ -f "$build/ffmpeg-$FFMPEG_VER.tar.xz" ] || curl -fL -o "$build/ffmpeg-$FFMPEG_VER.tar.xz" "$FFMPEG_URL"
		tar -C "$build" -xf "$build/ffmpeg-$FFMPEG_VER.tar.xz"
	fi
	rm -rf "$build/ffmpeg-host-obj" && mkdir -p "$build/ffmpeg-host-obj"
	# --disable-asm, not --disable-x86asm: there is no nasm here, and with only
	# the external assembly off FFmpeg keeps its inline MMX, which without the
	# rest of the x86 code makes the scaler write garbage -- every clip comes
	# out as noise, at the right size and bitrate, and nothing complains.
	(
		cd "$build/ffmpeg-host-obj"
		PKG_CONFIG_PATH="$host/lib/pkgconfig" "$src/configure" \
			--prefix="$host" --pkg-config-flags=--static \
			--disable-everything --disable-autodetect --disable-doc --disable-ffplay --disable-ffprobe \
			--disable-asm --disable-network --disable-debug \
			--enable-gpl --enable-libx264 \
			--enable-encoder=libx264,mpeg4,mpeg2video,mjpeg \
			--enable-decoder=h264,hevc,mpeg4,mpeg2video,vp8,vp9,mjpeg \
			--enable-demuxer=mov,matroska,avi,mpegts,mpegps \
			--enable-muxer=matroska \
			--enable-parser=h264,hevc,mpeg4video,mpegvideo,vp8,vp9 \
			--enable-filter=scale,fps,format \
			--enable-protocol=file
		make -j"$(nproc)"
		make install
	)
fi

# ---- the source ----
source_clip="${1:-}"
if [ -z "$source_clip" ]; then
	source_clip="$build/sample-720p.mp4"
	[ -f "$source_clip" ] || curl -fL -o "$source_clip" "$SAMPLE_URL"
fi
if [ ! -f "$source_clip" ]; then
	echo "error: no such file: $source_clip" >&2
	exit 1
fi

# ---- the matrix ----
# Sizes: 16:9, from what fills the 800x480 panel sideways down to a quarter of
# it. Bitrates: what each size is usually encoded at for H.264; the two MPEG
# codecs get half as much again, as they need it for the same picture.
#           WxH      h264 kbit/s
SIZES="800x450:1500 640x360:1000 480x270:600 320x180:350"
FPS=30

encode() { # name, then encoder arguments
	local out="$clips/$1.mkv"
	shift
	if [ -f "$out" ]; then return; fi
	echo "  $(basename "$out")"
	"$ffmpeg" -hide_banner -loglevel error -y -t 10 -i "$source_clip" -an "$@" "$out"
}

echo "encoding into $clips"
for entry in $SIZES; do
	size="${entry%%:*}"
	kb="${entry##*:}"
	w="${size%x*}"
	h="${size#*x}"
	vf="scale=$w:$h,fps=$FPS,format=yuv420p"
	mkb=$((kb * 3 / 2))

	# H.264 Baseline: no B-frames, CAVLC. The cheapest H.264 there is to decode.
	encode "h264-baseline-$size" -vf "$vf" -c:v libx264 -profile:v baseline -preset medium -b:v "${kb}k"
	# H.264 High: B-frames, CABAC, 8x8 transform. What most files in the wild are.
	encode "h264-high-$size" -vf "$vf" -c:v libx264 -profile:v high -preset medium -b:v "${kb}k"
	# MPEG-4 ASP (the Xvid/DivX codec), with B-frames.
	encode "mpeg4-$size" -vf "$vf" -c:v mpeg4 -bf 2 -b:v "${mkb}k"
	# MPEG-2.
	encode "mpeg2-$size" -vf "$vf" -c:v mpeg2video -bf 2 -b:v "${mkb}k"
	# Motion JPEG: every frame on its own, no motion compensation at all.
	encode "mjpeg-$size" -vf "scale=$w:$h,fps=$FPS,format=yuvj420p" -c:v mjpeg -q:v 5
done

echo ""
ls -l "$clips"
echo ""
echo "Next: copy tools/video-probe/decprobe and $clips to the SD card,"
echo "stop the player, and on the device run:  ./decprobe clips/*.mkv"
