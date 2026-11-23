// Host test harness for the MSEB Extended Bass clip fix (issue #180).
//
// It includes the production eq.c directly so the static measurement, policy
// and soft-clip functions are exercised exactly as the audio thread calls them.
// Only the three config symbols eq.c references need stubs.
//
// Build and run from sonix-player/:
//
//   gcc -O2 -Wall -Wextra -D_FILE_OFFSET_BITS=64 -I.
//       tools/mseb_clip_test.c -lm -o /tmp/mseb_clip_test
//   /tmp/mseb_clip_test
//
// Prints PASS/FAIL per assertion and exits nonzero if anything failed.

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// --- the only external symbols eq.c needs ----------------------------------

long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	(void)key;
	return fallback;
}

void config_set_int(const char *section, const char *key, long value) {
	(void)section;
	(void)key;
	(void)value;
}

bool config_save(void) { return true; }

#include "src/system/audio/eq.c"

// --- test plumbing ---------------------------------------------------------

#define TEST_RATE 44100
#define TEST_FREQ 60.0
#define WARM_FRAMES 4410 // 100 ms: covers the 20 ms pregain glide
// 150 exact 60 Hz cycles at 44.1 kHz (735 samples each) after the warm-up.
#define SIGNAL_FRAMES (WARM_FRAMES + 150 * 735)

static int checks;
static int failures;

static void check(int ok, const char *what) {
	checks++;
	if (!ok) {
		failures++;
	}
	printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
}

static void check_close(double got, double want, double tol, const char *what) {
	char msg[256];
	snprintf(msg, sizeof(msg), "%s [got %+.6f, want %+.6f +/- %.4f]", what, got, want, tol);
	check(fabs(got - want) <= tol, msg);
}

static void checkf(int ok, const char *fmt, ...) {
	char msg[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	check(ok, msg);
}

// --- MSEB setting helpers --------------------------------------------------

static void mseb_setup(void) {
	mseb_set_enabled(true);
	mseb_set_range(100);
	mseb_reset();
}

static void mseb_one(int band, int value) {
	mseb_setup();
	mseb_set_value(band, value);
}

// --- audio helpers ---------------------------------------------------------

static short in_buf[SIGNAL_FRAMES];
static short out_buf[SIGNAL_FRAMES];
static int32_t s32_buf[SIGNAL_FRAMES];

static void make_sine(double amplitude) {
	for (int i = 0; i < SIGNAL_FRAMES; i++) {
		double s = amplitude * 32767.0 * sin(2.0 * M_PI * TEST_FREQ * (double)i / (double)TEST_RATE);
		in_buf[i] = (short)lrint(s);
	}
}

static int peak_abs(const short *buf, int count, int from) {
	int peak = 0;
	for (int i = from; i < count; i++) {
		int a = buf[i] < 0 ? -(int)buf[i] : (int)buf[i];
		if (a > peak) {
			peak = a;
		}
	}
	return peak;
}

// Peak of an int32 buffer in 16-bit LSB units, plus a count of samples that
// would not fit back into int32 (a wrap guard).
static double s32_peak16(const int32_t *buf, int count, int from, int *over) {
	int64_t peak = 0;
	*over = 0;
	for (int i = from; i < count; i++) {
		int64_t a = buf[i] < 0 ? -(int64_t)buf[i] : (int64_t)buf[i];
		if (a > peak) {
			peak = a;
		}
		if (a > INT32_MAX) {
			(*over)++;
		}
	}
	return (double)peak / 65536.0;
}

static int limit_hits(const short *buf, int count) {
	int hits = 0;
	for (int i = 0; i < count; i++) {
		int a = buf[i] < 0 ? -(int)buf[i] : (int)buf[i];
		if (a >= 32767) {
			hits++;
		}
	}
	return hits;
}

// Amplitude of one exact bin, by correlation over an integer number of cycles.
static double bin_amplitude(const short *buf, int count, double freq) {
	double w = 2.0 * M_PI * freq / (double)TEST_RATE;
	double re = 0.0;
	double im = 0.0;
	for (int i = 0; i < count; i++) {
		re += (double)buf[i] * cos(w * (double)i);
		im += (double)buf[i] * sin(w * (double)i);
	}
	return 2.0 * sqrt(re * re + im * im) / (double)count;
}

static double thd_percent(const short *buf, int count, int max_harmonic) {
	double fundamental = bin_amplitude(buf, count, TEST_FREQ);
	if (fundamental <= 0.0) {
		return 1.0e9;
	}
	double sum = 0.0;
	for (int k = 2; k <= max_harmonic; k++) {
		double a = bin_amplitude(buf, count, TEST_FREQ * (double)k);
		sum += a * a;
	}
	return 100.0 * sqrt(sum) / fundamental;
}

// The production chain, run exactly as eq_process() runs it, but returning the
// peak at the shaper input so the test can see what the soft clip was given.
// Shared filter state is intentional: this is called after eq_process() has
// settled the settings, so the replica starts in the same steady state.
static float eq_chain_peak(const short *frames, int frame_count, int channels, int sample_rate, int from_frame) {
	if (!process_prepare(frame_count, channels, sample_rate)) {
		return 0.0f;
	}
	float step = 1.0f / ((float)sample_rate * 0.02f);
	float peak = 0.0f;
	for (int done = 0; done < frame_count; done += EQ_PASS_FRAMES) {
		int n = frame_count - done < EQ_PASS_FRAMES ? frame_count - done : EQ_PASS_FRAMES;
		const short *in = frames + (size_t)done * channels;
		int samples = n * channels;
		for (int i = 0; i < samples; i++) {
			pass_buf[i] = (float)in[i];
		}
		chain_run_pass(n, channels, step);
		for (int i = 0; i < samples; i++) {
			if (done + i / channels < from_frame) {
				continue;
			}
			float a = pass_buf[i] < 0.0f ? -pass_buf[i] : pass_buf[i];
			if (a > peak) {
				peak = a;
			}
		}
	}
	return peak;
}

// --- tests -----------------------------------------------------------------

static void test_soft_clip(void) {
	const float fs = 32767.0f;
	const float t = 0.95f * fs;

	check(soft_clip_sample(0.0f) == 0.0f, "soft clip: zero maps to zero");
	check(soft_clip_sample(t) == t, "soft clip: identity exactly at the knee");
	check(soft_clip_sample(-t) == -t, "soft clip: identity exactly at the negative knee");
	check(fabsf(soft_clip_sample(t + 0.001f) - t) < 0.01f, "soft clip: continuous at the knee");

	int unity = 1;
	for (int i = -1000; i <= 1000; i++) {
		float x = t * (float)i / 1000.0f;
		if (soft_clip_sample(x) != x) {
			unity = 0;
		}
	}
	check(unity, "soft clip: bit-exact everywhere below the knee");

	int monotone = 1;
	int bounded = 1;
	float prev = soft_clip_sample(-10.0f * fs);
	for (int i = 1; i <= 100000; i++) {
		float x = -10.0f * fs + (20.0f * fs) * (float)i / 100000.0f;
		float y = soft_clip_sample(x);
		if (y < prev) {
			monotone = 0;
		}
		if (fabsf(y) > fs) {
			bounded = 0;
		}
		prev = y;
	}
	check(monotone, "soft clip: monotonic from -10 FS to +10 FS");
	check(bounded, "soft clip: never exceeds full scale up to 10 FS");
	check(fabsf(soft_clip_sample(10.0f * fs)) <= fs, "soft clip: bounded at +10 FS");
	check(fabsf(soft_clip_sample(-10.0f * fs)) <= fs, "soft clip: bounded at -10 FS");
	checkf(soft_clip_sample(fs) > t && soft_clip_sample(fs) < fs, "soft clip: full scale compresses to %.1f",
		   (double)soft_clip_sample(fs));
}

static void test_stock_search(void) {
	static const int values[] = {10, 20, 30, 40, 50, 60};
	static const double expected[] = {-0.59, -0.98, -1.37, -1.76, -2.15, -2.54};
	char what[128];

	for (int i = 0; i < (int)(sizeof(values) / sizeof(values[0])); i++) {
		mseb_one(1, values[i]);
		mseb_level_t level = mseb_level_measure(TEST_RATE);
		snprintf(what, sizeof(what), "stock search: Extended Bass %d matches the user's log", values[i]);
		check_close(level.pregain_db, expected[i], 0.01, what);
	}
}

static void test_policy(void) {
	int bounded = 1;
	for (int v = 0; v <= 100; v += 5) {
		mseb_one(1, v);
		mseb_level_t level = mseb_level_measure(TEST_RATE);
		float effective = mseb_effective_db(&level, NULL);
		double net = (double)effective + (double)level.peak_db;
		if (net > 10.3 + 0.002) {
			bounded = 0;
			printf("      v=%d net=%+0.4f exceeds the ceiling\n", v, net);
		}
	}
	check(bounded, "policy: effective net <= +10.3 dB for Extended Bass 0..100 step 5");

	mseb_one(1, 0);
	mseb_level_t l0 = mseb_level_measure(TEST_RATE);
	float p0 = mseb_effective_db(&l0, NULL);
	check(l0.pregain_db == 0.0f && l0.peak_db == 0.0f && p0 == 0.0f,
		  "policy: a flat bank is exactly p = peak = net = 0");

	mseb_one(1, 60);
	mseb_level_t l60 = mseb_level_measure(TEST_RATE);
	float p60 = mseb_effective_db(&l60, NULL);
	check_close(l60.peak_db, 15.000, 0.002, "peak(60): analytic DC asymptote is +15.000 dB");
	check_close(p60, -5.903516, 0.002, "pregain(60): 0.73-scaled effective value");
	check_close((double)p60 + (double)l60.peak_db, 9.0965, 0.005, "policy: net(60) is the stock level");
	check(l60.boosts, "policy: Extended Bass 60 is a boosting bank");

	mseb_one(1, 69);
	mseb_level_t l69 = mseb_level_measure(TEST_RATE);
	bool capped69 = false;
	float p69 = mseb_effective_db(&l69, &capped69);
	check_close((double)p69 + (double)l69.peak_db, 10.300, 0.002, "policy: net(69) is exactly at the ceiling");
	check(capped69, "policy: the +10.3 dB ceiling engages at v=69");

	mseb_one(1, 70);
	mseb_level_t l70 = mseb_level_measure(TEST_RATE);
	float p70 = mseb_effective_db(&l70, NULL);
	check_close(l70.peak_db, 17.500, 0.002, "peak(70): analytic DC asymptote is +17.500 dB");
	check_close(p70, -7.2, 0.002, "pregain(70): capped effective value");
	check_close((double)p70 + (double)l70.peak_db, 10.300, 0.002, "policy: net(70) is held at the ceiling");

	mseb_one(1, 100);
	mseb_level_t l100 = mseb_level_measure(TEST_RATE);
	float p100 = mseb_effective_db(&l100, NULL);
	check_close(l100.peak_db, 25.000, 0.002, "peak(100): analytic DC asymptote is +25.000 dB");
	check_close(p100, -14.7, 0.002, "pregain(100): capped effective value");
	check_close((double)p100 + (double)l100.peak_db, 10.300, 0.002, "policy: net(100) is held at the ceiling");
}

static void test_mixed(void) {
	// Brief's mixed case: Extended Bass 60 + Air -100 together. The low shelf
	// still peaks at DC, the Air cut is at Nyquist, and the combined net must
	// stay under the ceiling.
	mseb_setup();
	mseb_set_value(1, 60);
	mseb_set_value(9, -100);

	mseb_level_t level = mseb_level_measure(TEST_RATE);
	float effective = mseb_effective_db(&level, NULL);
	double net = (double)effective + (double)level.peak_db;
	check(level.boosts, "mixed: Extended Bass 60 + Air -100 is a boosting bank");
	check_close(net, 9.9519, 0.05, "mixed: effective net stays at the calibrated level");
	checkf(net <= 10.3 + 0.002, "mixed: effective net %.4f <= +10.3 dB", net);
	checkf(effective < level.pregain_db, "mixed: effective p %.4f < stock p %.4f", (double)effective,
		   (double)level.pregain_db);
}

static void test_cut_only(void) {
	// Temperature's db_per_step is -0.06, so +100 is the cut (four -6 dB
	// shelves). No filter boosts, so the stock positive make-up gain stays.
	// With the analytic DC asymptote the measured peak is exactly 0 dB -- high
	// shelves are unity at DC -- above the 20 Hz-grid value of -1.93 dB; it
	// changes nothing here because the bank takes the cut-only branch.
	mseb_one(0, 100);
	mseb_level_t level = mseb_level_measure(TEST_RATE);
	float effective = mseb_effective_db(&level, NULL);
	check(!level.boosts, "cut-only: Temperature +100 reports no boost");
	check_close(level.peak_db, 0.0, 0.001, "cut-only: analytic DC asymptote is 0 dB");
	check_close(level.pregain_db, 11.1328125, 1.0e-4, "cut-only: stock p is +11.1328125 dB");
	checkf(fabsf(effective - level.pregain_db) <= 1.0e-6f, "cut-only: effective p equals stock p (%.7f)",
		   (double)effective);
	check_close((double)effective + (double)level.peak_db, 11.1328125, 0.002,
				"cut-only: net keeps the stock make-up gain");
}

static void test_air_cut(void) {
	// The 0.7289 Q leaves +0.0135 dB of ripple at ~1.4 kHz on a nominally
	// cutting Air shelf. That is not a boost: the declared gains decide.
	mseb_one(9, -100);
	mseb_level_t level = mseb_level_measure(TEST_RATE);
	float effective = mseb_effective_db(&level, NULL);
	check(!level.boosts, "air cut: Air -100 reports no boost despite the ripple");
	check_close(level.peak_db, 0.0135, 0.001, "air cut: measured ripple is +0.0135 dB");
	check_close(level.pregain_db, 1.3671875, 1.0e-4, "air cut: stock p is +1.3671875 dB");
	checkf(fabsf(effective - level.pregain_db) <= 1.0e-6f, "air cut: effective p equals stock p (%.7f)",
		   (double)effective);
	check_close((double)effective + (double)level.peak_db, 1.3806, 0.002, "air cut: net is the stock make-up");
}

static void test_air_boost_allcuts(void) {
	// Air +5 with every other slider at -100. Temperature's -100 is itself a
	// +6 dB boost, so the cascade peaks at Nyquist (+25.25 dB) and the stock
	// make-up is only +13.87 dB; the policy must cap the net at +10.3.
	mseb_setup();
	for (int i = 0; i < MSEB_BANDS; i++) {
		mseb_set_value(i, -100);
	}
	mseb_set_value(9, 5);

	mseb_level_t level = mseb_level_measure(TEST_RATE);
	float effective = mseb_effective_db(&level, NULL);
	double net = (double)effective + (double)level.peak_db;
	check(level.boosts, "Air +5, others -100: declared boost is detected");
	check_close(level.peak_db, 25.250, 0.01, "Air +5, others -100: peak is +25.25 dB at Nyquist");
	check_close(level.pregain_db, 13.8671875, 1.0e-4, "Air +5, others -100: stock p is +13.8671875 dB");
	check_close(effective, -14.95, 0.002, "Air +5, others -100: effective p is -14.95 dB");
	check_close(net, 10.300, 0.002, "Air +5, others -100: net is capped at +10.3 dB");
}

static void test_boost_hidden_by_cuts(void) {
	// Air +5 is the only positive filter. Temperature +100 cuts, and every
	// other slider is at -100, so the combined cascade peak is strongly
	// negative (-22.75 dB) while the stock make-up gain is +34.57 dB. The
	// declared gains must still call this a boost and cap it: gating on the
	// sign of the combined peak would leave the stage ~11.8 dB over.
	mseb_setup();
	mseb_set_value(0, 100);
	for (int i = 1; i < MSEB_BANDS; i++) {
		mseb_set_value(i, -100);
	}
	mseb_set_value(9, 5);

	mseb_level_t level = mseb_level_measure(TEST_RATE);
	float effective = mseb_effective_db(&level, NULL);
	double net = (double)effective + (double)level.peak_db;
	check(level.boosts, "boost hidden by cuts: declared +5 dB Air is detected as a boost");
	check_close(level.peak_db, -22.75, 0.01, "boost hidden by cuts: combined peak is -22.75 dB");
	check_close(level.pregain_db, 34.5703125, 1.0e-4, "boost hidden by cuts: stock p is +34.5703125 dB");
	check_close(effective, 31.3788, 0.005, "boost hidden by cuts: effective p is +31.3788 dB");
	check_close(net, 8.6288, 0.005, "boost hidden by cuts: effective net is +8.6288 dB");
	checkf(effective < level.pregain_db, "boost hidden by cuts: effective p %.3f < stock p %.3f", (double)effective,
		   (double)level.pregain_db);
}

static void test_temp_all_cuts(void) {
	// Temperature +100 with every other slider at -100: no positive filter, so
	// the stock make-up (+35.74 dB) is preserved verbatim and the net is
	// +10.74 dB. That is the deliberate cut-only exemption.
	mseb_setup();
	mseb_set_value(0, 100);
	for (int i = 1; i < MSEB_BANDS; i++) {
		mseb_set_value(i, -100);
	}

	mseb_level_t level = mseb_level_measure(TEST_RATE);
	float effective = mseb_effective_db(&level, NULL);
	check(!level.boosts, "Temperature +100, others -100: no boost declared");
	check_close(level.peak_db, -25.000, 0.002, "Temperature +100, others -100: DC peak is -25.000 dB");
	check_close(level.pregain_db, 35.7421875, 1.0e-4, "Temperature +100, others -100: stock p is +35.7421875 dB");
	checkf(fabsf(effective - level.pregain_db) <= 1.0e-6f, "Temperature +100, others -100: effective p equals stock p");
	check_close((double)effective + (double)level.peak_db, 10.7422, 0.002,
				"Temperature +100, others -100: net keeps the stock make-up");
}

static void test_nyquist_probe(void) {
	// At 22.05 kHz the 10 kHz Air shelf is allowed and holds its declared +25 dB
	// at Nyquist, where the 128-point grid (which stops below Nyquist and zeroes
	// the rest) cannot see it. The analytic asymptote must recover it exactly.
	mseb_one(9, 100);
	mseb_level_t level = mseb_level_measure(22050);
	check(level.boosts, "peak probe: Air +100 at 22.05 kHz is a boosting bank");
	check_close(level.peak_db, 25.000, 0.002, "peak probe: Nyquist asymptote is +25.000 dB");
}

static void test_audio_bass60(void) {
	mseb_one(1, 60);

	make_sine(0.5);
	memcpy(out_buf, in_buf, sizeof(in_buf));
	eq_process(out_buf, SIGNAL_FRAMES, 1, TEST_RATE);

	check(clip_guard, "audio v=60: clip guard is on for a boosting MSEB bank");

	int peak = peak_abs(out_buf, SIGNAL_FRAMES, WARM_FRAMES);
	checkf(peak >= 24551 && peak <= 24571, "audio v=60: 60 Hz 0.5 FS output peak %d LSB (expect 24561 +/- 10)",
		   peak);
	checkf(limit_hits(out_buf, SIGNAL_FRAMES) == 0, "audio v=60: no samples at the 16-bit limit");

	double thd = thd_percent(out_buf + WARM_FRAMES, SIGNAL_FRAMES - WARM_FRAMES, 20);
	checkf(thd < 0.01, "audio v=60: THD %0.5f%% < 0.01%%", thd);

	// What the soft clip was given, for the record.
	float pre = eq_chain_peak(in_buf, SIGNAL_FRAMES, 1, TEST_RATE, WARM_FRAMES);
	printf("      info: v=60 pre-shaper peak %.5f FS\n", (double)pre / 32767.0);
}

static void test_audio_bass100(void) {
	mseb_one(1, 100);

	make_sine(1.0);
	memcpy(out_buf, in_buf, sizeof(in_buf));
	eq_process(out_buf, SIGNAL_FRAMES, 1, TEST_RATE);

	check(clip_guard, "audio v=100: clip guard is on");

	int peak = peak_abs(out_buf, SIGNAL_FRAMES, WARM_FRAMES);
	checkf(peak >= 32118 && peak <= 32138, "audio v=100: 60 Hz 1.0 FS soft-clips to %d LSB (expect 32128 +/- 10)",
		   peak);
	checkf(limit_hits(out_buf, SIGNAL_FRAMES) == 0, "audio v=100: no samples at the 16-bit limit");

	double thd = thd_percent(out_buf + WARM_FRAMES, SIGNAL_FRAMES - WARM_FRAMES, 20);
	checkf(thd < 2.5, "audio v=100: THD %0.3f%% < 2.5%%", thd);

	float pre = eq_chain_peak(in_buf, SIGNAL_FRAMES, 1, TEST_RATE, WARM_FRAMES);
	printf("      info: v=100 pre-shaper peak %.5f FS (soft clip output %d LSB)\n", (double)pre / 32767.0, peak);
	checkf(pre > (float)peak, "audio v=100: the shaper reduced the peak (%.1f -> %d)", (double)pre, peak);
}

static void test_audio_s32(void) {
	// Below the knee, the same case as the 16-bit path.
	mseb_one(1, 60);
	make_sine(0.5);
	for (int i = 0; i < SIGNAL_FRAMES; i++) {
		s32_buf[i] = (int32_t)lrint((double)in_buf[i] * 65536.0);
	}
	eq_process_s32(s32_buf, SIGNAL_FRAMES, 1, TEST_RATE);

	int over = 0;
	double peak16 = s32_peak16(s32_buf, SIGNAL_FRAMES, WARM_FRAMES, &over);
	checkf(over == 0, "s32 v=60: no sample exceeds INT32 range");
	checkf(peak16 >= 24551.0 && peak16 <= 24571.0, "s32 v=60: 0.5 FS equivalent peak %.1f LSB (expect 24561)",
		   peak16);

	// Above the knee: the soft clip must take over in the 32-bit path too.
	mseb_one(1, 100);
	make_sine(1.0);
	for (int i = 0; i < SIGNAL_FRAMES; i++) {
		s32_buf[i] = (int32_t)lrint((double)in_buf[i] * 65536.0);
	}
	eq_process_s32(s32_buf, SIGNAL_FRAMES, 1, TEST_RATE);

	peak16 = s32_peak16(s32_buf, SIGNAL_FRAMES, WARM_FRAMES, &over);
	checkf(over == 0, "s32 v=100: no sample exceeds INT32 range");
	checkf(peak16 >= 32118.0 && peak16 <= 32138.0,
		   "s32 v=100: 1.0 FS soft-clips to %.1f LSB (expect 32128 +/- 10)", peak16);
}

static void test_glide_guard(void) {
	// A settled flat chain has clip_guard off, but a running make-up gain from
	// a previous setting can still be above unity while it glides down. The
	// local guard in eq_process must keep the soft clip on for that buffer.
	short buf[1024];

	mseb_setup();
	filters_rebuild(TEST_RATE);
	check(!clip_guard, "glide guard: settled flat chain has the guard off");

	mseb_pregain = 1.5f; // as if arriving from a +3.5 dB make-up setting
	mseb_pregain_target = 1.0f;
	for (int i = 0; i < 1024; i++) {
		buf[i] = (short)lrint(0.9 * 32767.0 * sin(2.0 * M_PI * TEST_FREQ * (double)i / (double)TEST_RATE));
	}
	eq_process(buf, 1024, 1, TEST_RATE);

	checkf(limit_hits(buf, 1024) == 0, "glide guard: soft clip covers a running gain above unity");
	checkf(peak_abs(buf, 1024, 0) < 32767, "glide guard: transient peak %d LSB never hits the clamp",
		   peak_abs(buf, 1024, 0));
	check_close(mseb_pregain, 1.0, 1.0e-6, "glide guard: the glide still lands on its target");
}

static void test_flat_enabled(void) {
	// MSEB enabled with every slider at 0 is a flat chain: the gains are
	// exactly 1, clip_guard is false, and the buffer must come back untouched.
	// One warm-up buffer lets a running gain from the previous test settle.
	short scratch[1000];
	short a[1000];
	short b[1000];

	mseb_setup();
	filters_rebuild(TEST_RATE);
	check(!clip_guard, "flat enabled: clip guard is off for a flat MSEB bank");

	for (int i = 0; i < 1000; i++) {
		scratch[i] = (short)((i * 104729) % 65536 - 32768);
	}
	eq_process(scratch, 1000, 1, TEST_RATE); // warm-up: settles the gain, no comparison

	for (int i = 0; i < 1000; i++) {
		a[i] = (short)((i * 7919) % 65536 - 32768);
	}
	memcpy(b, a, sizeof(a));
	eq_process(b, 500, 2, TEST_RATE);
	check(memcmp(a, b, sizeof(a)) == 0, "flat enabled: MSEB on with all sliders 0 is bit-exact");
}

static void test_bypass(void) {
	const int frames = 500; // 500 stereo frames = 1000 samples
	short a[1000];
	short b[1000];

	mseb_set_enabled(false);
	filters_rebuild(TEST_RATE);
	check(!clip_guard, "bypass: clip guard is off for a flat/disabling chain");

	for (int i = 0; i < 2 * frames; i++) {
		a[i] = (short)((i * 7919) % 65536 - 32768);
	}
	memcpy(b, a, sizeof(a));

	eq_process(b, frames, 2, TEST_RATE);
	check(memcmp(a, b, sizeof(a)) == 0, "bypass: MSEB/EQ/PEQ off leaves the buffer bit-exact");
}

int main(void) {
	test_soft_clip();
	test_stock_search();
	test_policy();
	test_mixed();
	test_cut_only();
	test_air_cut();
	test_air_boost_allcuts();
	test_boost_hidden_by_cuts();
	test_temp_all_cuts();
	test_nyquist_probe();
	test_audio_bass60();
	test_audio_bass100();
	test_audio_s32();
	test_glide_guard();
	test_flat_enabled();
	test_bypass();

	printf("\n%d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
	return failures == 0 ? 0 : 1;
}
