/*
 * Regression test for the plain DSD containers of the output stage
 * (src/system/decode/dsd.c): a .dsf and a .dff, not an SACD image.
 *
 * The fixtures come from tests/make_dsd_fixtures.py: 4096 bytes per channel of
 * one known pattern, so every byte the DoP path hands out can be checked. The
 * .dsf stores its bits least significant first, so its bytes come out of the
 * decoder bit-reversed; the .dff is already most significant bit first.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "system/decode/dsd.h"

#define CHANNELS 2
#define BYTES_PER_CHANNEL 4096
#define DOP_FRAMES (BYTES_PER_CHANNEL / 2) /* two stream bytes a DoP frame */
#define PCM_FRAMES (BYTES_PER_CHANNEL / 2) /* step is two bytes at DSD64 */
#define DSD64_RATE 2822400u

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

/* The formula tests/make_dsd_fixtures.py fills the audio with. */
static unsigned char pattern(int channel, uint64_t index)
{
	return (unsigned char)((channel * 31 + index * 7 + 17) & 0xff);
}

static unsigned char reverse_bits(unsigned char v)
{
	v = (unsigned char)(((v & 0xF0) >> 4) | ((v & 0x0F) << 4));
	v = (unsigned char)(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
	v = (unsigned char)(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
	return v;
}

/* Checks `frames` DoP words against the stream starting at `byte`. */
static void check_dop(const int32_t *buf, int frames, uint64_t byte,
		      int first_phase, int lsb_first)
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
			unsigned want_hi = pattern(c, byte + (uint64_t)i * 2);
			unsigned want_lo = pattern(c, byte + (uint64_t)i * 2 + 1);

			if (lsb_first) {
				want_hi = reverse_bits((unsigned char)want_hi);
				want_lo = reverse_bits((unsigned char)want_lo);
			}

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

static void test_container(const char *path, int lsb_first, const char *label)
{
	int32_t buf[256 * 2];
	char what[128];

	snprintf(what, sizeof(what), "%s: open", label);
	dsd_file_t *d = dsd_open(path);
	check(d != NULL, what);
	if (!d) {
		return;
	}
	snprintf(what, sizeof(what), "%s: channels", label);
	check(dsd_channels(d) == CHANNELS, what);
	snprintf(what, sizeof(what), "%s: DSD rate", label);
	check(dsd_rate(d) == DSD64_RATE, what);
	snprintf(what, sizeof(what), "%s: multiple", label);
	check(dsd_multiple(d) == 64, what);
	snprintf(what, sizeof(what), "%s: DoP output rate", label);
	check(dsd_output_rate(d) == 176400, what);
	snprintf(what, sizeof(what), "%s: DoP total frames", label);
	check(dsd_total_frames(d) == DOP_FRAMES, what);
	snprintf(what, sizeof(what), "%s: DoP, not PCM", label);
	check(!dsd_is_pcm(d), what);

	snprintf(what, sizeof(what), "%s: read 256 DoP frames", label);
	uint64_t got = dsd_read(d, 256, buf);
	check(got == 256, what);
	if (got == 256) {
		check_dop(buf, 256, 0, 0, lsb_first);
	}

	/* Seek to DoP frame 1000: stream byte 2000, not a block boundary. */
	snprintf(what, sizeof(what), "%s: seek", label);
	check(dsd_seek(d, 1000), what);
	snprintf(what, sizeof(what), "%s: read after the seek", label);
	got = dsd_read(d, 256, buf);
	check(got == 256, what);
	if (got == 256) {
		check_dop(buf, 256, 2000, 0, lsb_first);
	}
	dsd_close(d);

	/* PCM: a fresh handle, because dsd_to_pcm does not rewind. */
	snprintf(what, sizeof(what), "%s: open a second handle", label);
	dsd_file_t *p = dsd_open(path);
	check(p != NULL, what);
	if (!p) {
		return;
	}
	snprintf(what, sizeof(what), "%s: switch to PCM", label);
	check(dsd_to_pcm(p), what);
	snprintf(what, sizeof(what), "%s: PCM mode", label);
	check(dsd_is_pcm(p), what);
	snprintf(what, sizeof(what), "%s: PCM output rate", label);
	check(dsd_output_rate(p) == 176400, what);
	snprintf(what, sizeof(what), "%s: PCM total frames", label);
	check(dsd_total_frames(p) == PCM_FRAMES, what);

	snprintf(what, sizeof(what), "%s: read 256 PCM frames", label);
	got = dsd_read(p, 256, buf);
	check(got == 256, what);
	int nonzero = 0;
	for (int i = 0; i < 256 * 2; i++) {
		if (buf[i] != 0) {
			nonzero = 1;
			break;
		}
	}
	snprintf(what, sizeof(what), "%s: PCM output is not all zero", label);
	check(nonzero, what);
	dsd_close(p);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <stereo.dsf> <stereo.dff>\n", argv[0]);
		return 2;
	}

	test_container(argv[1], 1, "dsf");
	test_container(argv[2], 0, "dff");

	if (failures > 0) {
		printf("dsd_regression_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("dsd_regression_test: ok\n");
	return 0;
}
