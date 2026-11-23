/*
 * Tests metadata_read_sacd() in src/system/library/metadata.c.
 *
 * The fixture is the dsd14 image written by tests/make_sacd_iso.py: two
 * stereo tracks, a master text sector naming the album and its artist, a
 * Classical genre and the year 2024. Track 2's title and performer come from
 * the area's track text sector.
 */

#include <stdio.h>
#include <string.h>

#include "src/system/library/metadata.h"

static int failures;

static void fail(const char *what, const char *got)
{
	printf("FAIL: %s: \"%s\"\n", what, got ? got : "(null)");
	failures++;
}

static void check_string(const char *what, const char *got, const char *want)
{
	if (!got || strcmp(got, want) != 0) {
		fail(what, got);
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <dsd14.iso>\n", argv[0]);
		return 2;
	}

	char path[1200];
	snprintf(path, sizeof(path), "%s?sacd=2", argv[1]);

	song_metadata_t tags;
	if (!metadata_read_sacd(path, &tags)) {
		printf("FAIL: metadata_read_sacd returned false\n");
		return 1;
	}

	check_string("title", tags.title, "Track 2");
	check_string("artist", tags.artist, "Artist 2");
	check_string("album", tags.album, "SACD Test Album");
	check_string("album_artist", tags.album_artist, "Test Artist");
	check_string("genre", tags.genre, "Classical");
	if (tags.year != 2024) {
		printf("FAIL: year: %d\n", tags.year);
		failures++;
	}
	if (tags.track_number != 2) {
		printf("FAIL: track_number: %d\n", tags.track_number);
		failures++;
	}
	if (!tags.has_tags) {
		printf("FAIL: has_tags is false\n");
		failures++;
	}

	/* A plain image path means its first track. */
	song_metadata_t first;
	if (!metadata_read_sacd(argv[1], &first)) {
		printf("FAIL: a plain .iso path was refused\n");
		failures++;
	} else {
		check_string("plain title", first.title, "Track 1");
		check_string("plain artist", first.artist, "Artist 1");
		if (first.track_number != 1) {
			printf("FAIL: plain track_number: %d\n", first.track_number);
			failures++;
		}
	}

	/* A .iso that is not a SACD image: refused, and the caller's struct is
	 * left zeroed rather than holding whatever was in it. */
	char bad[1200];
	snprintf(bad, sizeof(bad), "%s", argv[1]);
	char *slash = strrchr(bad, '/');
	if (slash) {
		snprintf(slash + 1, sizeof(bad) - (size_t)(slash + 1 - bad), "bad.iso");
	} else {
		snprintf(bad, sizeof(bad), "bad.iso");
	}
	song_metadata_t none;
	memset(&none, 0xA5, sizeof(none));
	if (metadata_read_sacd(bad, &none)) {
		printf("FAIL: a non-SACD image was accepted\n");
		failures++;
	}
	song_metadata_t zeroed;
	memset(&zeroed, 0, sizeof(zeroed));
	if (memcmp(&none, &zeroed, sizeof(none)) != 0) {
		printf("FAIL: a failed read left the struct non-zero\n");
		failures++;
	}

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("metadata_sacd: ok\n");
	return 0;
}
