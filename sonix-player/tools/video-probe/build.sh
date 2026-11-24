#!/usr/bin/env bash
# Cross-compile fbprobe for the device and run the same glibc-2.22 ABI gate the
# main build uses. Produces tools/video-probe/fbprobe (a MIPS binary).
#
# Run from the repo root (sonix-player/) or anywhere; paths are resolved from
# this script's location.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"   # .../sonix-player/sonix-player

TOOLCHAIN="$root/rockbox-toolchain"
CC="$TOOLCHAIN/bin/mipsel-rockbox-linux-gnu-gcc"
READELF="$TOOLCHAIN/bin/mipsel-rockbox-linux-gnu-readelf"
STRIP="$TOOLCHAIN/bin/mipsel-rockbox-linux-gnu-strip"

if [ ! -x "$CC" ]; then
	echo "error: cross compiler not found at $CC" >&2
	echo "       build it first:  ./rockboxdev/rockboxdev.sh" >&2
	exit 1
fi

out="$here/fbprobe"

# Same core target flags as the Makefile's TARGET_CFLAGS: mips32r2, static.
# No glibc_compat.h needed here -- fbprobe uses only plain libc (open, mmap,
# ioctl, clock_gettime, printf), none of which is a >2.22 symbol -- but the ABI
# gate below proves that rather than assuming it.
"$CC" -O2 -march=mips32r2 -mno-madd4 -mplt -Wall -Wextra \
	-static \
	-o "$out" "$here/fbprobe.c"

echo "built: $out"

# ---- the glibc-2.22 ABI gate, same logic as the Makefile's check-abi ----
# A fully static binary has no versioned dynamic symbols at all, so this is
# belt-and-braces; it still runs so the probe is held to the same bar as the
# player.
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
echo ""
echo "Next: push to the R1 and run it:"
echo "    adb push $out /tmp/fbprobe && adb shell 'chmod +x /tmp/fbprobe && /tmp/fbprobe 3'"
echo "  (or copy to the SD card and run from a shell on the device)"
