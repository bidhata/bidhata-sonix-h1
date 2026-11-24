#!/bin/sh
# The card checker (src/system/device/fatcheck.c) against FAT32 images made
# here: clean ones, ones with lost clusters to free, and ones damaged in ways
# it must report and not touch. What it frees is checked twice over: by
# fsck.fat finding nothing left to complain about, and by every file reading
# back the same.
#
# Needs mkfs.fat and fsck.fat (dosfstools), mtools and python3. Run from
# anywhere; it works in a directory of its own under ${TMPDIR:-/tmp}.
set -e
cd "$(dirname "$0")/.."

WORK="$(mktemp -d "${TMPDIR:-/tmp}/fatcheck-test.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

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

# A tree with what trips a reader of folders: long names, empty
# files, sizes on either side of a cluster, a hidden file, folders in folders,
# and one folder with enough entries to need several clusters.
TREE="$WORK/tree"
mkdir -p "$TREE/Music/Artist with a long name/Album (2024) [FLAC]" "$TREE/a/b/c/d" "$TREE/many" "$TREE/empty folder"
head -c 3000000 /dev/urandom > "$TREE/big.bin"
head -c 70000 /dev/urandom > "$TREE/other.bin"
: > "$TREE/zero.txt"
for n in 1 511 512 513 4095 4096 4097 100000; do
	head -c "$n" /dev/urandom > "$TREE/a/b/size-$n.bin"
done
head -c 200000 /dev/urandom > "$TREE/Music/Artist with a long name/Album (2024) [FLAC]/01 - A track with a long title.flac"
printf 'x' > "$TREE/a/b/c/d/deep.txt"
i=0
while [ $i -lt 300 ]; do
	printf '%d' $i > "$TREE/many/file number $i with a long name.txt"
	i=$((i + 1))
done
FILES=$(find "$TREE" -type f | wc -l)
FOLDERS=$(find "$TREE" -mindepth 1 -type d | wc -l)

make_image() { # make_image PATH MEGABYTES SECTORS_PER_CLUSTER
	rm -f "$1"
	truncate -s "${2}M" "$1"
	mkfs.fat -F 32 -s "$3" -n TEST "$1" > /dev/null
	for entry in "$TREE"/*; do
		mcopy -i "$1" -s -Q "$entry" ::
	done
	mattrib -i "$1" +h ::zero.txt
}

same_files() { # same_files IMAGE: every file reads back as it was put in
	rm -rf "$WORK/out"
	mkdir "$WORK/out"
	mcopy -i "$1" -s -n -Q ::* "$WORK/out/" > /dev/null
	diff -r "$TREE" "$WORK/out" > /dev/null || fail "files differ after the repair of $1"
}

for geometry in "64 1" "300 8" "2200 64"; do
	set -- $geometry
	IMG="$WORK/fat-$1-$2.img"
	echo "--- $1 MB, $2 sectors to a cluster"
	make_image "$IMG" "$1" "$2"
	cp --sparse=always "$IMG" "$WORK/clean.img"

	expect "a clean card" "$($RUN "$IMG" check)" "clean files=$FILES folders=$FOLDERS lost=0 *"
	expect "a repair with nothing to do" "$($RUN "$IMG" repair "$FILES" "$FOLDERS")" "clean * repaired=0 *"
	cmp -s "$IMG" "$WORK/clean.img" || fail "a clean card was written to"

	python3 tests/fatcheck_images.py "$IMG" lost
	cp --sparse=always "$IMG" "$WORK/lost.img"
	expect "lost clusters found" "$($RUN "$IMG" check)" "lost files=$FILES folders=$FOLDERS lost=6 * repaired=0 *"
	cmp -s "$IMG" "$WORK/lost.img" || fail "a check wrote to the card"

	# A count that does not agree: nothing may be freed.
	expect "a repair refused on a different count" "$($RUN "$IMG" repair $((FILES + 1)) "$FOLDERS")" "mismatch * repaired=0 *"
	cmp -s "$IMG" "$WORK/lost.img" || fail "a refused repair wrote to the card"

	expect "lost clusters freed" "$($RUN "$IMG" repair "$FILES" "$FOLDERS")" "lost files=$FILES folders=$FOLDERS lost=6 * repaired=1 *"
	expect "clean after the repair" "$($RUN "$IMG" check)" "clean files=$FILES folders=$FOLDERS lost=0 *"
	fsck.fat -n "$IMG" > "$WORK/fsck.txt" 2>&1 || { cat "$WORK/fsck.txt"; fail "fsck.fat finds fault after the repair"; }
	same_files "$IMG"
	# Freed and nothing else: but for the free count, the card is the one it
	# was before the clusters were lost.
	[ "$(cmp -l "$IMG" "$WORK/clean.img" | wc -l)" -le 8 ] || fail "the repair changed more than the tables' lost entries"

	for damage in tables crosslink cut short; do
		cp --sparse=always "$WORK/lost.img" "$IMG"
		python3 tests/fatcheck_images.py "$IMG" "$damage"
		cp --sparse=always "$IMG" "$WORK/damaged.img"
		expect "damage ($damage) reported" "$($RUN "$IMG" check)" "damaged *"
		expect "damage ($damage) not repaired" "$($RUN "$IMG" repair)" "damaged * repaired=0 *"
		cmp -s "$IMG" "$WORK/damaged.img" || fail "a damaged card ($damage) was written to"
	done
	rm -f "$IMG" "$WORK/clean.img" "$WORK/lost.img" "$WORK/damaged.img"
done

# Not FAT32: left alone.
truncate -s 16M "$WORK/fat16.img"
mkfs.fat -F 16 "$WORK/fat16.img" > /dev/null
expect "a FAT16 card" "$($RUN "$WORK/fat16.img" repair)" "unsupported *"
head -c 1000000 /dev/urandom > "$WORK/noise.img"
expect "not a filesystem" "$($RUN "$WORK/noise.img" repair)" "unsupported *"

echo "fatcheck: all passed"
