#ifndef THEME_COLOR_H
#define THEME_COLOR_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// The colour maths behind the preset and custom accents, kept apart from
// theme.c so it can be tested on the host without LVGL or a display.
//
// Nothing here reads a config or a style: it turns numbers into colours and
// answers the two questions the theme asks about them -- how light is this
// colour, and which shipped accent is closest to it.

// HSV to RGB. hue is 0..359 (values above wrap), sat and val are 0..255. A
// saturation of 0 gives grey; a value of 0 gives black.
lv_color_t theme_color_hsv(uint16_t hue, uint8_t sat, uint8_t val);

// How saturated a colour is, 0..255: 0 for any grey, 255 for a pure hue.
uint8_t theme_color_saturation(lv_color_t c);

// WCAG relative luminance, 0 (black) to 255 (white).
uint8_t theme_color_luminance(lv_color_t c);

// The foreground for text or an icon on a filled accent: white on the mid and
// dark accents, near-black on the pale ones where white would be unreadable.
lv_color_t theme_color_on(lv_color_t background);

// The index of the preset whose hue is closest to `c`, or -1 when there are
// none. Exact presets map to themselves.
int theme_color_nearest(const lv_color_t *presets, int count, lv_color_t c);

// Reads an 0xRRGGBB value out of the config. False, with `out` untouched, when
// the value has bits above 24 or is negative.
bool theme_color_parse_rgb(long value, lv_color_t *out);

#endif // THEME_COLOR_H
