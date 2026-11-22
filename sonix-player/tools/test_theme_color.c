// The colour maths behind a custom accent, on the host and against no LVGL
// library: theme_color.c is the same file the player builds.
//
// Built and run by `make test-theme-color`. Not part of host or target.

#include "src/gui/shell/theme_color.h"

#include <stdio.h>
#include <stdlib.h>

// The three LVGL colour constructors theme_color.c calls. In the player they
// come from lv_color.c; here the definitions keep the test free of the library
// and the display, which is the point of the module being separate.
lv_color_t lv_color_make(uint8_t r, uint8_t g, uint8_t b) {
	lv_color_t c = {.red = r, .green = g, .blue = b};
	return c;
}

lv_color_t lv_color_white(void) { return lv_color_make(255, 255, 255); }

lv_color_t lv_color_black(void) { return lv_color_make(0, 0, 0); }

#define CHECK(cond, name)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              \
	do {                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               \
		if (cond) {                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    \
			printf("ok   %s\n", name);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 \
		} else {                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       \
			printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     \
			exit(1);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   \
		}                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              \
	} while (0)

static int near(int a, int b) { return a > b ? a - b : b - a; }

static int same(lv_color_t c, int r, int g, int b) { return near(c.red, r) <= 1 && near(c.green, g) <= 1 && near(c.blue, b) <= 1; }

int main(void) {
	// 1. The primaries come out of the wheel in the right places.
	lv_color_t red = theme_color_hsv(0, 255, 255);
	lv_color_t green = theme_color_hsv(120, 255, 255);
	lv_color_t blue = theme_color_hsv(240, 255, 255);
	CHECK(same(red, 255, 0, 0), "hue 0 is red");
	CHECK(same(green, 0, 255, 0), "hue 120 is green");
	CHECK(same(blue, 0, 0, 255), "hue 240 is blue");

	// 2. Every hue on the outer ring has a full channel and stays a colour.
	int ok = 1;
	for (int h = 0; h < 360; h++) {
		lv_color_t c = theme_color_hsv((uint16_t)h, 255, 255);
		int max = c.red > c.green ? c.red : c.green;
		if (c.blue > max) {
			max = c.blue;
		}
		int min = c.red < c.green ? c.red : c.green;
		if (c.blue < min) {
			min = c.blue;
		}
		if (max != 255 || min != 0) {
			printf("hue %d gave (%d,%d,%d)\n", h, c.red, c.green, c.blue);
			ok = 0;
		}
	}
	CHECK(ok, "every hue has one channel at 255 and one at 0");

	// 3. Degenerate inputs: grey has no hue, black is black, white is white.
	CHECK(same(theme_color_hsv(0, 0, 128), 128, 128, 128), "zero saturation is grey");
	CHECK(same(theme_color_hsv(0, 255, 0), 0, 0, 0), "zero value is black");
	CHECK(same(theme_color_hsv(0, 0, 255), 255, 255, 255), "full value with no saturation is white");

	// 4. Luminance orders the way the eye does.
	CHECK(theme_color_luminance(lv_color_make(255, 255, 255)) == 255, "white luminance is 255");
	CHECK(theme_color_luminance(lv_color_make(0, 0, 0)) == 0, "black luminance is 0");
	CHECK(theme_color_luminance(lv_color_make(46, 194, 126)) < theme_color_luminance(lv_color_make(229, 165, 10)), "yellow reads brighter than green");

	// 5. On-accent foreground: white on the presets that are dark, black on
	// the pale ones. These are the six shipped accents.
	CHECK(same(theme_color_on(lv_color_make(53, 132, 228)), 255, 255, 255), "white on blue");
	CHECK(same(theme_color_on(lv_color_make(255, 120, 0)), 0, 0, 0), "black on orange");
	CHECK(same(theme_color_on(lv_color_make(229, 165, 10)), 0, 0, 0), "black on yellow");
	CHECK(same(theme_color_on(lv_color_make(46, 194, 126)), 0, 0, 0), "black on green");
	CHECK(same(theme_color_on(lv_color_make(145, 65, 172)), 255, 255, 255), "white on purple");
	CHECK(same(theme_color_on(lv_color_make(213, 97, 153)), 255, 255, 255), "white on pink");
	CHECK(same(theme_color_on(lv_color_make(255, 255, 200)), 0, 0, 0), "black on pale yellow");
	CHECK(same(theme_color_on(lv_color_make(10, 10, 40)), 255, 255, 255), "white on near-black blue");

	// 6. Nearest preset follows the hue: an orange is orange, not the closest
	// in brightness; a grey has no hue to match.
	const lv_color_t presets[] = {
		{.red = 53, .green = 132, .blue = 228}, {.red = 255, .green = 120, .blue = 0}, {.red = 229, .green = 165, .blue = 10}, {.red = 46, .green = 194, .blue = 126}, {.red = 145, .green = 65, .blue = 172}, {.red = 213, .green = 97, .blue = 153},
	};
	for (int i = 0; i < 6; i++) {
		if (theme_color_nearest(presets, 6, presets[i]) != i) {
			printf("preset %d did not map to itself\n", i);
			exit(1);
		}
	}
	CHECK(theme_color_nearest(presets, 6, theme_color_hsv(30, 255, 255)) == 1, "a warm orange lands on orange");
	CHECK(theme_color_nearest(presets, 6, theme_color_hsv(300, 255, 255)) == 4, "a violet lands on purple");
	CHECK(theme_color_nearest(NULL, 0, red) == -1, "no presets has no nearest");
	CHECK(theme_color_saturation(lv_color_make(100, 100, 100)) == 0, "grey has no saturation");
	CHECK(theme_color_saturation(lv_color_make(255, 0, 0)) == 255, "pure red is fully saturated");

	// 7. The config value is accepted only inside 0x000000..0xFFFFFF.
	lv_color_t parsed = lv_color_make(1, 2, 3);
	CHECK(theme_color_parse_rgb(0x3584e4, &parsed) && same(parsed, 0x35, 0x84, 0xe4), "0x3584e4 round trips");
	CHECK(!theme_color_parse_rgb(-1, &parsed), "negative is rejected");
	CHECK(!theme_color_parse_rgb(0x1000000, &parsed), "above 24 bits is rejected");
	CHECK(!theme_color_parse_rgb(0x3584e4, NULL), "a null output is rejected");

	return 0;
}
