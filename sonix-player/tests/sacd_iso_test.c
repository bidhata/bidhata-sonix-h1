/*
 * Tests for the SACD ISO reader (src/system/decode/sacd_iso.c).
 *
 * The images are synthetic -- tests/make_sacd_iso.py writes them -- and their
 * audio follows one known pattern, so every byte read can be checked against
 * the same formula the generator used.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "system/decode/sacd_iso.h"

#define BYTES_PER_FRAME 4704
#define CHUNK 8192

static int failures;

static void fail(const char *what)
{
	printf("FAIL: %s\n", what);
	failures++;
}

static void check(int condition, const char *what)
{
	if (!condition)
		fail(what);
}

/* The pattern every fixture is filled with: channel c, track-relative byte i
 * of a track numbered from 1. */
static unsigned char pattern(int track, int channel, uint64_t index)
{
	return (unsigned char)((track * 17 + channel * 31 + index * 7) & 0xff);
}

static void check_bytes(unsigned char *const *chan, int track,
			uint64_t start, uint64_t count)
{
	for (uint64_t i = 0; i < count; i++) {
		for (int c = 0; c < 2; c++) {
			if (chan[c][i] != pattern(track, c, start + i)) {
				printf("FAIL: track %d byte %llu channel %d: "
				       "got 0x%02x\n",
				       track, (unsigned long long)(start + i), c,
				       chan[c][i]);
				failures++;
				return;
			}
		}
	}
}

static void test_track(sacd_iso_t *iso, int track, uint32_t frames)
{
	unsigned char *chan[2];
	uint64_t total = (uint64_t)frames * BYTES_PER_FRAME;

	chan[0] = malloc(CHUNK);
	chan[1] = malloc(CHUNK);
	if (!chan[0] || !chan[1]) {
		fail("allocation");
		free(chan[0]);
		free(chan[1]);
		return;
	}

	check(sacd_iso_select(iso, 0, track), "select");
	check(sacd_iso_channels(iso) == 2, "selected channels");
	check(sacd_iso_multiple(iso) == 64, "multiple");
	check(sacd_iso_rate(iso) == 2822400u, "rate");
	check(sacd_iso_track_bytes(iso) == total, "track bytes");

	/* Read the whole track in chunks that do not line up with frames. */
	uint64_t done = 0;
	while (done < total) {
		uint64_t want = total - done < CHUNK ? total - done : CHUNK;
		uint64_t got = sacd_iso_read(iso, chan, want);
		if (got != want) {
			printf("FAIL: track %d read at %llu returned %llu\n", track,
			       (unsigned long long)done, (unsigned long long)got);
			failures++;
			break;
		}
		check_bytes(chan, track, done, got);
		done += got;
	}
	check(sacd_iso_read(iso, chan, 16) == 0, "read past the end");

	/* Just past the middle: never on a frame boundary. */
	uint64_t half = total / 2 + 1;
	check(sacd_iso_seek(iso, half), "seek into a frame");
	uint64_t got = sacd_iso_read(iso, chan, 4096);
	check(got == 4096, "read after a middle seek");
	if (got > 0) {
		check_bytes(chan, track, half, got);
	}

	check(sacd_iso_seek(iso, 0), "seek to the start");
	got = sacd_iso_read(iso, chan, 1024);
	check(got == 1024, "read after seeking to the start");
	if (got > 0) {
		check_bytes(chan, track, 0, got);
	}

	free(chan[0]);
	free(chan[1]);
}

static void test_deep_seek(sacd_iso_t *iso, int track, uint32_t frames)
{
	uint64_t total = (uint64_t)frames * BYTES_PER_FRAME;
	uint64_t start = total - 2 * BYTES_PER_FRAME;
	unsigned char *chan[2];

	chan[0] = malloc(2 * BYTES_PER_FRAME);
	chan[1] = malloc(2 * BYTES_PER_FRAME);
	if (!chan[0] || !chan[1]) {
		fail("allocation");
		free(chan[0]);
		free(chan[1]);
		return;
	}

	/* Frame 13 of the DST fixture is reached through access-list entry 1. */
	check(sacd_iso_select(iso, 0, track), "deep seek select");
	check(sacd_iso_seek(iso, start), "deep seek");
	uint64_t got = sacd_iso_read(iso, chan, 2 * BYTES_PER_FRAME);
	check(got == 2 * BYTES_PER_FRAME, "deep seek read");
	if (got > 0) {
		check_bytes(chan, track, start, got);
	}

	free(chan[0]);
	free(chan[1]);
}

static void test_paths(const char *path)
{
	char virtual_two[1200];
	char virtual_ten[1200];
	char base[1200];
	char other_base[1200];
	int order = 0;

	sacd_virtual_path(path, 2, virtual_two, sizeof(virtual_two));
	sacd_virtual_path(path, 10, virtual_ten, sizeof(virtual_ten));

	size_t len = strlen(virtual_two);
	check(len > 7 && strcmp(virtual_two + len - 7, "?sacd=2") == 0,
	      "virtual path suffix");

	check(sacd_split_path(virtual_two, base, sizeof(base)) == 2,
	      "split virtual path");
	check(strcmp(base, path) == 0, "split copies the base path");
	check(sacd_is_iso(virtual_two), "virtual path is an iso");

	/* Opening a virtual path must land on the image itself. */
	sacd_iso_t *virtual_iso = sacd_iso_open(virtual_two);
	check(virtual_iso != NULL, "open a virtual path");
	sacd_iso_close(virtual_iso);

	check(sacd_order_tracks(virtual_two, virtual_ten, &order),
	      "order two tracks of one image");
	check(order < 0, "track 2 sorts before track 10");

	/* CUE's ?track= is not ours, and neither is a different image. */
	check(sacd_split_path("album.cue?track=2", other_base,
			      sizeof(other_base)) == 0,
	      "cue suffix is not an sacd suffix");
	check(!sacd_order_tracks(virtual_two, "other.iso?sacd=10", &order),
	      "tracks of different images do not order");
	check(!sacd_is_iso("album.iso?sacd=999"), "out-of-range track is not an iso");
}

static void test_bad(const char *path)
{
	sacd_iso_t *iso = sacd_iso_open(path);

	check(iso == NULL, "a file of zero sectors is not an SACD");
	sacd_iso_close(iso);
}

static void test_image(const char *path, int track_count, const uint32_t *frames,
		       int deep_seek)
{
	sacd_iso_t *iso = sacd_iso_open(path);

	check(iso != NULL, "open");
	if (!iso)
		return;

	check(sacd_iso_area_count(iso) == 1, "area count");
	check(sacd_iso_stereo_area(iso) == 0, "stereo area");

	const sacd_area_t *area = sacd_iso_area(iso, 0);
	check(area != NULL, "area 0");
	if (!area) {
		sacd_iso_close(iso);
		return;
	}
	check(area->channels == 2, "area channels");
	check(area->track_count == track_count, "track count");
	check(strcmp(sacd_iso_album_title(iso), "SACD Test Album") == 0,
	      "album title");
	check(strcmp(sacd_iso_album_artist(iso), "Test Artist") == 0,
	      "album artist");
	check(sacd_iso_year(iso) == 2024, "year");
	check(strcmp(sacd_iso_genre(iso), "Classical") == 0, "genre");

	for (int i = 0; i < track_count; i++) {
		char title[32];
		char performer[32];

		snprintf(title, sizeof(title), "Track %d", i + 1);
		snprintf(performer, sizeof(performer), "Artist %d", i + 1);
		check(area->tracks[i].number == i + 1, "track number");
		check(strcmp(area->tracks[i].title, title) == 0, "track title");
		check(strcmp(area->tracks[i].performer, performer) == 0,
		      "track performer");
		check(area->tracks[i].frames == frames[i], "track frames");
		check(area->tracks[i].start_lsn >= area->track_start_lsn &&
		      area->tracks[i].start_lsn <= area->track_end_lsn,
		      "track start inside the area");
		test_track(iso, i + 1, frames[i]);
	}

	if (deep_seek) {
		test_deep_seek(iso, 1, frames[0]);
	}

	test_paths(path);
	sacd_iso_close(iso);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr,
			"usage: %s <image.iso> <dsd14|dsd16|dst|dst_shared|bad>\n",
			argv[0]);
		return 2;
	}

	const char *path = argv[1];
	const char *mode = argv[2];

	if (strcmp(mode, "bad") == 0) {
		test_bad(path);
	} else if (strcmp(mode, "dsd14") == 0) {
		static const uint32_t frames[] = {10, 5};
		test_image(path, 2, frames, 0);
	} else if (strcmp(mode, "dsd16") == 0) {
		static const uint32_t frames[] = {9};
		test_image(path, 1, frames, 0);
	} else if (strcmp(mode, "dst") == 0) {
		static const uint32_t frames[] = {15};
		test_image(path, 1, frames, 1);
	} else if (strcmp(mode, "dst_shared") == 0) {
		static const uint32_t frames[] = {2};
		test_image(path, 1, frames, 0);
	} else {
		fprintf(stderr, "unknown mode: %s\n", mode);
		return 2;
	}

	if (failures > 0) {
		printf("sacd_iso_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("sacd_iso_test: ok\n");
	return 0;
}
