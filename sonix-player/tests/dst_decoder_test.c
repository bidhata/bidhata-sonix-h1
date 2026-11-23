/*
 * Tests for the vendored DST decoder (src/system/decode/dst_decoder.c).
 *
 * Only the paths that need no arithmetic-coded data are exercised here: an
 * uncompressed frame, the malformed-table refusal and the argument checks.
 *
 * The compressed path is NOT covered by these tests, and the SACD reader tests
 * do not cover it either: they decode uncompressed frames that were put in a
 * DST container (see make_sacd_iso.py). Arithmetic-coded DST is only proved by
 * a real DST frame from a real image, which is the field/release gate.
 */

#include <stdio.h>
#include <string.h>

#include "system/decode/dst_decoder.h"

#define CHANNELS 2
#define SAMPLES_PER_FRAME 4704 /* 588 * 2822400 / 44100 / 8 */
#define FRAME_BYTES (SAMPLES_PER_FRAME * CHANNELS)

static int failures;
static unsigned char output[FRAME_BYTES];

static void fail(const char *what)
{
	printf("FAIL: %s\n", what);
	failures++;
}

static void check(int cond, const char *what)
{
	if (!cond)
		fail(what);
}

static void fill_pattern(unsigned char *buf, int len)
{
	int i;

	for (i = 0; i < len; i++)
		buf[i] = (unsigned char)(i * 37 + 11);
}

/* An uncompressed DST frame: a zero bit, then five bits of zero (same
 * segmentation, same map, ...), then the raw DSD bytes. 9409 bytes total. */
static int test_uncompressed(void)
{
	unsigned char frame[1 + FRAME_BYTES];
	dst_decoder_t *dec = NULL;
	int out_len = -1;
	int ret;

	frame[0] = 0x00;
	fill_pattern(frame + 1, FRAME_BYTES);

	memset(output, 0, sizeof(output));

	ret = dst_decoder_init(&dec, CHANNELS, 2822400);
	check(ret == 0 && dec != NULL, "init for the uncompressed frame");
	if (ret != 0 || !dec)
		return 1;

	ret = dst_decoder_decode(dec, frame, (int)sizeof(frame), output, &out_len);
	check(ret == 0, "uncompressed decode returns 0");
	check(out_len == FRAME_BYTES, "uncompressed decode produces 9408 bytes");
	if (ret == 0 && out_len == FRAME_BYTES)
		check(memcmp(output, frame + 1, FRAME_BYTES) == 0,
		      "uncompressed decode copies the frame through");

	dst_decoder_close(dec);
	return 0;
}

/* The frame may carry more bytes than the decoder produces: the copy has to
 * stop at 4704 * channels, and the bytes after it must stay untouched. */
static void test_uncompressed_clamp(void)
{
	unsigned char frame[1 + FRAME_BYTES + 8];
	unsigned char out[FRAME_BYTES + 8];
	dst_decoder_t *dec = NULL;
	int out_len = -1;
	int ret;

	frame[0] = 0x00;
	fill_pattern(frame + 1, FRAME_BYTES + 8);
	memset(out, 0x5A, sizeof(out));

	ret = dst_decoder_init(&dec, CHANNELS, 2822400);
	check(ret == 0 && dec != NULL, "init for the oversized frame");
	if (ret != 0 || !dec)
		return;

	ret = dst_decoder_decode(dec, frame, (int)sizeof(frame), out, &out_len);
	check(ret == 0, "oversized uncompressed decode returns 0");
	check(out_len == FRAME_BYTES, "oversized frame still produces 9408 bytes");
	if (ret == 0 && out_len == FRAME_BYTES)
		check(memcmp(out, frame + 1, FRAME_BYTES) == 0,
		      "oversized frame copies exactly 9408 bytes");

	for (ret = FRAME_BYTES; ret < (int)sizeof(out); ret++) {
		if (out[ret] != 0x5A) {
			fail("oversized frame wrote past 4704 * channels bytes");
			break;
		}
	}

	dst_decoder_close(dec);
}

/* A compressed frame whose filter table uses reserved method 3: read_table
 * fails and the decoder must abort instead of decoding with half-filled
 * tables. Bits: compressed=1, segmentation=111, same-map=1, per-channel map
 * giving two elements, half-probs=00, length[0]=1, coded=1, method=3. */
static void test_malformed_table(void)
{
	static const unsigned char frame[] = { 0xFA, 0x00, 0xE0 };
	dst_decoder_t *dec = NULL;
	int out_len = -1;
	int ret;

	ret = dst_decoder_init(&dec, CHANNELS, 2822400);
	check(ret == 0 && dec != NULL, "init for the malformed frame");
	if (ret != 0 || !dec)
		return;

	ret = dst_decoder_decode(dec, frame, (int)sizeof(frame), output, &out_len);
	check(ret < 0, "reserved prediction method is rejected");

	dst_decoder_close(dec);
}

static void test_bad_input(void)
{
	unsigned char one = 0x00;
	dst_decoder_t *dec = NULL;
	int out_len = -1;
	int ret;

	if (dst_decoder_init(&dec, CHANNELS, 2822400) != 0 || !dec) {
		fail("init for the bad-input test");
		return;
	}

	out_len = -1;
	ret = dst_decoder_decode(dec, &one, 0, output, &out_len);
	check(ret < 0, "empty frame is rejected");

	out_len = -1;
	ret = dst_decoder_decode(dec, &one, 1, output, &out_len);
	check(ret < 0, "one-byte frame is rejected");

	dst_decoder_close(dec);
}

static void test_init_close(void)
{
	dst_decoder_t *dec = NULL;

	check(dst_decoder_init(&dec, CHANNELS, 2822400) == 0 && dec != NULL,
	      "init with 2 channels at 2822400");
	check(dst_decoder_close(dec) == 0, "close returns 0");
	check(dst_decoder_close(NULL) == 0, "close(NULL) returns 0");

	dec = NULL;
	check(dst_decoder_init(&dec, 0, 2822400) < 0 && dec == NULL,
	      "init rejects 0 channels");
	dec = NULL;
	check(dst_decoder_init(&dec, -1, 2822400) < 0 && dec == NULL,
	      "init rejects negative channels");
	dec = NULL;
	check(dst_decoder_init(&dec, 7, 2822400) < 0 && dec == NULL,
	      "init rejects more than 6 channels");
}

int main(void)
{
	test_uncompressed();
	test_uncompressed_clamp();
	test_malformed_table();
	test_bad_input();
	test_init_close();

	if (failures) {
		printf("dst_decoder_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("dst_decoder_test: ok\n");
	return 0;
}
