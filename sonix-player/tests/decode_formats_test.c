// Decodes one file through the player's own decoder layer and checks that
// what comes out is the test tone that went in: the right rate, both
// channels, the right length, and the right note in each ear.
//
//     decode_formats_test FILE RATE SECONDS LEFT_HZ RIGHT_HZ
//
// The files are made by tests/decode_formats_test.sh. A decoder that plays
// at the wrong speed, swaps the channels, drops the end of a file or returns
// silence fails here, which is what a listener would notice first.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "system/decode/decode.h"

// The note in one channel, from how often the wave crosses zero going up.
// Good to a hertz or so over twenty seconds, which is all this needs. The
// first and last tenth of a second are left out: a lossy codec is allowed its
// run-in there.
static double pitch_of(const short *pcm, uint64_t frames, int channels, int channel, int rate) {
	uint64_t from = (uint64_t)rate / 10, to = frames - (uint64_t)rate / 10;
	uint64_t crossings = 0;
	for (uint64_t i = from + 1; i < to; i++) {
		short before = pcm[(i - 1) * channels + channel], now = pcm[i * channels + channel];
		if (before < 0 && now >= 0) {
			crossings++;
		}
	}
	return (double)crossings * rate / (double)(to - from);
}

static double level_of(const short *pcm, uint64_t frames, int channels, int channel) {
	double sum = 0;
	for (uint64_t i = 0; i < frames; i++) {
		double s = pcm[i * channels + channel] / 32768.0;
		sum += s * s;
	}
	return sqrt(sum / (double)frames);
}

int main(int argc, char **argv) {
	if (argc != 6) {
		fprintf(stderr, "usage: %s FILE RATE SECONDS LEFT_HZ RIGHT_HZ\n", argv[0]);
		return 2;
	}
	const char *path = argv[1];
	int want_rate = atoi(argv[2]);
	double want_seconds = atof(argv[3]), want_left = atof(argv[4]), want_right = atof(argv[5]);

	decode_format_t format = decode_detect_format(path);
	decoder_t *dec = decoder_open(path, format);
	if (!dec) {
		printf("FAIL %s: does not open (format %d)\n", path, (int)format);
		return 1;
	}
	int rate = decoder_sample_rate(dec), channels = decoder_channels(dec);
	if (rate != want_rate || channels != 2) {
		printf("FAIL %s: %d Hz, %d channels; wanted %d Hz stereo\n", path, rate, channels, want_rate);
		return 1;
	}

	// Twice the length it should be, so a file that decodes too long shows as
	// too long instead of being cut off at what was expected.
	uint64_t room = (uint64_t)(want_seconds * 2 * rate);
	short *pcm = malloc(room * 2 * sizeof(short));
	if (!pcm) {
		return 2;
	}
	uint64_t frames = 0;
	while (frames < room) {
		uint64_t ask = room - frames < 4096 ? room - frames : 4096;
		uint64_t got = decoder_read_pcm_frames_s16(dec, ask, pcm + frames * 2);
		if (got == 0) {
			break;
		}
		frames += got;
	}
	const char *codec = decoder_codec_name(dec);
	decoder_close(dec);

	// A lossy codec pads its ends by a frame or two; a twentieth of a second
	// is room for any of them, and far less than a wrong speed would give.
	double seconds = (double)frames / rate;
	double left = pitch_of(pcm, frames, 2, 0, rate), right = pitch_of(pcm, frames, 2, 1, rate);
	double left_level = level_of(pcm, frames, 2, 0), right_level = level_of(pcm, frames, 2, 1);
	free(pcm);

	int bad = 0;
	if (fabs(seconds - want_seconds) > 0.05) {
		printf("FAIL %s: %.3f s long, wanted %.3f\n", path, seconds, want_seconds);
		bad = 1;
	}
	if (fabs(left - want_left) > 2 || fabs(right - want_right) > 2) {
		printf("FAIL %s: %.1f Hz left and %.1f Hz right, wanted %.0f and %.0f\n", path, left, right, want_left, want_right);
		bad = 1;
	}
	// The tone is written at a quarter of full scale, 0.177 as a sine's
	// average. Half or double that is a decoder scaling its samples wrongly.
	if (left_level < 0.12 || left_level > 0.25 || right_level < 0.12 || right_level > 0.25) {
		printf("FAIL %s: levels %.3f and %.3f, wanted about 0.177\n", path, left_level, right_level);
		bad = 1;
	}
	if (!bad) {
		printf("ok: %-12s %-10s %d Hz, %.2f s, %.0f/%.0f Hz, level %.3f\n", path, codec ? codec : "?", rate, seconds, left, right, left_level);
	}
	return bad;
}
