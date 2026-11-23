#include "theme_color.h"

#include <math.h>

// A 0..255 channel as a 0..1 fraction. The conversion is sRGB's own, not a
// plain divide: two colours at the same brightness do not look equally bright,
// which is the whole reason a custom accent needs a foreground chosen for it.
static float linear_channel(uint8_t channel) {
	float c = (float)channel / 255.0f;
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float relative_luminance(lv_color_t c) { return 0.2126f * linear_channel(c.red) + 0.7152f * linear_channel(c.green) + 0.0722f * linear_channel(c.blue); }

lv_color_t theme_color_hsv(uint16_t hue, uint8_t sat, uint8_t val) {
	if (sat == 0) {
		return lv_color_make(val, val, val);
	}

	uint32_t h = (uint32_t)(hue % 360u);
	uint32_t region = h / 60u;
	// Where in the sector the hue sits, on 0..255. The +30 rounds to the
	// nearest step instead of truncating, so 359 is the near-red the caller
	// asked for and not the one step before it.
	uint32_t f = ((h % 60u) * 255u + 30u) / 60u;
	uint32_t s = sat;
	uint32_t v = val;

	uint32_t p = v * (255u - s) / 255u;
	uint32_t q = v * (255u - s * f / 255u) / 255u;
	uint32_t t = v * (255u - s * (255u - f) / 255u) / 255u;

	switch (region) {
	case 0:
		return lv_color_make((uint8_t)v, (uint8_t)t, (uint8_t)p);
	case 1:
		return lv_color_make((uint8_t)q, (uint8_t)v, (uint8_t)p);
	case 2:
		return lv_color_make((uint8_t)p, (uint8_t)v, (uint8_t)t);
	case 3:
		return lv_color_make((uint8_t)p, (uint8_t)q, (uint8_t)v);
	case 4:
		return lv_color_make((uint8_t)t, (uint8_t)p, (uint8_t)v);
	default:
		return lv_color_make((uint8_t)v, (uint8_t)p, (uint8_t)q);
	}
}

// The hue of a colour, 0..359, and its saturation. A grey has no hue; it is
// reported as 0, and callers that care about that check the saturation.
static uint16_t hue_of(lv_color_t c, uint8_t *sat_out) {
	uint8_t max = c.red > c.green ? c.red : c.green;
	if (c.blue > max) {
		max = c.blue;
	}
	uint8_t min = c.red < c.green ? c.red : c.green;
	if (c.blue < min) {
		min = c.blue;
	}

	uint8_t span = (uint8_t)(max - min);
	if (sat_out) {
		*sat_out = max ? (uint8_t)((uint32_t)span * 255u / max) : 0;
	}
	if (span == 0) {
		return 0;
	}

	// Scaled by 60 and unwrapped, then folded back into 0..359. Integer
	// arithmetic keeps a 480x720 device off its soft float for a value that is
	// only ever an index into six presets.
	int32_t h;
	if (max == c.red) {
		h = ((int32_t)c.green - c.blue) * 60 / span;
	} else if (max == c.green) {
		h = 120 + ((int32_t)c.blue - c.red) * 60 / span;
	} else {
		h = 240 + ((int32_t)c.red - c.green) * 60 / span;
	}
	h %= 360;
	if (h < 0) {
		h += 360;
	}
	return (uint16_t)h;
}

uint8_t theme_color_saturation(lv_color_t c) {
	uint8_t sat = 0;
	hue_of(c, &sat);
	return sat;
}

uint8_t theme_color_luminance(lv_color_t c) {
	int value = (int)(relative_luminance(c) * 255.0f + 0.5f);
	if (value < 0) {
		value = 0;
	}
	if (value > 255) {
		value = 255;
	}
	return (uint8_t)value;
}

lv_color_t theme_color_on(lv_color_t background) {
	float l = relative_luminance(background);
	// White stays the default: it is the foreground the six shipped accents
	// were designed around. Past this point white would fall under 3:1 -- the
	// orange, the yellows, the greens -- so the foreground flips to near-black.
	return l < 0.30f ? lv_color_white() : lv_color_black();
}

int theme_color_nearest(const lv_color_t *presets, int count, lv_color_t c) {
	if (!presets || count <= 0) {
		return -1;
	}

	uint16_t hue = hue_of(c, NULL);
	int best = 0;
	int best_distance = 360;
	for (int i = 0; i < count; i++) {
		int distance = hue;
		distance -= hue_of(presets[i], NULL);
		if (distance < 0) {
			distance = -distance;
		}
		if (distance > 180) {
			distance = 360 - distance;
		}
		if (distance < best_distance) {
			best_distance = distance;
			best = i;
		}
	}
	return best;
}

bool theme_color_parse_rgb(long value, lv_color_t *out) {
	if (!out || value < 0 || value > 0xFFFFFFL) {
		return false;
	}
	*out = lv_color_make((uint8_t)((value >> 16) & 0xFF), (uint8_t)((value >> 8) & 0xFF), (uint8_t)(value & 0xFF));
	return true;
}
