#!/bin/sh
# Every audio format the player decodes, through the player's own decoder
# layer: a twenty-second tone, 440 Hz in the left ear and 660 Hz in the right,
# is encoded with ffmpeg in each format and decoded again by
# tests/decode_formats_test.c, which checks rate, length, note and level.
#
# Needs ffmpeg, and the development files of opusfile and wavpack, as the
# simulator does. AAC is decoded by libfdk-aac and WAV, AIFF and CAF by
# libsndfile, both loaded at run time as on the device: where one of them is
# not installed its formats are skipped, and said to be.
#
# Not here: plain WAV, which the output stage reads by itself without this
# layer (src/system/audio/audio.c), and DSD and SACD, which have tests of
# their own in tests/run.sh.
set -e
cd "$(dirname "$0")/.."

WORK="$(mktemp -d "${TMPDIR:-/tmp}/decode-test.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# With the two files the decoders lean on outside their own folder: cue.c for
# a track of a cue sheet, utils.c for a few helpers. --gc-sections drops what
# those two would otherwise drag in after them.
cc -O2 -Wall -D_FILE_OFFSET_BITS=64 -DHOST_BUILD=1 -I. -Isrc $(pkg-config --cflags opusfile wavpack) \
   -ffunction-sections -fdata-sections -Wl,--gc-sections \
   -o "$WORK/decode_formats_test" tests/decode_formats_test.c \
   src/system/library/cue.c src/system/core/utils.c \
   src/system/decode/decode.c src/system/decode/aacdec.c src/system/decode/alacdec.c \
   src/system/decode/apedec.c src/system/decode/dsd.c src/system/decode/dst_decoder.c \
   src/system/decode/growfile.c src/system/decode/mp4.c src/system/decode/opusdec.c \
   src/system/decode/sacd_iso.c src/system/decode/sndfile.c src/system/decode/stb_vorbis.c \
   src/system/decode/wavpackdec.c $(find src/system/decode/alac src/system/decode/ape -name '*.c') \
   $(pkg-config --libs opusfile wavpack) -lpthread -lm -ldl
RUN="$WORK/decode_formats_test"

SECONDS_LONG=20
LEFT=440
RIGHT=660

have_lib() { ldconfig -p 2> /dev/null | grep -q "$1"; }
have_encoder() { ffmpeg -hide_banner -encoders 2> /dev/null | grep -q " $1 "; }

failed=0
skipped=""

# one NAME RATE ffmpeg-arguments...
one() {
	name="$1"
	rate="$2"
	shift 2
	if ! ffmpeg -hide_banner -loglevel error -y \
		-f lavfi -i "sine=frequency=$LEFT:duration=$SECONDS_LONG:sample_rate=$rate" \
		-f lavfi -i "sine=frequency=$RIGHT:duration=$SECONDS_LONG:sample_rate=$rate" \
		-filter_complex "[0:a][1:a]join=inputs=2:channel_layout=stereo,volume=2[a]" -map "[a]" \
		"$@" "$WORK/$name"; then
		echo "FAIL $name: ffmpeg could not make it"
		failed=1
		return
	fi
	( cd "$WORK" && ./decode_formats_test "$name" "$rate" "$SECONDS_LONG" "$LEFT" "$RIGHT" ) || failed=1
}

one tone.flac 44100 -c:a flac
one tone24.flac 96000 -c:a flac -sample_fmt s32 -bits_per_raw_sample 24
if have_encoder libmp3lame; then
	one tone.mp3 44100 -c:a libmp3lame -b:a 192k
else
	skipped="$skipped mp3(no libmp3lame in ffmpeg)"
fi
one tone.ogg 44100 -c:a libvorbis -q:a 5
one tone.opus 48000 -c:a libopus -b:a 128k
one tone.wv 44100 -c:a wavpack
one alac.m4a 44100 -c:a alac
if have_lib libfdk-aac; then
	one aac.m4a 44100 -c:a aac -b:a 192k
	one tone.aac 44100 -c:a aac -b:a 192k
else
	skipped="$skipped aac(libfdk-aac not installed)"
fi
if have_lib libsndfile; then
	one tone.aiff 44100 -c:a pcm_s16be
	one tone24.aiff 48000 -c:a pcm_s24be
	one tone.caf 44100 -c:a pcm_s16le
else
	skipped="$skipped aiff/caf(libsndfile not installed)"
fi

[ -z "$skipped" ] || echo "decode formats: skipped:$skipped"
if [ "$failed" -ne 0 ]; then
	echo "decode formats: FAILED"
	exit 1
fi
echo "decode formats: all passed"
