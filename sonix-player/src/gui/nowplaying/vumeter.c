#include "vumeter.h"

#include <math.h>

#include "src/system/audio/levelmeter.h"

// How often the meter is moved.
#define FRAME_MS 40
// The timer may not have run for a while (a popup, a slow frame); the meter
// then moves as if one long frame had passed, not all of it.
#define MAX_STEP_MS 200

// The scale: empty at this many decibels under full scale.
#define FLOOR_DB (-48.0f)
// The RMS of a full-scale sine is 3 dB under its peak; read that much up, a
// sine at full scale fills the bar.
#define RMS_LIFT_DB 3.0103f
// A mechanical needle takes about 300 ms to settle, the same both ways. On a
// decibel scale that fall reads as twitchy, so a level rises at that speed and
// falls more slowly.
#define RISE_TAU_MS 60.0f
#define FALL_TAU_MS 190.0f
// A mark stays where the loudest sample put it for this long, then falls the
// whole scale in PEAK_FALL_MS.
#define PEAK_HOLD_MS 800
#define PEAK_FALL_MS 1400.0f

// The spectrum. A band holds a tenth or so of the energy of the whole, so its
// scale sits lower than the level's, and it moves faster: it is watched for
// rhythm, not read for level.
#define BAND_FLOOR_DB (-54.0f)
#define BAND_RISE_TAU_MS 35.0f
#define BAND_FALL_TAU_MS 220.0f

// The pill of the bars.
#define BARS_H 30
#define BARS_PAD_HOR 14
#define BAR_H 5
#define BAR_GAP 4
#define TICK_W 3

// The panel of the dials and of the spectrum.
#define TALL_H 112
#define TALL_RADIUS 16
#define TALL_PAD_HOR 12
#define TALL_PAD_VER 8

// A dial: a quarter-circle fan, the needle 45 degrees left of upright when
// empty and 45 right when full, like the dial it is a picture of.
#define DIAL_GAP 12
#define DIAL_SWEEP 90
#define DIAL_START 225 // LVGL counts from three o'clock, clockwise: 270 is up
#define DIAL_ARC_W 5
#define DIAL_MARK_DEG 4
#define DIAL_HUB 10
#define NEEDLE_W 3

#define BAND_GAP 6
#define BAND_CAP_H 3

typedef struct {
	float level; // 0 at the floor to 1 at full scale
	float peak;
	int hold_ms;
} moving_t;

typedef struct {
	lv_obj_t *track, *fill, *tick;
	int fill_px, tick_px; // as drawn, -1 for not yet
} bar_t;

typedef struct {
	lv_obj_t *area;	 // the fan's own box: what sticks out of it is not drawn
	lv_obj_t *arc;	 // the scale, and the level along it
	lv_obj_t *mark;	 // the held peak, a short stretch of the same arc
	lv_obj_t *needle;
	lv_obj_t *hub;
	lv_point_precise_t points[2];
	int cx, cy, reach; // the pivot and the needle's length, in the area
	int level_deg, mark_deg; // as drawn, -1 for not yet
} dial_t;

typedef struct {
	lv_obj_t *track, *fill, *cap;
	int fill_px, cap_px;
} column_t;

static lv_obj_t *box;
static lv_obj_t *bars_group, *dials_group, *spectrum_group;
static bar_t bars[2];
static dial_t dials[2];
static column_t columns[LEVELMETER_BANDS];
static moving_t channels[2];
static moving_t band_state[LEVELMETER_BANDS];

static lv_timer_t *frame_timer;
static uint32_t last_frame;
static vumeter_style_t style = VUMETER_BARS;
static bool shown;
static lv_color_t accent;

// Where the player put it.
static int place_x, place_bottom, place_w;
static int bar_w;  // the length of a bar
static int col_h;  // the height of a column of the spectrum

// ---------------------------------------------------------------------------
// The scale and the way a figure moves along it
// ---------------------------------------------------------------------------

static float along(float db, float floor_db) {
	float at = (db - floor_db) / -floor_db;
	return at < 0.0f ? 0.0f : (at > 1.0f ? 1.0f : at);
}

static float square_db(float mean_square) { return mean_square > 0.0f ? 10.0f * log10f(mean_square) : -200.0f; }
static float sample_db(float sample) { return sample > 0.0f ? 20.0f * log10f(sample) : -200.0f; }

// `rise` and `fall` are the share of the way gone in this frame, either way.
static void step(moving_t *m, float body, float top, float rise, float fall, int step_ms) {
	m->level += (body - m->level) * (body > m->level ? rise : fall);

	if (top >= m->peak) {
		m->peak = top;
		m->hold_ms = PEAK_HOLD_MS;
	} else if (m->hold_ms > 0) {
		m->hold_ms -= step_ms;
	} else {
		m->peak -= (float)step_ms / PEAK_FALL_MS;
		if (m->peak < top) {
			m->peak = top;
		}
	}
	// The mark is never behind the level it belongs to.
	if (m->peak < m->level) {
		m->peak = m->level;
	}
}

// ---------------------------------------------------------------------------
// Drawing. Only what moved by a pixel or a degree is touched: a meter standing
// still costs nothing.
// ---------------------------------------------------------------------------

static void draw_bar(bar_t *b, const moving_t *m) {
	int fill = (int)(m->level * (float)bar_w + 0.5f);
	int tick = (int)(m->peak * (float)bar_w + 0.5f);
	if (fill != b->fill_px) {
		b->fill_px = fill;
		lv_obj_set_hidden(b->fill, fill <= 0);
		if (fill > 0) {
			lv_obj_set_width(b->fill, fill);
		}
	}
	if (tick != b->tick_px) {
		b->tick_px = tick;
		lv_obj_set_hidden(b->tick, tick <= 0);
		if (tick > 0) {
			lv_obj_set_x(b->tick, tick < TICK_W ? 0 : tick - TICK_W);
		}
	}
}

static void draw_dial(dial_t *d, const moving_t *m) {
	int deg = (int)(m->level * (float)DIAL_SWEEP + 0.5f);
	if (deg != d->level_deg) {
		d->level_deg = deg;
		lv_arc_set_value(d->arc, deg);

		// A line from the pivot, not an object turned about it: a turned object
		// is drawn through a layer of its own on every new angle.
		float turn = ((float)deg - (float)DIAL_SWEEP / 2.0f) * (3.14159265f / 180.0f);
		d->points[0].x = d->cx;
		d->points[0].y = d->cy;
		d->points[1].x = d->cx + (lv_value_precise_t)((float)d->reach * sinf(turn));
		d->points[1].y = d->cy - (lv_value_precise_t)((float)d->reach * cosf(turn));
		lv_line_set_points(d->needle, d->points, 2);
	}

	int mark = (int)(m->peak * (float)DIAL_SWEEP + 0.5f);
	if (mark != d->mark_deg) {
		d->mark_deg = mark;
		// The stretch of the arc just under the held peak, and nothing at all
		// at the empty end.
		bool there = mark > DIAL_MARK_DEG / 2;
		lv_obj_set_hidden(d->mark, !there);
		if (there) {
			lv_arc_set_angles(d->mark, DIAL_START + (mark > DIAL_MARK_DEG ? mark - DIAL_MARK_DEG : 0), DIAL_START + mark);
		}
	}
}

static void draw_column(column_t *c, const moving_t *m) {
	int fill = (int)(m->level * (float)col_h + 0.5f);
	int cap = (int)(m->peak * (float)col_h + 0.5f);
	if (fill != c->fill_px) {
		c->fill_px = fill;
		lv_obj_set_hidden(c->fill, fill <= 0);
		if (fill > 0) {
			lv_obj_set_height(c->fill, fill);
			lv_obj_set_y(c->fill, col_h - fill);
		}
	}
	if (cap != c->cap_px) {
		c->cap_px = cap;
		lv_obj_set_hidden(c->cap, cap <= BAND_CAP_H);
		if (cap > BAND_CAP_H) {
			lv_obj_set_y(c->cap, col_h - cap);
		}
	}
}

static void draw_all(void) {
	if (style == VUMETER_BARS) {
		draw_bar(&bars[0], &channels[0]);
		draw_bar(&bars[1], &channels[1]);
	} else if (style == VUMETER_DIALS) {
		draw_dial(&dials[0], &channels[0]);
		draw_dial(&dials[1], &channels[1]);
	} else {
		for (int b = 0; b < LEVELMETER_BANDS; b++) {
			draw_column(&columns[b], &band_state[b]);
		}
	}
}

// Everything back at empty and drawn there.
static void rest(void) {
	for (int i = 0; i < 2; i++) {
		channels[i] = (moving_t){0};
	}
	for (int b = 0; b < LEVELMETER_BANDS; b++) {
		band_state[b] = (moving_t){0};
	}
	draw_all();
}

static void frame_cb(lv_timer_t *timer) {
	(void)timer;
	uint32_t now = lv_tick_get();
	int step_ms = (int)(now - last_frame);
	last_frame = now;
	if (step_ms < 1) {
		return;
	}
	if (step_ms > MAX_STEP_MS) {
		step_ms = MAX_STEP_MS;
	}

	// With no level -- paused, stopped, a DSD stream -- the readings are
	// nothing, and everything comes down to rest at its own pace.
	levelmeter_levels_t levels;
	levelmeter_read(&levels);

	if (style == VUMETER_SPECTRUM) {
		float rise = 1.0f - expf(-(float)step_ms / BAND_RISE_TAU_MS);
		float fall = 1.0f - expf(-(float)step_ms / BAND_FALL_TAU_MS);
		for (int b = 0; b < LEVELMETER_BANDS; b++) {
			float at = along(square_db(levels.band_mean_square[b]), BAND_FLOOR_DB);
			step(&band_state[b], at, at, rise, fall, step_ms);
		}
	} else {
		float rise = 1.0f - expf(-(float)step_ms / RISE_TAU_MS);
		float fall = 1.0f - expf(-(float)step_ms / FALL_TAU_MS);
		for (int i = 0; i < 2; i++) {
			float body = along(square_db(levels.mean_square[i]) + RMS_LIFT_DB, FLOOR_DB);
			float top = along(sample_db(levels.peak[i]), FLOOR_DB);
			step(&channels[i], body, top, rise, fall, step_ms);
		}
	}
	draw_all();
}

// ---------------------------------------------------------------------------
// Building and laying out
// ---------------------------------------------------------------------------

static lv_obj_t *plain(lv_obj_t *parent) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_scrollable(obj, false);
	// Nothing here takes a press: it lies on the sleeve, which is what the
	// player is dragged by, and the sleeve is told of a tap here by the player
	// (vumeter_hit).
	lv_obj_set_clickable(obj, false);
	return obj;
}

static lv_obj_t *dial_arc(lv_obj_t *parent, bool scale) {
	lv_obj_t *arc = lv_arc_create(parent);
	lv_obj_remove_style_all(arc); // the knob goes with its style
	lv_obj_set_clickable(arc, false);
	lv_arc_set_bg_angles(arc, DIAL_START, DIAL_START + DIAL_SWEEP);
	lv_arc_set_range(arc, 0, DIAL_SWEEP);
	lv_arc_set_value(arc, 0);
	lv_obj_set_style_arc_width(arc, DIAL_ARC_W, LV_PART_MAIN);
	lv_obj_set_style_arc_color(arc, lv_color_white(), LV_PART_MAIN);
	lv_obj_set_style_arc_opa(arc, scale ? LV_OPA_30 : LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_set_style_arc_width(arc, DIAL_ARC_W, LV_PART_INDICATOR);
	lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
	return arc;
}

static void build(lv_obj_t *parent) {
	box = plain(parent);
	lv_obj_set_ignore_layout(box, true);
	lv_obj_set_style_bg_color(box, lv_color_black(), 0);
	lv_obj_set_hidden(box, true);

	// The bars.
	bars_group = plain(box);
	for (int i = 0; i < 2; i++) {
		bar_t *b = &bars[i];
		b->track = plain(bars_group);
		lv_obj_set_pos(b->track, BARS_PAD_HOR, (BARS_H - 2 * BAR_H - BAR_GAP) / 2 + i * (BAR_H + BAR_GAP));
		lv_obj_set_height(b->track, BAR_H);
		lv_obj_set_style_radius(b->track, BAR_H / 2, 0);
		lv_obj_set_style_bg_color(b->track, lv_color_white(), 0);
		lv_obj_set_style_bg_opa(b->track, LV_OPA_20, 0);

		b->fill = plain(b->track);
		lv_obj_set_pos(b->fill, 0, 0);
		lv_obj_set_height(b->fill, BAR_H);
		lv_obj_set_style_radius(b->fill, BAR_H / 2, 0);
		lv_obj_set_style_bg_opa(b->fill, LV_OPA_COVER, 0);
		lv_obj_set_hidden(b->fill, true);

		b->tick = plain(b->track);
		lv_obj_set_size(b->tick, TICK_W, BAR_H);
		lv_obj_set_y(b->tick, 0);
		lv_obj_set_style_bg_color(b->tick, lv_color_white(), 0);
		lv_obj_set_style_bg_opa(b->tick, LV_OPA_COVER, 0);
		lv_obj_set_hidden(b->tick, true);
	}

	// The dials.
	dials_group = plain(box);
	for (int i = 0; i < 2; i++) {
		dial_t *d = &dials[i];
		d->area = plain(dials_group);
		d->arc = dial_arc(d->area, true);
		d->mark = dial_arc(d->area, false);
		lv_obj_set_hidden(d->mark, true);

		d->needle = lv_line_create(d->area);
		lv_obj_remove_style_all(d->needle);
		lv_obj_set_clickable(d->needle, false);
		lv_obj_set_pos(d->needle, 0, 0);
		lv_obj_set_style_line_width(d->needle, NEEDLE_W, 0);
		lv_obj_set_style_line_color(d->needle, lv_color_white(), 0);
		lv_obj_set_style_line_opa(d->needle, LV_OPA_COVER, 0);
		lv_obj_set_style_line_rounded(d->needle, true, 0);

		d->hub = plain(d->area);
		lv_obj_set_size(d->hub, DIAL_HUB, DIAL_HUB);
		lv_obj_set_style_radius(d->hub, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(d->hub, LV_OPA_COVER, 0);
	}

	// The spectrum.
	spectrum_group = plain(box);
	for (int b = 0; b < LEVELMETER_BANDS; b++) {
		column_t *c = &columns[b];
		c->track = plain(spectrum_group);
		lv_obj_set_style_radius(c->track, 3, 0);
		lv_obj_set_style_bg_color(c->track, lv_color_white(), 0);
		lv_obj_set_style_bg_opa(c->track, LV_OPA_10, 0);

		c->fill = plain(c->track);
		lv_obj_set_x(c->fill, 0);
		lv_obj_set_style_radius(c->fill, 3, 0);
		lv_obj_set_style_bg_opa(c->fill, LV_OPA_COVER, 0);
		lv_obj_set_hidden(c->fill, true);

		c->cap = plain(c->track);
		lv_obj_set_x(c->cap, 0);
		lv_obj_set_height(c->cap, BAND_CAP_H);
		lv_obj_set_style_bg_opa(c->cap, LV_OPA_COVER, 0);
		lv_obj_set_hidden(c->cap, true);
	}
}

// Sizes everything for the place and the shape it has now, and draws it there.
static void layout(void) {
	if (!box || place_w <= 0) {
		return;
	}
	bool tall = style != VUMETER_BARS;
	int h = tall ? TALL_H : BARS_H;
	lv_obj_set_size(box, place_w, h);
	lv_obj_set_pos(box, place_x, place_bottom - h);
	lv_obj_set_style_radius(box, tall ? TALL_RADIUS : LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(box, tall ? LV_OPA_60 : LV_OPA_50, 0);

	lv_obj_t *const groups[] = {bars_group, dials_group, spectrum_group};
	for (int i = 0; i < VUMETER_STYLES; i++) {
		lv_obj_set_pos(groups[i], 0, 0);
		lv_obj_set_size(groups[i], place_w, h);
		lv_obj_set_hidden(groups[i], i != (int)style);
	}

	if (style == VUMETER_BARS) {
		bar_w = place_w - 2 * BARS_PAD_HOR;
		if (bar_w < 1) {
			bar_w = 1;
		}
		for (int i = 0; i < 2; i++) {
			lv_obj_set_width(bars[i].track, bar_w);
			bars[i].fill_px = -1;
			bars[i].tick_px = -1;
		}
	} else if (style == VUMETER_DIALS) {
		int area_w = (place_w - 2 * TALL_PAD_HOR - DIAL_GAP) / 2;
		int area_h = TALL_H - 2 * TALL_PAD_VER;
		if (area_w < 40) {
			area_w = 40;
		}
		// The fan is a quarter circle: as wide as 1.42 of its radius. It takes
		// what the height allows, less when the dial is narrow.
		int radius = area_h - DIAL_HUB - 4;
		if (radius > (area_w - 8) * 100 / 142) {
			radius = (area_w - 8) * 100 / 142;
		}
		for (int i = 0; i < 2; i++) {
			dial_t *d = &dials[i];
			lv_obj_set_size(d->area, area_w, area_h);
			lv_obj_set_pos(d->area, TALL_PAD_HOR + i * (area_w + DIAL_GAP), TALL_PAD_VER);
			d->cx = area_w / 2;
			d->cy = area_h - DIAL_HUB / 2 - 1;
			d->reach = radius - DIAL_ARC_W - 4;
			lv_obj_t *const arcs[] = {d->arc, d->mark};
			for (int a = 0; a < 2; a++) {
				lv_obj_set_size(arcs[a], 2 * radius, 2 * radius);
				lv_obj_set_pos(arcs[a], d->cx - radius, d->cy - radius);
			}
			lv_obj_set_pos(d->hub, d->cx - DIAL_HUB / 2, d->cy - DIAL_HUB / 2);
			d->level_deg = -1;
			d->mark_deg = -1;
		}
	} else {
		int inner = place_w - 2 * TALL_PAD_HOR;
		int col_w = (inner - (LEVELMETER_BANDS - 1) * BAND_GAP) / LEVELMETER_BANDS;
		if (col_w < 2) {
			col_w = 2;
		}
		col_h = TALL_H - 2 * TALL_PAD_VER;
		// What the division left over goes to the two sides.
		int left = TALL_PAD_HOR + (inner - LEVELMETER_BANDS * col_w - (LEVELMETER_BANDS - 1) * BAND_GAP) / 2;
		for (int b = 0; b < LEVELMETER_BANDS; b++) {
			column_t *c = &columns[b];
			lv_obj_set_size(c->track, col_w, col_h);
			lv_obj_set_pos(c->track, left + b * (col_w + BAND_GAP), TALL_PAD_VER);
			lv_obj_set_width(c->fill, col_w);
			lv_obj_set_width(c->cap, col_w);
			c->fill_px = -1;
			c->cap_px = -1;
		}
	}
	draw_all();
}

static void paint(void) {
	if (!box) {
		return;
	}
	for (int i = 0; i < 2; i++) {
		lv_obj_set_style_bg_color(bars[i].fill, accent, 0);
		lv_obj_set_style_arc_color(dials[i].arc, accent, LV_PART_INDICATOR);
		lv_obj_set_style_arc_color(dials[i].mark, accent, LV_PART_INDICATOR);
		lv_obj_set_style_bg_color(dials[i].hub, accent, 0);
	}
	for (int b = 0; b < LEVELMETER_BANDS; b++) {
		lv_obj_set_style_bg_color(columns[b].fill, accent, 0);
		lv_obj_set_style_bg_color(columns[b].cap, accent, 0);
	}
}

// ---------------------------------------------------------------------------

void vumeter_create(lv_obj_t *parent) {
	if (box) {
		return;
	}
	accent = lv_color_white();
	build(parent);
	paint();
	frame_timer = lv_timer_create(frame_cb, FRAME_MS, NULL);
	lv_timer_pause(frame_timer);
}

void vumeter_place(lv_obj_t *parent, int x, int bottom, int w) {
	if (!box || !parent) {
		return;
	}
	if (lv_obj_get_parent(box) != parent) {
		lv_obj_set_parent(box, parent);
	}
	lv_obj_move_foreground(box);
	if (x == place_x && bottom == place_bottom && w == place_w) {
		return;
	}
	place_x = x;
	place_bottom = bottom;
	place_w = w;
	layout();
}

void vumeter_show(bool on) {
	if (!box || on == shown) {
		return;
	}
	shown = on;
	lv_obj_set_hidden(box, !on);
	// The measuring goes with the meter: nothing reads the levels while it is
	// away, so nothing works them out.
	levelmeter_set_wanted(on, style == VUMETER_SPECTRUM);
	if (on) {
		last_frame = lv_tick_get();
		lv_timer_resume(frame_timer);
	} else {
		lv_timer_pause(frame_timer);
		rest();
	}
}

void vumeter_set_color(lv_color_t body) {
	accent = body;
	paint();
}

void vumeter_set_style(vumeter_style_t wanted) {
	if (wanted < 0 || wanted >= VUMETER_STYLES) {
		wanted = VUMETER_BARS;
	}
	if (wanted == style) {
		return;
	}
	style = wanted;
	if (!box) {
		return;
	}
	// The new shape starts from empty: what the other was showing is not its
	// to carry on from (the spectrum's figures are not the channels').
	for (int i = 0; i < 2; i++) {
		channels[i] = (moving_t){0};
	}
	for (int b = 0; b < LEVELMETER_BANDS; b++) {
		band_state[b] = (moving_t){0};
	}
	layout();
	if (shown) {
		levelmeter_set_wanted(true, style == VUMETER_SPECTRUM);
	}
}

vumeter_style_t vumeter_style(void) { return style; }

bool vumeter_hit(const lv_point_t *point) {
	if (!box || !shown || !point) {
		return false;
	}
	lv_area_t area;
	lv_obj_get_coords(box, &area);
	return point->x >= area.x1 && point->x <= area.x2 && point->y >= area.y1 && point->y <= area.y2;
}
