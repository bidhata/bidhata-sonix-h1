#!/bin/sh
# The card checker's exFAT half (src/system/device/fatcheck.c) against images
# made here, in the manner of tests/fatcheck_test.sh: clean ones, ones with
# lost clusters to free, and ones damaged in ways it must report and not
# touch. What it frees is checked by fsck.exfat finding nothing left to
# complain about, and by every file reading back the same.
#
# There is no tool that writes files into an exFAT image without mounting it,
# so this needs root and a kernel with the exFAT driver, besides exfatprogs
# and python3. Where it cannot mount it says so and passes.
set -e
cd "$(dirname "$0")/.."

WORK="$(mktemp -d "${TMPDIR:-/tmp}/exfatcheck-test.XXXXXX")"
MNT="$WORK/mnt"
mkdir "$MNT"
cleanup() {
	umount "$MNT" 2> /dev/null || true
	rm -rf "$WORK"
}
trap cleanup EXIT

if [ "$(id -u)" -ne 0 ] || ! command -v mkfs.exfat > /dev/null || ! command -v fsck.exfat > /dev/null; then
	echo "exfatcheck: skipped (needs root and exfatprogs)"
	exit 0
fi
truncate -s 64M "$WORK/probe.img"
mkfs.exfat -q "$WORK/probe.img" > /dev/null 2>&1
if ! mount -o loop -t exfat "$WORK/probe.img" "$MNT" 2> /dev/null; then
	echo "exfatcheck: skipped (this kernel does not mount exFAT)"
	exit 0
fi
umount "$MNT"

cc -O2 -Wall -Wextra -D_FILE_OFFSET_BITS=64 -Isrc -o "$WORK/fatcheck_test" \
   tests/fatcheck_test.c src/system/device/fatcheck.c
RUN="$WORK/fatcheck_test"

fail() { echo "FAIL: $*" >&2; exit 1; }
expect() { # expect LABEL OUTPUT PATTERN
	case "$2" in
	$3) echo "ok: $1 -- $2" ;;
	*) fail "$1: got '$2', wanted '$3'" ;;
	esac
}

TREE="$WORK/tree"
mkdir -p "$TREE/Music/Artist with a long name/Album (2024) [FLAC]" "$TREE/a/b/c/d" "$TREE/many" "$TREE/empty folder"
head -c 3000000 /dev/urandom > "$TREE/big.bin"
: > "$TREE/zero.txt"
for n in 1 511 512 513 4095 4096 4097 100000; do
	head -c "$n" /dev/urandom > "$TREE/a/b/size-$n.bin"
done
head -c 200000 /dev/urandom > "$TREE/Music/Artist with a long name/Album (2024) [FLAC]/01 - A track with a long title.flac"
printf 'x' > "$TREE/a/b/c/d/deep.txt"
# A name outside ASCII, in two scripts: on the card it is stored as UTF-16.
printf 'x' > "$TREE/Music/$(printf 'caf\303\251 \340\246\254\340\246\277\340\246\247\340\246\276\340\246\244\340\246\276').txt"
i=0
while [ $i -lt 300 ]; do
	printf '%d' $i > "$TREE/many/file number $i with a long name.txt"
	i=$((i + 1))
done
# Two files that grow by turns, each taking the cluster after the other's:
# both end up in pieces, which is the one case where exFAT keeps a chain.
head -c 4000000 /dev/urandom > "$WORK/a.src"
head -c 4000000 /dev/urandom > "$WORK/b.src"

make_image() { # make_image PATH MEGABYTES CLUSTER_SIZE
	rm -f "$1"
	truncate -s "${2}M" "$1"
	mkfs.exfat -q -c "$3" "$1" > /dev/null
	mount -o loop,iocharset=utf8 -t exfat "$1" "$MNT"
	cp -r "$TREE"/. "$MNT"/
	i=0
	while [ $i -lt 20 ]; do
		dd if="$WORK/a.src" of="$MNT/frag-a.bin" bs=200000 skip=$i seek=$i count=1 conv=notrunc,fsync 2> /dev/null
		dd if="$WORK/b.src" of="$MNT/frag-b.bin" bs=200000 skip=$i seek=$i count=1 conv=notrunc,fsync 2> /dev/null
		i=$((i + 1))
	done
	FILES=$(find "$MNT" -type f | wc -l)
	FOLDERS=$(find "$MNT" -mindepth 1 -type d | wc -l)
	umount "$MNT"
}

same_files() { # same_files IMAGE: every file reads back as it was put in
	mount -o loop,ro,iocharset=utf8 -t exfat "$1" "$MNT"
	if ! diff -r "$TREE" "$MNT" -x frag-a.bin -x frag-b.bin > /dev/null || ! cmp -s "$WORK/a.src" "$MNT/frag-a.bin" || ! cmp -s "$WORK/b.src" "$MNT/frag-b.bin"; then
		umount "$MNT"
		fail "files differ after the repair of $1"
	fi
	umount "$MNT"
}

for geometry in "300 4K" "600 32K" "4000 128K"; do
	set -- $geometry
	IMG="$WORK/exfat-$1-$2.img"
	echo "--- $1 MB, clusters of $2"
	make_image "$IMG" "$1" "$2"
	[ "$(python3 tests/exfatcheck_images.py "$IMG" info)" = "pieces" ] || fail "the test files did not come out in pieces; the chain is not being tested"
	cp --sparse=always "$IMG" "$WORK/clean.img"

	expect "a clean card" "$($RUN "$IMG" check)" "clean files=$FILES folders=$FOLDERS lost=0 *"
	expect "a repair with nothing to do" "$($RUN "$IMG" repair "$FILES" "$FOLDERS")" "clean * repaired=0 *"
	cmp -s "$IMG" "$WORK/clean.img" || fail "a clean card was written to"

	python3 tests/exfatcheck_images.py "$IMG" lost
	cp --sparse=always "$IMG" "$WORK/lost.img"
	expect "lost clusters found" "$($RUN "$IMG" check)" "lost files=$FILES folders=$FOLDERS lost=6 * repaired=0 *"
	cmp -s "$IMG" "$WORK/lost.img" || fail "a check wrote to the card"

	expect "a repair refused on a different count" "$($RUN "$IMG" repair $((FILES + 1)) "$FOLDERS")" "mismatch * repaired=0 *"
	cmp -s "$IMG" "$WORK/lost.img" || fail "a refused repair wrote to the card"

	expect "lost clusters freed" "$($RUN "$IMG" repair "$FILES" "$FOLDERS")" "lost files=$FILES folders=$FOLDERS lost=6 * repaired=1 *"
	expect "clean after the repair" "$($RUN "$IMG" check)" "clean files=$FILES folders=$FOLDERS lost=0 *"
	# Freed and nothing else: the card is byte for byte the one it was before
	# the clusters were lost.
	cmp -s "$IMG" "$WORK/clean.img" || fail "the repair did not give back exactly the clean card"
	fsck.exfat -n "$IMG" > "$WORK/fsck.txt" 2>&1 || { cat "$WORK/fsck.txt"; fail "fsck.exfat finds fault after the repair"; }
	same_files "$IMG"

	for damage in unmarked cut crosslink; do
		cp --sparse=always "$WORK/lost.img" "$IMG"
		python3 tests/exfatcheck_images.py "$IMG" "$damage"
		cp --sparse=always "$IMG" "$WORK/damaged.img"
		expect "damage ($damage) reported" "$($RUN "$IMG" check)" "damaged *"
		expect "damage ($damage) not repaired" "$($RUN "$IMG" repair)" "damaged * repaired=0 *"
		cmp -s "$IMG" "$WORK/damaged.img" || fail "a damaged card ($damage) was written to"
	done
	rm -f "$IMG" "$WORK/clean.img" "$WORK/lost.img" "$WORK/damaged.img"
done

echo "exfatcheck: all passed"
