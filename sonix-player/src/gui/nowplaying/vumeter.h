#ifndef VUMETER_H
#define VUMETER_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// The level meter of the now-playing page, in one of three shapes:
//
//   bars      two bars, left over right, each as long as that channel is loud,
//             with a tick that stays for a moment where the loudest sample
//             reached;
//   dials     two round dials, a needle on each and the same held mark on the
//             arc above it;
//   spectrum  ten columns an octave apart, 31 Hz to 16 kHz, each with a cap
//             that is held and then falls.
//
// Plain objects on a dark panel laid over the foot of the sleeve: a pill for
// the bars, a taller panel for the other two. The scale is in decibels, from
// 48 below full scale at the empty end, so quiet music still moves it. A level
// is the RMS read 3 dB up (a full-scale sine reaches the end), rising quickly
// and falling more slowly; a mark is the largest sample, held and then let
// fall.
//
// It shows the sound that is being heard, not the sound just handed to the
// output: see src/system/audio/levelmeter.h.
//
// One meter, owned by the player (player.c decides where it goes, when it is
// up and what a tap on it does). While it is hidden nothing runs: no timer
// here, and no measuring on the audio thread.

typedef enum {
	VUMETER_BARS,
	VUMETER_DIALS,
	VUMETER_SPECTRUM,
	VUMETER_STYLES,
} vumeter_style_t;

// Builds it on `parent`, hidden and at rest.
void vumeter_create(lv_obj_t *parent);

// Where it goes: from `x`, `w` wide, its foot at `bottom` on `parent`, which
// it is moved to when it is on another object. How tall it is depends on the
// shape.
void vumeter_place(lv_obj_t *parent, int x, int bottom, int w);

// Up and following the output, or put away and stopped.
void vumeter_show(bool on);

// The colour of everything that moves.
void vumeter_set_color(lv_color_t body);

// Which of the three it is.
void vumeter_set_style(vumeter_style_t style);
vumeter_style_t vumeter_style(void);

// Whether a point of the screen is on the meter while it is up: the player
// takes a tap there as "the next shape".
bool vumeter_hit(const lv_point_t *point);

#endif /* VUMETER_H */
