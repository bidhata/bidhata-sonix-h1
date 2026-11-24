#include "levelmeter.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// One block is about 10 ms of sound: short enough that a 40 ms frame of the
// meter sees several, long enough that the ring spans the deepest output (the
// card's three quarters of a second, or a Bluetooth link's second or so) with
// room to spare.
#define BLOCK_MS 10
#define RING_BLOCKS 256 // a power of two; 2.5 s of blocks

// A block heard this long ago was not read in time (the meter was away): it is
// not what is sounding now, and does not count.
#define STALE_MS 250

// Above 48 kHz every Nth frame is measured. The meter shows loudness, and a
// 192 kHz stream does not need 384000 multiplies a second to say how loud.
#define FULL_RATE_LIMIT 48000

typedef struct {
	uint32_t due_ms;					// when the block's last frame is heard, on the monotonic clock
	uint32_t peak[2];					// the largest sample, on the 16-bit scale
	uint32_t mean_sq[2];				// the mean of the squares, on the same (32768 squared fits)
	float band_sq[LEVELMETER_BANDS]; // the mean square of the mix in each band, 0 to 1
} block_t;

static const float band_hz[LEVELMETER_BANDS] = {
	31.25f, 62.5f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f,
};

// One band-pass per band (RBJ's, unity gain at the centre, an octave wide), in
// transposed direct form II. b1 is 0 and b2 is -b0. Touched by the audio
// thread only.
#define BAND_Q 1.414f
typedef struct {
	float b0, a1, a2;
	float z1, z2;
	bool live; // false when the centre is too near half the sample rate
} band_filter_t;
static band_filter_t bands[LEVELMETER_BANDS];
static int bands_rate; // the rate the coefficients are for

static void bands_design(int rate) {
	for (int i = 0; i < LEVELMETER_BANDS; i++) {
		band_filter_t *f = &bands[i];
		memset(f, 0, sizeof(*f));
		if (band_hz[i] > 0.45f * (float)rate) {
			continue;
		}
		float w0 = 2.0f * 3.14159265f * band_hz[i] / (float)rate;
		float alpha = sinf(w0) / (2.0f * BAND_Q);
		float a0 = 1.0f + alpha;
		f->b0 = alpha / a0;
		f->a1 = -2.0f * cosf(w0) / a0;
		f->a2 = (1.0f - alpha) / a0;
		f->live = true;
	}
	bands_rate = rate;
}

static block_t ring[RING_BLOCKS];
// The head is written by the audio thread only, the tail by the reader only.
static atomic_uint ring_head;
static atomic_uint ring_tail;
static atomic_bool want_levels;
static atomic_bool want_spectrum;

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

void levelmeter_set_wanted(bool levels, bool spectrum) {
	atomic_store_explicit(&want_spectrum, levels && spectrum, memory_order_relaxed);
	atomic_store_explicit(&want_levels, levels, memory_order_relaxed);
}

bool levelmeter_wanted(void) { return atomic_load_explicit(&want_levels, memory_order_relaxed); }

static void push_block(const block_t *block) {
	unsigned head = atomic_load_explicit(&ring_head, memory_order_relaxed);
	unsigned tail = atomic_load_explicit(&ring_tail, memory_order_acquire);
	// Full: the reader stopped reading. The new block is dropped rather than
	// the reader's index moved from this thread.
	if (head - tail >= RING_BLOCKS) {
		return;
	}
	ring[head & (RING_BLOCKS - 1)] = *block;
	atomic_store_explicit(&ring_head, head + 1, memory_order_release);
}

// A sample on the 16-bit scale, whatever it is stored as.
static inline int32_t sample16(const uint8_t *at, int bits) {
	if (bits == 16) {
		return *(const int16_t *)(const void *)at;
	}
	if (bits == 32) {
		return *(const int32_t *)(const void *)at >> 16;
	}
	return (int32_t)((int8_t)at[2]) * 256 + at[1]; // 24 bits in three bytes, low byte first
}

void levelmeter_feed(const void *buf, size_t frames, int channels, int rate, int bits, unsigned latency_ms) {
	if (!buf || frames == 0 || channels < 1 || rate <= 0 || (bits != 16 && bits != 24 && bits != 32)) {
		return;
	}
	size_t stride = rate > FULL_RATE_LIMIT ? (size_t)(rate + 22050) / 44100 : 1;
	size_t block_frames = (size_t)rate * BLOCK_MS / 1000;
	if (block_frames < stride) {
		block_frames = stride;
	}
	size_t sample_bytes = (size_t)bits / 8;
	size_t frame_bytes = sample_bytes * (size_t)channels;
	size_t right = channels > 1 ? sample_bytes : 0;
	uint32_t end_ms = now_ms() + latency_ms;

	bool spectrum = atomic_load_explicit(&want_spectrum, memory_order_relaxed);
	int measured_rate = rate / (int)stride;
	if (spectrum && bands_rate != measured_rate) {
		bands_design(measured_rate);
	}

	for (size_t start = 0; start < frames; start += block_frames) {
		size_t stop = start + block_frames < frames ? start + block_frames : frames;
		uint32_t peak_l = 0, peak_r = 0, count = 0;
		uint64_t sum_l = 0, sum_r = 0;
		float band_sum[LEVELMETER_BANDS] = {0};

		for (size_t f = start; f < stop; f += stride) {
			const uint8_t *frame = (const uint8_t *)buf + f * frame_bytes;
			int32_t l = sample16(frame, bits);
			int32_t r = sample16(frame + right, bits);
			uint32_t al = (uint32_t)(l < 0 ? -l : l);
			uint32_t ar = (uint32_t)(r < 0 ? -r : r);
			if (al > peak_l) {
				peak_l = al;
			}
			if (ar > peak_r) {
				peak_r = ar;
			}
			sum_l += (uint64_t)al * al;
			sum_r += (uint64_t)ar * ar;
			count++;

			if (spectrum) {
				float x = (float)(l + r) * (1.0f / 65536.0f);
				for (int b = 0; b < LEVELMETER_BANDS; b++) {
					band_filter_t *bf = &bands[b];
					if (!bf->live) {
						continue;
					}
					float y = bf->b0 * x + bf->z1;
					bf->z1 = bf->z2 - bf->a1 * y;
					bf->z2 = -bf->b0 * x - bf->a2 * y;
					band_sum[b] += y * y;
				}
			}
		}
		if (!count) {
			continue;
		}

		block_t block = {
			.due_ms = end_ms - (uint32_t)((uint64_t)(frames - stop) * 1000u / (unsigned)rate),
			.peak = {peak_l, peak_r},
			.mean_sq = {(uint32_t)(sum_l / count), (uint32_t)(sum_r / count)},
		};
		if (spectrum) {
			for (int b = 0; b < LEVELMETER_BANDS; b++) {
				block.band_sq[b] = band_sum[b] / (float)count;
				// A filter ringing down into denormals costs a trap into the
				// kernel for every operation on this FPU: it is stopped at
				// silence instead.
				if (fabsf(bands[b].z1) < 1e-15f) {
					bands[b].z1 = 0.0f;
				}
				if (fabsf(bands[b].z2) < 1e-15f) {
					bands[b].z2 = 0.0f;
				}
			}
		}
		push_block(&block);
	}
}

bool levelmeter_read(levelmeter_levels_t *out) {
	memset(out, 0, sizeof(*out));
	unsigned tail = atomic_load_explicit(&ring_tail, memory_order_relaxed);
	unsigned head = atomic_load_explicit(&ring_head, memory_order_acquire);
	uint32_t now = now_ms();

	uint64_t sum[2] = {0, 0};
	uint32_t peak[2] = {0, 0};
	float band[LEVELMETER_BANDS] = {0};
	unsigned taken = 0;
	while (tail != head) {
		const block_t *block = &ring[tail & (RING_BLOCKS - 1)];
		// Signed, so the comparison survives the counter wrapping.
		int32_t late_ms = (int32_t)(now - block->due_ms);
		if (late_ms < 0) {
			break; // not heard yet
		}
		tail++;
		if (late_ms > STALE_MS) {
			continue;
		}
		for (int c = 0; c < 2; c++) {
			sum[c] += block->mean_sq[c];
			if (block->peak[c] > peak[c]) {
				peak[c] = block->peak[c];
			}
		}
		for (int b = 0; b < LEVELMETER_BANDS; b++) {
			band[b] += block->band_sq[b];
		}
		taken++;
	}
	atomic_store_explicit(&ring_tail, tail, memory_order_release);
	if (!taken) {
		return false;
	}

	for (int c = 0; c < 2; c++) {
		out->mean_square[c] = (float)(sum[c] / taken) / (32768.0f * 32768.0f);
		out->peak[c] = (float)peak[c] / 32768.0f;
	}
	for (int b = 0; b < LEVELMETER_BANDS; b++) {
		out->band_mean_square[b] = band[b] / (float)taken;
	}
	return true;
}
