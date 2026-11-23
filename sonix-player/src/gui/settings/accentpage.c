#include "accentpage.h"

#include <stdint.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/settings/appearance.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/theme_color.h"
#include "src/system/core/lang.h"

lv_obj_t *accentpage_screen;

// The preview circle, large enough to judge a colour by, and the card it sits
// in. The three slider cards are shorter than the ordinary settings slider
// cards: this page has four controls, and it must not need scrolling on the
// shorter of the two panels.
#define PREVIEW_SIZE 100
#define PREVIEW_CARD_H 112
#define SLIDER_CARD_H 90
#define TRACK_HEIGHT 18

// One descriptor per strip: the gradient a slider draws is a pointer into this
// storage, so it has to live as long as the page.
static lv_grad_dsc_t hue_grad;
static lv_grad_dsc_t sat_grad;
static lv_grad_dsc_t bright_grad;

static lv_style_t hue_main_style;
static lv_style_t sat_main_style;
static lv_style_t bright_main_style;

static lv_obj_t *preview_swatch;
static lv_obj_t *preview_label;
static lv_obj_t *hue_slider;
static lv_obj_t *sat_slider;
static lv_obj_t *bright_slider;
static lv_obj_t *hue_value;
static lv_obj_t *sat_value;
static lv_obj_t *bright_value;
static lv_obj_t *ok_label;

// Set by OK before the screen is left, so the unload handler does not put the
// persisted accent back over the one just committed.
static bool committed;

// Whether a finger has moved a slider since the page opened. OK on an
// untouched picker is not a change: the preset stays a preset, and no config
// or boot-logo write happens.
static bool touched;

static int clamp_int(int value, int low, int high) {
	if (value < low) {
		return low;
	}
	if (value > high) {
		return high;
	}
	return value;
}

// The sliders work in whole degrees and percent; the theme API takes 0..255
// channels, as the colour maths does.
static uint8_t percent_to_channel(int percent) { return (uint8_t)((percent * 255 + 50) / 100); }

// The live colour the three sliders describe. Pure maths, so the preview circle
// can follow a drag without touching the palette.
static lv_color_t picked_color(void) {
	int hue = (int)lv_slider_get_value(hue_slider);
	int sat = (int)lv_slider_get_value(sat_slider);
	int bright = (int)lv_slider_get_value(bright_slider);
	return theme_color_hsv((uint16_t)hue, percent_to_channel(sat), percent_to_channel(bright));
}

// Seeds the sliders from an RGB colour: the hue of a preset is a much better
// starting point than red at half brightness.
static void rgb_to_hsv(lv_color_t c, int *hue, int *sat, int *bright) {
	int max = c.red > c.green ? c.red : c.green;
	if (c.blue > max) {
		max = c.blue;
	}
	int min = c.red < c.green ? c.red : c.green;
	if (c.blue < min) {
		min = c.blue;
	}

	int span = max - min;
	int h = 0;
	if (span > 0) {
		if (max == c.red) {
			h = (60 * ((int)c.green - (int)c.blue)) / span;
		} else if (max == c.green) {
			h = 120 + (60 * ((int)c.blue - (int)c.red)) / span;
		} else {
			h = 240 + (60 * ((int)c.red - (int)c.green)) / span;
		}
		if (h < 0) {
			h += 360;
		}
	}

	*hue = clamp_int(h, 0, 359);
	*sat = max > 0 ? clamp_int(span * 100 / max, 0, 100) : 0;
	*bright = clamp_int(max * 100 / 255, 20, 100);
}

// Rebuilds the two strips whose colours depend on another channel. The hue
// strip never changes: it is the whole wheel, whichever colour is chosen.
static void update_tracks(int hue, int sat, int bright) {
	uint8_t value = percent_to_channel(bright);

	sat_grad.stops[0].color = theme_color_hsv((uint16_t)hue, 0, value);
	sat_grad.stops[1].color = theme_color_hsv((uint16_t)hue, 255, value);

	bright_grad.stops[0].color = lv_color_black();
	bright_grad.stops[1].color = theme_color_hsv((uint16_t)hue, percent_to_channel(sat), 255);
	bright_grad.stops[2].color = lv_color_white();

	// The descriptors are read when the slider is drawn, so the pixels follow
	// the new stops once the object is invalidated.
	lv_obj_invalidate(sat_slider);
	lv_obj_invalidate(bright_slider);
}

static void update_ui(void) {
	int hue = (int)lv_slider_get_value(hue_slider);
	int sat = (int)lv_slider_get_value(sat_slider);
	int bright = (int)lv_slider_get_value(bright_slider);

	lv_label_set_text_fmt(hue_value, "%d", hue);
	lv_label_set_text_fmt(sat_value, "%d", sat);
	lv_label_set_text_fmt(bright_value, "%d", bright);

	lv_color_t c = theme_color_hsv((uint16_t)hue, percent_to_channel(sat), percent_to_channel(bright));
	lv_obj_set_style_bg_color(preview_swatch, c, 0);
	// The contrast before committing: the same choice theme_on_accent() makes
	// for every accent-filled control.
	lv_obj_set_style_text_color(preview_label, theme_color_on(c), 0);

	update_tracks(hue, sat, bright);
}

static void slider_changed_cb(lv_event_t *e) {
	(void)e;
	// Labels and the preview only. The palette is expensive to rebuild and a
	// drag never needs it at touch rate; see slider_released_cb().
	touched = true;
	update_ui();
}

static void slider_released_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	touched = true;
	update_ui();
	// The whole interface follows the colour now, without saving it or touching
	// the boot-logo marker.
	theme_preview_accent(picked_color());
}

static void ok_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	if (!touched) {
		// Nothing moved. A preview may still have left another accent live, so
		// put the persisted one back; then leave without writing anything.
		theme_restore_accent();
		committed = true;
		switch_screen_return_to(appearance_screen);
		return;
	}

	committed = true;
	theme_set_accent_custom(picked_color());
	switch_screen_return_to(appearance_screen);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	committed = false;

	touched = false;
	int hue, sat, bright;
	rgb_to_hsv(theme_accent_color(), &hue, &sat, &bright);
	lv_slider_set_value(hue_slider, hue, LV_ANIM_OFF);
	lv_slider_set_value(sat_slider, sat, LV_ANIM_OFF);
	lv_slider_set_value(bright_slider, bright, LV_ANIM_OFF);
	update_ui();
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (!committed) {
		// Back, a swipe, or anything else that is not OK: the preview was never
		// a choice.
		theme_restore_accent();
	}
	committed = false;
}

// The knobs and the OK label are painted by hand, so a theme or accent change
// has to be told about them.
static void refresh_theme(void) {
	theme_apply_slider_knob(hue_slider);
	theme_apply_slider_knob(sat_slider);
	theme_apply_slider_knob(bright_slider);
	if (ok_label) {
		lv_obj_set_style_text_color(ok_label, theme_on_accent(), 0);
	}
}

static lv_obj_t *make_slider_card(lv_obj_t *parent, const char *tag, int low, int high, lv_style_t *main_style, lv_obj_t **value_out, lv_obj_t **slider_out) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, SLIDER_CARD_H);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_hor(card, 20, 0);
	lv_obj_set_style_pad_ver(card, 8, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr(tag));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);
	lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

	lv_obj_t *value = lv_label_create(card);
	lv_label_set_text(value, "");
	lv_obj_add_style(value, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(value, &font_ui_24, 0);
	lv_obj_align(value, LV_ALIGN_TOP_RIGHT, 0, 0);

	lv_obj_t *slider = lv_slider_create(card);
	lv_obj_set_width(slider, lv_pct(100));
	lv_obj_set_height(slider, TRACK_HEIGHT);
	lv_slider_set_range(slider, low, high);
	lv_slider_set_mode(slider, LV_SLIDER_MODE_NORMAL);

	// The strip itself is the whole track: the accent fill a normal slider
	// draws over the left of it would hide half the scale.
	lv_obj_add_style(slider, main_style, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
	lv_obj_set_style_border_width(slider, 0, LV_PART_INDICATOR);

	lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
	lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
	lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(slider, 8, LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(slider, LV_OPA_30, LV_PART_KNOB);
	lv_obj_set_style_shadow_color(slider, lv_color_black(), LV_PART_KNOB);
	lv_obj_set_style_shadow_offset_y(slider, 1, LV_PART_KNOB);
	theme_apply_slider_knob(slider);

	lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -8);
	// A finger is wider than an 18 px strip; the extra area is what makes the
	// control usable without making the track a slab.
	lv_obj_set_ext_click_area(slider, 16);

	lv_obj_add_event_cb(slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(slider, slider_released_cb, LV_EVENT_RELEASED, NULL);

	if (value_out) {
		*value_out = value;
	}
	if (slider_out) {
		*slider_out = slider;
	}
	return card;
}

static void build_hue_gradient(void) {
	// Eight sectors plus the wrap back to red: 0, 45, ... 315, 359.
	static const uint8_t FRACS[9] = {0, 32, 64, 96, 128, 160, 192, 224, 255};
	lv_color_t colors[9];
	for (int i = 0; i < 8; i++) {
		colors[i] = theme_color_hsv((uint16_t)(i * 45), 255, 255);
	}
	colors[8] = theme_color_hsv(359, 255, 255);

	lv_grad_horizontal_init(&hue_grad);
	lv_grad_init_stops(&hue_grad, colors, NULL, FRACS, 9);
}

static void build_strip_styles(void) {
	build_hue_gradient();

	static const uint8_t SAT_FRACS[2] = {0, 255};
	lv_color_t sat_colors[2] = {lv_color_black(), lv_color_black()};
	lv_grad_horizontal_init(&sat_grad);
	lv_grad_init_stops(&sat_grad, sat_colors, NULL, SAT_FRACS, 2);

	static const uint8_t BRIGHT_FRACS[3] = {0, 128, 255};
	lv_color_t bright_colors[3] = {lv_color_black(), lv_color_black(), lv_color_white()};
	lv_grad_horizontal_init(&bright_grad);
	lv_grad_init_stops(&bright_grad, bright_colors, NULL, BRIGHT_FRACS, 3);

	lv_style_init(&hue_main_style);
	lv_style_set_bg_opa(&hue_main_style, LV_OPA_COVER);
	lv_style_set_radius(&hue_main_style, LV_RADIUS_CIRCLE);
	lv_style_set_border_width(&hue_main_style, 0);
	lv_style_set_bg_grad(&hue_main_style, &hue_grad);

	lv_style_init(&sat_main_style);
	lv_style_set_bg_opa(&sat_main_style, LV_OPA_COVER);
	lv_style_set_radius(&sat_main_style, LV_RADIUS_CIRCLE);
	lv_style_set_border_width(&sat_main_style, 0);
	lv_style_set_bg_grad(&sat_main_style, &sat_grad);

	lv_style_init(&bright_main_style);
	lv_style_set_bg_opa(&bright_main_style, LV_OPA_COVER);
	lv_style_set_radius(&bright_main_style, LV_RADIUS_CIRCLE);
	lv_style_set_border_width(&bright_main_style, 0);
	lv_style_set_bg_grad(&bright_main_style, &bright_grad);
}

void accentpage_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(accentpage_screen, cfg, "appearance_custom_accent");

	build_strip_styles();

	// The preview: the chosen colour, and the foreground the interface would
	// put on it, so a colour that makes white text vanish is obvious here
	// rather than after OK.
	lv_obj_t *preview_card = lv_obj_create(container);
	lv_obj_set_width(preview_card, lv_pct(100));
	lv_obj_set_height(preview_card, PREVIEW_CARD_H);
	lv_obj_add_style(preview_card, &theme_style_card, 0);
	lv_obj_set_style_radius(preview_card, 12, 0);
	lv_obj_set_style_border_width(preview_card, 0, 0);
	lv_obj_set_style_shadow_width(preview_card, 0, 0);
	lv_obj_set_scrollable(preview_card, false);
	lv_obj_set_event_bubble(preview_card, true);

	preview_swatch = lv_obj_create(preview_card);
	lv_obj_set_size(preview_swatch, PREVIEW_SIZE, PREVIEW_SIZE);
	lv_obj_set_style_radius(preview_swatch, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(preview_swatch, 0, 0);
	lv_obj_set_style_shadow_width(preview_swatch, 0, 0);
	lv_obj_set_style_bg_color(preview_swatch, theme_accent_color(), 0);
	lv_obj_set_style_bg_opa(preview_swatch, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(preview_swatch, false);
	lv_obj_center(preview_swatch);

	preview_label = lv_label_create(preview_swatch);
	lv_label_set_text(preview_label, "Aa");
	lv_obj_set_style_text_font(preview_label, &font_ui_32, 0);
	lv_obj_set_style_text_color(preview_label, theme_on_accent(), 0);
	lv_obj_center(preview_label);

	lv_obj_t *note = lv_label_create(container);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("appearance_custom_accent_note"));

	make_slider_card(container, "appearance_hue", 0, 359, &hue_main_style, &hue_value, &hue_slider);
	make_slider_card(container, "appearance_saturation", 0, 100, &sat_main_style, &sat_value, &sat_slider);
	make_slider_card(container, "appearance_brightness", 20, 100, &bright_main_style, &bright_value, &bright_slider);

	lv_obj_t *actions = lv_obj_create(container);
	lv_obj_set_size(actions, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(actions, 0, 0);
	lv_obj_set_style_border_width(actions, 0, 0);
	lv_obj_set_style_pad_all(actions, 0, 0);
	lv_obj_set_scrollable(actions, false);
	lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *ok = settingsrow_pill(actions, "ok", 0, ok_cb);
	lv_obj_add_style(ok, &theme_style_accent_bg, 0);
	lv_obj_add_style(ok, &theme_style_card_pressed, LV_STATE_PRESSED);
	ok_label = lv_obj_get_child(ok, 0);
	lv_obj_set_style_text_color(ok_label, theme_on_accent(), 0);

	lv_obj_add_event_cb(accentpage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(accentpage_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	theme_register_refresh(refresh_theme);
}
