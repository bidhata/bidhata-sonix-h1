/*
 * Tests the library's SACD virtual paths (Task 5).
 *
 * No SQLite here: the scan itself needs a database and a card, and Task 6
 * smoke-tests that on the player. What can be checked in isolation is the
 * naming a scanned track is given -- sacd_virtual_path(), sacd_split_path(),
 * sacd_order_tracks() -- and the strip-at-either-marker rule library.c uses to
 * turn a virtual path back into its file ("?track=" for a CUE sheet, "?sacd="
 * for an image).
 */

#include <stdio.h>
#include <string.h>

#include "system/decode/sacd_iso.h"

static int failures;

static void check_string(const char *what, const char *got, const char *want) {
	if (strcmp(got, want) != 0) {
		printf("FAIL: %s: \"%s\", wanted \"%s\"\n", what, got, want);
		failures++;
	}
}

static void check_int(const char *what, int got, int want) {
	if (got != want) {
		printf("FAIL: %s: %d, wanted %d\n", what, got, want);
		failures++;
	}
}

// A copy of library.c's virtual_track_len()/virtual_track_base(): the real ones
// are static and the file pulls in SQLite, so the rule is mirrored rather than
// called. Keep the two in step.
static size_t mirror_track_len(const char *path) {
	const char *a = strstr(path, "?track=");
	const char *b = strstr(path, "?sacd=");
	const char *m = a && b ? (a < b ? a : b) : (a ? a : b);
	return m ? (size_t)(m - path) : strlen(path);
}

static void mirror_track_base(const char *path, char *out, size_t size) {
	size_t n = mirror_track_len(path);
	if (n >= size)
		n = size - 1;
	memcpy(out, path, n);
	out[n] = '\0';
}

int main(void) {
	char vpath[1200];

	// The name a scanned SACD track is given, and the way back to its image.
	sacd_virtual_path("/mnt/sd/Music/Album/disc.iso", 2, vpath, sizeof(vpath));
	check_string("virtual path", vpath, "/mnt/sd/Music/Album/disc.iso?sacd=2");

	char iso[1200];
	check_int("split track", sacd_split_path(vpath, iso, sizeof(iso)), 2);
	check_string("split base", iso, "/mnt/sd/Music/Album/disc.iso");

	check_int("plain path is not a track", sacd_split_path(iso, NULL, 0), 0);

	// The image is playable by its extension, with or without a track number.
	if (!sacd_is_iso("/mnt/sd/Music/disc.iso") || !sacd_is_iso(vpath) || !sacd_is_iso("/mnt/sd/Music/DISC.ISO")) {
		printf("FAIL: sacd_is_iso missed an image\n");
		failures++;
	}
	if (sacd_is_iso("/mnt/sd/Music/track.flac")) {
		printf("FAIL: sacd_is_iso took a flac\n");
		failures++;
	}

	// Track two sorts before track ten; the other way round is the other way
	// round. Two different images are not ordered against each other.
	int by_track = 0;
	if (!sacd_order_tracks("/m/disc.iso?sacd=2", "/m/disc.iso?sacd=10", &by_track) || by_track >= 0) {
		printf("FAIL: track 2 did not sort before track 10\n");
		failures++;
	}
	if (!sacd_order_tracks("/m/disc.iso?sacd=10", "/m/disc.iso?sacd=2", &by_track) || by_track <= 0) {
		printf("FAIL: track 10 did not sort after track 2\n");
		failures++;
	}
	if (sacd_order_tracks("/m/one.iso?sacd=2", "/m/two.iso?sacd=2", &by_track)) {
		printf("FAIL: tracks of different images were ordered\n");
		failures++;
	}

	// What library.c's bookkeeping sees as the file behind a virtual path.
	char file[600];
	mirror_track_base("/m/x.cue?track=3", file, sizeof(file));
	check_string("cue base", file, "/m/x.cue");
	mirror_track_base("/m/x.iso?sacd=10", file, sizeof(file));
	check_string("sacd base", file, "/m/x.iso");
	mirror_track_base("/m/x.flac", file, sizeof(file));
	check_string("plain base", file, "/m/x.flac");
	// Whichever marker comes first, so a path carrying both cannot overrun.
	mirror_track_base("/m/x.cue?track=3?sacd=2", file, sizeof(file));
	check_string("first marker", file, "/m/x.cue");
	char short_out[5];
	mirror_track_base("/m/x.iso?sacd=2", short_out, sizeof(short_out));
	check_string("truncated", short_out, "/m/x");

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("library_sacd: ok\n");
	return 0;
}
