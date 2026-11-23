/*
 * Tests the SACD side of the DSD output stage (src/system/decode/dsd.c).
 *
 * The fixture is the dsd14 image written by tests/make_sacd_iso.py. Its track
 * 1 is 10 75 fps frames, so 47040 stream bytes per channel, and its bytes
 * follow one known pattern; the DoP words dsd_read hands out have to carry
 * those bytes in order, under a marker that alternates every frame.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "system/decode/dsd.h"

#define BYTES_PER_FRAME 4704  /* one 75 fps SACD frame, per channel */
#define TRACK_FRAMES 10
#define TRACK_BYTES (TRACK_FRAMES * BYTES_PER_FRAME)
#define DOP_FRAMES (TRACK_BYTES / 2)
#define DOP_PER_SACD_FRAME 2352 /* 176400 / 75 */

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

/* The same formula make_sacd_iso.py fills the audio with. */
static unsigned char pattern(int track, int channel, uint64_t index)
{
	return (unsigned char)((track * 17 + channel * 31 + index * 7) & 0xff);
}

/* Checks `frames` DoP words against the stream starting at `byte`. */
static void check_dop(const int32_t *buf, int frames, uint64_t byte,
		      int first_phase)
{
	for (int i = 0; i < frames; i++) {
		for (int c = 0; c < 2; c++) {
			/* Left-justified: the marker and two data bytes sit in
			 * the top 24 bits of the word. */
			uint32_t w = (uint32_t)buf[i * 2 + c] >> 8;
			uint32_t marker = w >> 16;
			unsigned hi = (w >> 8) & 0xff;
			unsigned lo = w & 0xff;
			uint32_t want_marker =
				((i + first_phase) & 1) ? 0xFA : 0x05;
			unsigned want_hi = pattern(1, c, byte + (uint64_t)i * 2);
			unsigned want_lo =
				pattern(1, c, byte + (uint64_t)i * 2 + 1);

			if (marker != want_marker) {
				printf("FAIL: frame %d channel %d marker "
				       "0x%02x, want 0x%02x\n",
				       i, c, marker, want_marker);
				failures++;
				return;
			}
			if (hi != want_hi || lo != want_lo) {
				printf("FAIL: frame %d channel %d bytes "
				       "0x%02x%02x, want 0x%02x%02x\n",
				       i, c, hi, lo, want_hi, want_lo);
				failures++;
				return;
			}
		}
	}
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <dsd14.iso>\n", argv[0]);
		return 2;
	}

	char path[1200];
	snprintf(path, sizeof(path), "%s?sacd=1", argv[1]);

	int32_t buf[1024 * 2];

	/* DoP: the track as it comes off the image. */
	dsd_file_t *d = dsd_open(path);
	check(d != NULL, "open the virtual track");
	if (!d) {
		return 1;
	}
	check(dsd_channels(d) == 2, "channels");
	check(dsd_rate(d) == 2822400u, "DSD rate");
	check(dsd_multiple(d) == 64, "multiple");
	check(dsd_output_rate(d) == 176400, "DoP output rate");
	check(dsd_total_frames(d) == DOP_FRAMES, "DoP total frames");
	check(!dsd_is_pcm(d), "DoP, not PCM");

	uint64_t got = dsd_read(d, 1024, buf);
	check(got == 1024, "read 1024 DoP frames");
	if (got == 1024) {
		check_dop(buf, 1024, 0, 0);
	}

	/* One 75 fps frame of DoP output is 2352 words, 4704 stream bytes. */
	check(dsd_seek(d, DOP_PER_SACD_FRAME), "seek one SACD frame in");
	got = dsd_read(d, 64, buf);
	check(got == 64, "read after the seek");
	if (got == 64) {
		check_dop(buf, 64, BYTES_PER_FRAME, 0);
	}
	dsd_close(d);

	/* A plain .iso path means track 1 of the stereo area, and the track ends
	 * exactly at dsd_total_frames(): one read past it returns nothing. */
	dsd_file_t *plain = dsd_open(argv[1]);
	check(plain != NULL, "open a plain .iso path");
	if (!plain) {
		return 1;
	}
	check(dsd_total_frames(plain) == DOP_FRAMES, "a plain path is track 1");
	got = dsd_read(plain, 1024, buf);
	check(got == 1024, "read from a plain .iso path");
	if (got == 1024) {
		check_dop(buf, 1024, 0, 0);
	}
	uint64_t consumed = got;
	while ((got = dsd_read(plain, 1024, buf)) > 0) {
		consumed += got;
	}
	check(consumed == DOP_FRAMES, "reading to the end takes the whole track");
	check(dsd_read(plain, 1024, buf) == 0, "a read past the end returns 0");
	dsd_close(plain);

	/* PCM: a fresh handle, because dsd_to_pcm does not rewind. */
	dsd_file_t *p = dsd_open(path);
	check(p != NULL, "open a second handle");
	if (!p) {
		return 1;
	}
	check(dsd_to_pcm(p), "switch to PCM");
	check(dsd_is_pcm(p), "PCM mode");
	check(dsd_output_rate(p) == 176400, "PCM output rate");
	check(dsd_total_frames(p) == DOP_FRAMES, "PCM total frames");

	got = dsd_read(p, 1024, buf);
	check(got == 1024, "read 1024 PCM frames");
	int nonzero = 0;
	for (int i = 0; i < 1024 * 2; i++) {
		if (buf[i] != 0) {
			nonzero = 1;
			break;
		}
	}
	check(nonzero, "PCM output is not all zero");
	dsd_close(p);

	if (failures > 0) {
		printf("dsd_sacd_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("dsd_sacd_test: ok\n");
	return 0;
}
