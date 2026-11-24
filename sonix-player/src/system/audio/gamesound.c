#include "gamesound.h"

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/streaming/radio.h"

#define RATE 44100
#define CHUNK 441 // 10 ms
// Short, so a sound is heard when the thing it belongs to is seen.
#define BUFFER_MS 46
// Each open waits out a busy device for two seconds (audio.c); a paused track
// lets go of it within five.
#define OPEN_TRIES 3

// The longest of the sounds, the three notes at the end of a game.
#define SOUND_FRAMES_MAX (RATE * 420 / 1000)

typedef struct {
	int16_t pcm[SOUND_FRAMES_MAX];
	int frames;
	int pos; // next frame to play; `frames` when silent
} sound_t;

static sound_t sounds[GAMESOUND_COUNT];
static bool made;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mixer;
static bool mixer_live;
static volatile bool stop_flag;

// A tone that slides from `hz0` to `hz1` over `ms`, struck at once and dying
// away, added to `s` from `at_ms`. `decay` is how many times over the level
// falls by e in the length of the tone.
static void tone(sound_t *s, int at_ms, int ms, double hz0, double hz1, double level, double decay) {
	int start = RATE * at_ms / 1000;
	int n = RATE * ms / 1000;
	double phase = 0;
	for (int i = 0; i < n && start + i < SOUND_FRAMES_MAX; i++) {
		double p = (double)i / n;
		double env = exp(-p * decay);
		if (i < 44) { // a millisecond in, so it does not start with a click
			env *= (double)i / 44;
		}
		if (n - i < 220) { // and five out
			env *= (double)(n - i) / 220;
		}
		phase += 2.0 * M_PI * (hz0 + (hz1 - hz0) * p) / RATE;
		double v = s->pcm[start + i] + 32767.0 * level * env * (sin(phase) + 0.3 * sin(2.0 * phase)) / 1.3;
		s->pcm[start + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v));
	}
	if (start + n > s->frames) {
		s->frames = start + n > SOUND_FRAMES_MAX ? SOUND_FRAMES_MAX : start + n;
	}
}

// A burst of noise, duller as it dies: each sample is averaged with more of
// the ones before it.
static void crack(sound_t *s, int ms, double level) {
	int n = RATE * ms / 1000;
	uint32_t seed = 0x9e3779b9;
	double held = 0;
	for (int i = 0; i < n && i < SOUND_FRAMES_MAX; i++) {
		double p = (double)i / n;
		seed = seed * 1664525u + 1013904223u;
		double white = (double)(int32_t)seed / 2147483648.0;
		held += (white - held) * (0.6 - 0.5 * p);
		double env = exp(-p * 5.0) * (i < 44 ? (double)i / 44 : 1.0) * (n - i < 220 ? (double)(n - i) / 220 : 1.0);
		s->pcm[i] = (int16_t)lrint(32767.0 * level * env * held);
	}
	s->frames = n > SOUND_FRAMES_MAX ? SOUND_FRAMES_MAX : n;
}

static void make_sounds(void) {
	memset(sounds, 0, sizeof(sounds));
	// A low thump, falling.
	tone(&sounds[GAMESOUND_DROP], 0, 110, 230, 110, 0.42, 4.0);
	// Two bright notes, a fourth apart, the second over the tail of the first.
	tone(&sounds[GAMESOUND_EXACT], 0, 160, 1318.5, 1318.5, 0.26, 4.0);
	tone(&sounds[GAMESOUND_EXACT], 80, 240, 1760, 1760, 0.26, 4.0);
	// A short blip, rising. It is heard more than once a second, so it is the
	// quietest of the lot.
	tone(&sounds[GAMESOUND_BOUNCE], 0, 70, 520, 800, 0.16, 3.0);
	// The same, longer and going much further up.
	tone(&sounds[GAMESOUND_SPRING], 0, 200, 320, 1280, 0.24, 2.0);
	crack(&sounds[GAMESOUND_BREAK], 90, 0.40);
	// Three notes coming down.
	tone(&sounds[GAMESOUND_OVER], 0, 140, 660, 660, 0.28, 3.0);
	tone(&sounds[GAMESOUND_OVER], 130, 140, 495, 495, 0.28, 3.0);
	tone(&sounds[GAMESOUND_OVER], 260, 160, 330, 330, 0.28, 2.5);
	for (int i = 0; i < GAMESOUND_COUNT; i++) {
		sounds[i].pos = sounds[i].frames;
	}
}

// Fills `out` (stereo) with what is sounding and moves each sound on.
static void mix_into(int16_t *out) {
	int32_t mix[CHUNK] = {0};
	pthread_mutex_lock(&lock);
	for (int i = 0; i < GAMESOUND_COUNT; i++) {
		sound_t *s = &sounds[i];
		int n = s->frames - s->pos;
		if (n > CHUNK) {
			n = CHUNK;
		}
		for (int f = 0; f < n; f++) {
			mix[f] += s->pcm[s->pos + f];
		}
		if (n > 0) {
			s->pos += n;
		}
	}
	pthread_mutex_unlock(&lock);
	for (int f = 0; f < CHUNK; f++) {
		int16_t v = (int16_t)(mix[f] > 32767 ? 32767 : mix[f] < -32768 ? -32768 : mix[f]);
		out[f * 2] = v;
		out[f * 2 + 1] = v;
	}
}

// Opens the output, unless music has it, then writes ten milliseconds at a
// time. Paced by the PCM: every write blocks until there is room. A write that
// takes nothing is slept out instead, so the loop cannot run away.
static void *mixer_main(void *unused) {
	(void)unused;
	if (audio_get_status() == AUDIO_STATUS_PLAYING || radio_is_playing() || radio_is_connecting()) {
		printf("gamesound: music is playing; the game is silent\n");
		return NULL;
	}
	// A paused track keeps its place, and keeps the DAC for a few seconds
	// after the pause (audio.c): it is waited out rather than stopped.
	bool open = false;
	for (int tries = 0; tries < OPEN_TRIES && !stop_flag && !open; tries++) {
		open = audio_external_begin_yielding(RATE, 2, 16, BUFFER_MS);
	}
	if (!open) {
		if (!stop_flag) {
			fprintf(stderr, "gamesound: no PCM; the game is silent\n");
		}
		return NULL;
	}

	int16_t out[CHUNK * 2];
	while (!stop_flag) {
		if (audio_external_wanted()) {
			// A track, a station or a receiver started: it has the output.
			printf("gamesound: something else wants the output; the game is silent from here\n");
			break;
		}
		mix_into(out);
		if (audio_external_write(out, CHUNK) < CHUNK) {
			usleep(CHUNK * 1000000 / RATE);
		}
	}
	audio_external_end();
	return NULL;
}

void gamesound_start(void) {
	gamesound_stop();
	if (!made) {
		make_sounds();
		made = true;
	}
	stop_flag = false;
	if (pthread_create(&mixer, NULL, mixer_main, NULL) == 0) {
		mixer_live = true;
	}
}

void gamesound_play(gamesound_t sound) {
	if (sound < 0 || sound >= GAMESOUND_COUNT) {
		return;
	}
	pthread_mutex_lock(&lock);
	sounds[sound].pos = 0;
	pthread_mutex_unlock(&lock);
}

void gamesound_stop(void) {
	if (!mixer_live) {
		return;
	}
	stop_flag = true;
	pthread_join(mixer, NULL);
	mixer_live = false;
	// Nothing left over to be heard at the start of the next game.
	pthread_mutex_lock(&lock);
	for (int i = 0; i < GAMESOUND_COUNT; i++) {
		sounds[i].pos = sounds[i].frames;
	}
	pthread_mutex_unlock(&lock);
}
