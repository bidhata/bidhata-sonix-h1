#ifndef LEVELMETER_H
#define LEVELMETER_H

#include <stdbool.h>
#include <stddef.h>

// The level of what is going to the output, for the meter of the now-playing
// page (src/gui/nowplaying/vumeter.h).
//
// The audio thread hands over every chunk it has just written; the interface
// thread reads the levels of the sound that is being heard now. The two meet in
// a ring of short blocks with one writer and one reader and no lock, each
// block stamped with the moment it leaves the output, so the meter moves with
// the sound and not a buffer's length ahead of it -- three quarters of a
// second on the card, more over Bluetooth.
//
// Nothing is measured unless the meter asked (levelmeter_set_wanted): with it
// away the audio thread pays one load of a flag per chunk.

// The spectrum: ten bands an octave apart.
#define LEVELMETER_BANDS 10

typedef struct {
	// The mean of the squares of the samples heard since the last read, 0 to 1
	// of full scale, left and right. A mono stream has the same figure twice.
	float mean_square[2];
	// The largest sample in the same stretch, 0 to 1 of full scale.
	float peak[2];
	// The mean square of left and right mixed, in each band, 31 Hz to 16 kHz.
	// All zero unless the spectrum was asked for.
	float band_mean_square[LEVELMETER_BANDS];
} levelmeter_levels_t;

// Interface thread. `levels` starts and stops the measuring; `spectrum` adds
// ten band filters to every sample, so it is asked for only while the spectrum
// is what is drawn.
void levelmeter_set_wanted(bool levels, bool spectrum);

// Audio thread: whether to hand chunks over at all.
bool levelmeter_wanted(void);

// Audio thread: `frames` interleaved frames just written, `bits` 16, 24 (three
// bytes a sample) or 32 a sample. `latency_ms` is how long after this call the
// LAST of them is heard. It only reads. Never a DSD stream: its words are
// packed bits, not levels.
void levelmeter_feed(const void *buf, size_t frames, int channels, int rate, int bits, unsigned latency_ms);

// Interface thread: the levels of everything that became audible since the
// last call. False, with `out` zeroed, when nothing did: paused, stopped, a
// stream that is not levels, or nothing asked for.
bool levelmeter_read(levelmeter_levels_t *out);

#endif /* LEVELMETER_H */
