#!/bin/sh
# Builds and runs the decoder tests. Invoked from the project root as
# `sh tests/run.sh`; it finds the root itself otherwise.
set -e
cd "$(dirname "$0")/.."

# Task 1: the DST decoder, standalone, no other sources needed.
cc -O2 -Wall -Wextra -Isrc -o /tmp/dst_decoder_test \
   tests/dst_decoder_test.c src/system/decode/dst_decoder.c
/tmp/dst_decoder_test

# Task 2: the SACD ISO reader, against synthetic images.
ISO_DIR=/tmp/sacd-tests
rm -rf "$ISO_DIR"
mkdir -p "$ISO_DIR"
python3 tests/make_sacd_iso.py "$ISO_DIR"
cc -O2 -Wall -Wextra -Isrc -o /tmp/sacd_iso_test \
   tests/sacd_iso_test.c src/system/decode/sacd_iso.c \
   src/system/decode/dst_decoder.c
for mode in dsd14 dsd16 dst dst_shared bad; do
	/tmp/sacd_iso_test "$ISO_DIR/$mode.iso" "$mode"
done

# Task 3: SACD ISO tracks through the DSD output stage (DoP and PCM).
cc -O2 -Wall -Wextra -pthread -Isrc -o /tmp/dsd_sacd_test \
   tests/dsd_sacd_test.c src/system/decode/dsd.c \
   src/system/decode/sacd_iso.c src/system/decode/dst_decoder.c
/tmp/dsd_sacd_test "$ISO_DIR/dsd14.iso"

# Task 3b: the plain .dsf/.dff containers of the same stage, not an image.
# This is the regression smoke for the DSD formats the SACD work sits beside.
python3 tests/make_dsd_fixtures.py "$ISO_DIR"
cc -O2 -Wall -Wextra -pthread -Isrc -o /tmp/dsd_regression_test \
   tests/dsd_regression_test.c src/system/decode/dsd.c \
   src/system/decode/sacd_iso.c src/system/decode/dst_decoder.c
/tmp/dsd_regression_test "$ISO_DIR/stereo.dsf" "$ISO_DIR/stereo.dff"

# Task 4: the tags of a SACD ISO track, read from the image's text sectors.
# --gc-sections drops the tag readers and decode_detect_format from
# metadata.c, which are not linked here.
cc -O2 -Wall -Wextra -I. -D_FILE_OFFSET_BITS=64 -DHOST_BUILD=1 \
   -ffunction-sections -fdata-sections -Wl,--gc-sections \
   -o /tmp/metadata_sacd_test tests/metadata_sacd_test.c \
   src/system/library/metadata.c src/system/decode/sacd_iso.c \
   src/system/decode/dst_decoder.c
/tmp/metadata_sacd_test "$ISO_DIR/dsd14.iso"

# Task 5: the library's SACD virtual-path naming and bookkeeping, plus compile
# checks of the library and the queue against the SACD API. The syntax-only pass
# catches a missing include or a changed signature without linking the player.
cc -O2 -Wall -Wextra -Isrc -o /tmp/library_sacd_test \
   tests/library_sacd_test.c src/system/decode/sacd_iso.c \
   src/system/decode/dst_decoder.c
/tmp/library_sacd_test
cc -fsyntax-only -I. -D_FILE_OFFSET_BITS=64 -DHOST_BUILD=1 \
   src/system/library/library.c
cc -fsyntax-only -I. -D_FILE_OFFSET_BITS=64 -DHOST_BUILD=1 \
   src/system/playback/playlist.c
