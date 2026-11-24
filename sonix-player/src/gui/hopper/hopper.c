#include "hopper.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/volume_overlay.h"
#include "src/system/audio/gamesound.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

lv_obj_t *hopper_screen;

// ---------------------------------------------------------------------------
// the world
// ---------------------------------------------------------------------------
//
// The world is measured in panel pixels with y going up: the ground is at 0
// and a ledge at 5000 is five thousand pixels above it. `cam_y` is the height
// at the bottom of the screen; it only ever rises, following the creature, so
// what has gone off the bottom is gone, and those ledges are used again above.

#define TICK_MS 16
// A gap between two ticks this long means the panel was dark or the player was
// busy with something else: the world does not jump ahead by that much.
#define STALL_US 250000
#define STEP_MAX_US 40000

#define GRAVITY 2400.0f
// The speed a ledge sends the creature up at: enough for 230 pixels of height,
// which every gap between two rungs stays well under.
#define JUMP_V 1050.0f
#define SPRING_V 1600.0f
// Sideways: the speed a held half of the screen works up to, and how quickly.
#define STEER_V 430.0f
#define STEER_GAIN 11.0f

#define BODY_R 22
#define LEDGE_H 18
// How far past the end of a ledge the creature's middle may be and still land.
#define LEDGE_GRACE 14
#define LEDGES 20
// The creature is kept at or below this share of the screen's height.
#define CAM_LEAD 0.55f

#define CLOUDS 4
#define STARS 22
#define OVER_HOLD_US 600000
#define SQUASH_US 140000

typedef enum {
	LEDGE_PLAIN,
	LEDGE_SLIDING, // goes from side to side
	LEDGE_BRITTLE, // holds for one bounce and falls
	LEDGE_SPRING,  // sends the creature half as high again
} ledge_kind_t;

typedef struct {
	float x, y; // the left end, and the height of the top
	int w;
	ledge_kind_t kind;
	float vx, vy;
	bool broken;
} ledge_t;

static int panel_w, panel_h;

static lv_obj_t *cloud_obj[CLOUDS];
static lv_obj_t *star_obj[STARS];
static lv_obj_t *ledge_obj[LEDGES];
static lv_obj_t *body_obj;
static lv_obj_t *score_label, *best_label, *hint_label;
static lv_obj_t *over_box, *over_title, *over_hint;
static lv_timer_t *tick_timer;

static bool active;
static bool over;
static int64_t last_us, over_since_us;

static ledge_t ledge[LEDGES];
static float top_y; // the highest rung made so far
static float cam_y;

static float body_x, body_y; // the middle, and the feet
static float body_vx, body_vy;
static int steer;	// -1 left, 0 neither, 1 right
static int facing;	// the way it looks: the last way it was steered
static int64_t squash_left_us;
static float peak_y;
static int score, best;
static bool best_unsaved;
static bool steered; // since the game began: the hint goes at the first touch

static int cloud_base[CLOUDS], cloud_xs[CLOUDS];
static int star_base[STARS], star_xs[STARS];

static int64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// ---------------------------------------------------------------------------
// the ledges
// ---------------------------------------------------------------------------

// Makes `p` a new ledge above everything made so far.
//
// The rungs -- plain, sliding and spring ledges -- are the way up, and no gap
// between two of them is more than a bounce covers. A brittle ledge is never a
// rung: it is put between two as something extra, because once it has fallen
// the creature must still be able to go on from the rung below.
static void ledge_make(ledge_t *p) {
	int h = (int)top_y;
	int gap_min = 56 + h / 140;
	int gap_max = 84 + h / 50;
	if (gap_min > 120) {
		gap_min = 120;
	}
	if (gap_max > 186) {
		gap_max = 186;
	}
	int gap = gap_min + rand() % (gap_max - gap_min + 1);
	int roll = rand() % 100;

	p->w = 104 - h / 160;
	if (p->w < 62) {
		p->w = 62;
	}
	p->x = (float)(rand() % (panel_w - p->w));
	p->vx = p->vy = 0;
	p->broken = false;

	if (h > 500 && roll < 20) {
		p->kind = LEDGE_BRITTLE;
		// Clear of the rung below it and of the lowest place the next can be.
		p->y = top_y + 28;
		return;
	}
	top_y += (float)gap;
	p->y = top_y;
	if (h > 1400 && roll < 44) {
		p->kind = LEDGE_SLIDING;
		p->vx = (float)(70 + rand() % 90) * (rand() % 2 ? 1.0f : -1.0f);
	} else if (roll >= 93) {
		p->kind = LEDGE_SPRING;
	} else {
		p->kind = LEDGE_PLAIN;
	}
}

static void fill(lv_layer_t *layer, int x1, int y1, int x2, int y2, int radius, lv_color_t colour, lv_opa_t opa) {
	if (x2 < x1 || y2 < y1) {
		return;
	}
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	d.bg_color = colour;
	d.bg_opa = opa;
	d.radius = radius;
	lv_area_t a = {x1, y1, x2, y2};
	lv_draw_rect(layer, &d, &a);
}

// Draws the object as a ledge of the kind in its user data: a rounded bar with
// a darker underside and a light strip along the top, and its kind's mark.
static void ledge_draw_cb(lv_event_t *e) {
	static const uint32_t body[] = {0x58c26a, 0x4aa3ff, 0xd9a25a, 0xff6fa3};
	lv_obj_t *obj = lv_event_get_current_target(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	if (!layer) {
		return;
	}
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	ledge_kind_t kind = (ledge_kind_t)(intptr_t)lv_obj_get_user_data(obj);
	lv_color_t colour = lv_color_hex(body[kind]);
	lv_color_t dark = lv_color_darken(colour, 80);
	int mid = (a.x1 + a.x2) / 2;

	fill(layer, a.x1, a.y1, a.x2, a.y2, 9, dark, LV_OPA_COVER);
	fill(layer, a.x1, a.y1, a.x2, a.y2 - 5, 9, colour, LV_OPA_COVER);
	fill(layer, a.x1 + 8, a.y1 + 3, a.x2 - 8, a.y1 + 5, 1, lv_color_white(), LV_OPA_60);

	switch (kind) {
	case LEDGE_SLIDING: // an arrowhead's worth of dots at either end
		fill(layer, a.x1 + 8, a.y1 + 8, a.x1 + 11, a.y1 + 11, 2, lv_color_white(), LV_OPA_80);
		fill(layer, a.x2 - 11, a.y1 + 8, a.x2 - 8, a.y1 + 11, 2, lv_color_white(), LV_OPA_80);
		break;
	case LEDGE_BRITTLE: // cracks
		fill(layer, mid - 14, a.y1 + 1, mid - 12, a.y2 - 6, 0, dark, LV_OPA_COVER);
		fill(layer, mid + 9, a.y1 + 5, mid + 11, a.y2 - 2, 0, dark, LV_OPA_COVER);
		break;
	case LEDGE_SPRING: // a pad in the middle
		fill(layer, mid - 16, a.y1 + 7, mid + 16, a.y1 + 11, 2, lv_color_white(), LV_OPA_90);
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// the creature
// ---------------------------------------------------------------------------

// Draws the creature into its object's box: a round body, lighter below, two
// eyes that look the way it is going, and two feet. Just after a bounce the
// box is lower and wider than it is tall, and the body with it.
static void body_draw_cb(lv_event_t *e) {
	lv_obj_t *obj = lv_event_get_current_target(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	if (!layer) {
		return;
	}
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
	int cx = a.x1 + w / 2;
	int foot_h = 7;

	fill(layer, cx - 15, a.y2 - foot_h, cx - 4, a.y2, 4, lv_color_hex(0x8a3a2c), LV_OPA_COVER);
	fill(layer, cx + 4, a.y2 - foot_h, cx + 15, a.y2, 4, lv_color_hex(0x8a3a2c), LV_OPA_COVER);

	int by2 = a.y2 - foot_h + 3;
	fill(layer, a.x1, a.y1, a.x2, by2, LV_RADIUS_CIRCLE, lv_color_hex(0xff7a59), LV_OPA_COVER);
	fill(layer, a.x1 + w / 5, a.y1 + (by2 - a.y1) * 11 / 20, a.x2 - w / 5, by2 - 3, LV_RADIUS_CIRCLE, lv_color_hex(0xffc3a6), LV_OPA_COVER);
	fill(layer, a.x1 + w / 4, a.y1 + 4, a.x1 + w / 2, a.y1 + 8, 3, lv_color_white(), LV_OPA_50);

	int ey = a.y1 + (h - foot_h) * 3 / 10;
	int look = facing * 3;
	for (int side = -1; side <= 1; side += 2) {
		int ex = cx + side * 9 + facing * 2;
		fill(layer, ex - 6, ey - 6, ex + 6, ey + 6, LV_RADIUS_CIRCLE, lv_color_white(), LV_OPA_COVER);
		fill(layer, ex - 3 + look, ey - 2, ex + 3 + look, ey + 4, LV_RADIUS_CIRCLE, lv_color_hex(0x2a1a3a), LV_OPA_COVER);
	}
}

// ---------------------------------------------------------------------------
// the game
// ---------------------------------------------------------------------------

static void show_score(void) {
	lv_label_set_text_fmt(score_label, "%d", score);
	lv_label_set_text_fmt(best_label, tr("tower_best"), best);
}

static void new_game(void) {
	over = false;
	cam_y = 0;
	top_y = 40;
	// The first ledge is the ground: all the way across, so the game cannot
	// be lost before a finger has touched the screen.
	ledge[0] = (ledge_t){.x = 0, .y = 40, .w = panel_w, .kind = LEDGE_PLAIN};
	for (int i = 1; i < LEDGES; i++) {
		ledge_make(&ledge[i]);
	}
	body_x = (float)panel_w / 2;
	body_y = 40;
	body_vx = 0;
	body_vy = JUMP_V;
	steer = 0;
	facing = 0;
	squash_left_us = 0;
	peak_y = body_y;
	score = 0;
	steered = false;

	show_score();
	lv_obj_set_hidden(over_box, true);
	lv_obj_set_hidden(hint_label, false);
}

static void game_over(void) {
	over = true;
	over_since_us = now_us();
	if (score > best) {
		best = score;
		config_set_int("hopper", "best", best);
		best_unsaved = true; // written out when the game is left
	}
	show_score();
	gamesound_play(GAMESOUND_OVER);
	lv_obj_set_hidden(hint_label, true);
	lv_obj_set_hidden(over_box, false);
}

static void step(int64_t us) {
	float dt = (float)us / 1e6f;

	for (int i = 0; i < LEDGES; i++) {
		ledge_t *p = &ledge[i];
		if (p->broken) {
			p->vy -= GRAVITY * dt;
			p->y += p->vy * dt;
		} else if (p->kind == LEDGE_SLIDING) {
			p->x += p->vx * dt;
			if (p->x < 0) {
				p->x = 0;
				p->vx = -p->vx;
			} else if (p->x > (float)(panel_w - p->w)) {
				p->x = (float)(panel_w - p->w);
				p->vx = -p->vx;
			}
		}
		// Off the bottom: it goes round to the top.
		if (!over && p->y < cam_y - 80) {
			ledge_make(p);
		}
	}
	if (over) {
		return;
	}

	float gain = STEER_GAIN * dt;
	body_vx += ((float)steer * STEER_V - body_vx) * (gain > 1.0f ? 1.0f : gain);
	body_x += body_vx * dt;
	if (body_x < -(float)BODY_R) {
		body_x += (float)(panel_w + 2 * BODY_R);
	} else if (body_x > (float)(panel_w + BODY_R)) {
		body_x -= (float)(panel_w + 2 * BODY_R);
	}

	float was_y = body_y;
	body_vy -= GRAVITY * dt;
	body_y += body_vy * dt;
	if (body_vy < 0) {
		// Coming down: the first ledge whose top the feet went through.
		for (int i = 0; i < LEDGES; i++) {
			ledge_t *p = &ledge[i];
			if (p->broken || was_y < p->y || body_y > p->y) {
				continue;
			}
			// The ground runs right across, also under a creature that is half
			// off one side and about to come back on the other.
			if (p->w < panel_w && (body_x < p->x - LEDGE_GRACE || body_x > p->x + (float)p->w + LEDGE_GRACE)) {
				continue;
			}
			body_y = p->y;
			body_vy = p->kind == LEDGE_SPRING ? SPRING_V : JUMP_V;
			squash_left_us = SQUASH_US;
			if (p->kind == LEDGE_BRITTLE) {
				p->broken = true;
				p->vy = 0;
			}
			gamesound_play(p->kind == LEDGE_SPRING ? GAMESOUND_SPRING : p->kind == LEDGE_BRITTLE ? GAMESOUND_BREAK : GAMESOUND_BOUNCE);
			break;
		}
	}
	if (squash_left_us > 0) {
		squash_left_us = squash_left_us > us ? squash_left_us - us : 0;
	}

	if (body_y > peak_y) {
		peak_y = body_y;
		int s = (int)(peak_y - 40) / 10;
		if (s != score) {
			score = s;
			show_score();
		}
	}
	float lead = body_y - (float)panel_h * CAM_LEAD;
	if (lead > cam_y) {
		cam_y = lead;
	}
	if (body_y < cam_y - 2 * BODY_R) {
		game_over();
	}
}

// The colour of the sky at the height the camera is at: day at the ground,
// through a warm evening to night.
static lv_color_t sky_colour(void) {
	static const struct {
		int y;
		uint32_t colour;
	} stops[] = {{0, 0x7ecbff}, {3500, 0xffb38a}, {6500, 0x5a3d9a}, {10000, 0x0b1030}};
	int y = (int)cam_y;
	for (int i = 1; i < 4; i++) {
		if (y < stops[i].y) {
			int mix = (y - stops[i - 1].y) * 255 / (stops[i].y - stops[i - 1].y);
			return lv_color_mix(lv_color_hex(stops[i].colour), lv_color_hex(stops[i - 1].colour), (uint8_t)mix);
		}
	}
	return lv_color_hex(stops[3].colour);
}

// A value that falls from 255 to 0, or rises from 0 to 255, between two heights.
static int ramp(int y, int from, int to) {
	if (y <= from) {
		return 0;
	}
	return y >= to ? 255 : (y - from) * 255 / (to - from);
}

// Puts every object where the world has it.
static void render(void) {
	static int sky_at = -1;
	// The sky changes too slowly to be worth a new colour, and with it a
	// repaint of the whole screen, more than once in a while.
	if ((int)cam_y / 40 != sky_at) {
		sky_at = (int)cam_y / 40;
		lv_obj_set_style_bg_color(hopper_screen, sky_colour(), 0);
		int clouds = 230 - ramp((int)cam_y, 3000, 6500) * 230 / 255;
		int stars = ramp((int)cam_y, 4500, 8000);
		for (int i = 0; i < CLOUDS; i++) {
			lv_obj_set_style_opa(cloud_obj[i], (lv_opa_t)clouds, 0);
			lv_obj_set_hidden(cloud_obj[i], clouds == 0);
		}
		for (int i = 0; i < STARS; i++) {
			lv_obj_set_style_bg_opa(star_obj[i], (lv_opa_t)stars, 0);
			lv_obj_set_hidden(star_obj[i], stars == 0);
		}
	}

	// What is far away goes by more slowly: the clouds at a third of the
	// camera's pace and the stars at a tenth, each coming round again.
	int span = panel_h + 160;
	for (int i = 0; i < CLOUDS; i++) {
		int y = (cloud_base[i] + (int)(cam_y * 0.33f)) % span;
		lv_obj_set_pos(cloud_obj[i], cloud_xs[i], y - 120);
	}
	for (int i = 0; i < STARS; i++) {
		int y = (star_base[i] + (int)(cam_y * 0.10f)) % panel_h;
		lv_obj_set_pos(star_obj[i], star_xs[i], y);
	}

	for (int i = 0; i < LEDGES; i++) {
		const ledge_t *p = &ledge[i];
		int y = panel_h - (int)(p->y - cam_y);
		if (y > panel_h || y < -LEDGE_H) {
			lv_obj_set_hidden(ledge_obj[i], true);
			continue;
		}
		lv_obj_set_pos(ledge_obj[i], (int)p->x, y);
		lv_obj_set_width(ledge_obj[i], p->w);
		if ((ledge_kind_t)(intptr_t)lv_obj_get_user_data(ledge_obj[i]) != p->kind) {
			lv_obj_set_user_data(ledge_obj[i], (void *)(intptr_t)p->kind);
			lv_obj_invalidate(ledge_obj[i]);
		}
		lv_obj_set_hidden(ledge_obj[i], false);
	}

	// Lower and wider for a moment after a bounce.
	int squash = (int)(squash_left_us * 10 / SQUASH_US);
	int w = 2 * BODY_R + squash, h = 2 * BODY_R + 6 - squash;
	lv_obj_set_size(body_obj, w, h);
	lv_obj_set_pos(body_obj, (int)body_x - w / 2, panel_h - (int)(body_y - cam_y) - h);
	lv_obj_invalidate(body_obj); // the eyes may have turned
}

static void tick_cb(lv_timer_t *timer) {
	if (!active) {
		lv_timer_pause(timer); // woken by power.c at the end of a standby
		return;
	}
	int64_t now = now_us();
	int64_t elapsed = now - last_us;
	last_us = now;
	if (elapsed > STALL_US) {
		return; // everything is where it was when the screen went dark
	}
	step(elapsed > STEP_MAX_US ? STEP_MAX_US : elapsed);
	render();
}

// A finger on the left half steers left and one on the right half steers
// right, for as long as it is down; sliding across the middle changes sides.
static void touch_cb(lv_event_t *e) {
	if (!active) {
		return;
	}
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		steer = 0;
		return;
	}
	if (over) {
		if (code == LV_EVENT_PRESSED && now_us() - over_since_us >= OVER_HOLD_US) {
			new_game();
		}
		return;
	}
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	steer = p.x < panel_w / 2 ? -1 : 1;
	facing = steer;
	if (!steered) {
		steered = true;
		lv_obj_set_hidden(hint_label, true);
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	topbar_set_hidden(true);
	volume_overlay_allow_outside_player(true);

	srand((unsigned)now_us());
	best = (int)config_get_int("hopper", "best", 0);
	lv_label_set_text(hint_label, tr("hopper_hint"));
	// The end of a game reads the same in both games.
	lv_label_set_text(over_title, tr("tower_game_over"));
	lv_label_set_text(over_hint, tr("tower_tap_again"));
	new_game();
	render();

	gamesound_start();
	active = true;
	last_us = now_us();
	lv_timer_resume(tick_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	active = false;
	lv_timer_pause(tick_timer);
	gamesound_stop();
	if (best_unsaved) {
		best_unsaved = false;
		config_save();
	}
	volume_overlay_allow_outside_player(false);
	topbar_set_hidden(false);
}

// An object that takes no touches: the screen under it gets them.
static lv_obj_t *plain_create(lv_obj_t *parent) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_clickable(obj, false);
	lv_obj_set_scrollable(obj, false);
	return obj;
}

static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font, lv_color_t colour) {
	lv_obj_t *label = lv_label_create(parent);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, colour, 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(label, "");
	return label;
}

void hopper_init(gui_config_t *cfg) {
	panel_w = (int)cfg->screen_width;
	panel_h = (int)cfg->screen_height;

	// The game has its own colours whatever the theme.
	lv_obj_set_style_bg_color(hopper_screen, lv_color_hex(0x7ecbff), 0);
	lv_obj_set_style_bg_opa(hopper_screen, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(hopper_screen, false);

	// The same places every time: a fixed sequence, not rand().
	uint32_t seed = 0x2f6e2b1d;
	for (int i = 0; i < STARS; i++) {
		seed = seed * 1664525u + 1013904223u;
		star_xs[i] = (int)((seed >> 8) % (uint32_t)(panel_w - 4));
		seed = seed * 1664525u + 1013904223u;
		star_base[i] = (int)((seed >> 8) % (uint32_t)panel_h);
		int size = i % 5 == 0 ? 4 : 2;
		star_obj[i] = plain_create(hopper_screen);
		lv_obj_set_size(star_obj[i], size, size);
		lv_obj_set_style_bg_color(star_obj[i], lv_color_white(), 0);
		lv_obj_set_style_radius(star_obj[i], 1, 0);
	}
	// Clouds: three rounds side by side with the middle one raised.
	for (int i = 0; i < CLOUDS; i++) {
		int unit = 30 + (i % 3) * 9;
		lv_obj_t *c = plain_create(hopper_screen);
		lv_obj_set_size(c, unit * 3, unit * 3 / 2);
		for (int k = 0; k < 3; k++) {
			lv_obj_t *puff = plain_create(c);
			int d = k == 1 ? unit * 3 / 2 : unit;
			lv_obj_set_size(puff, d, d);
			lv_obj_set_pos(puff, k == 0 ? 0 : k == 1 ? unit * 3 / 4 : unit * 2, k == 1 ? 0 : unit / 2);
			lv_obj_set_style_radius(puff, LV_RADIUS_CIRCLE, 0);
			lv_obj_set_style_bg_color(puff, lv_color_white(), 0);
			lv_obj_set_style_bg_opa(puff, LV_OPA_COVER, 0);
		}
		cloud_obj[i] = c;
		cloud_xs[i] = (i * 173 + 20) % (panel_w - unit * 3);
		cloud_base[i] = i * (panel_h + 160) / CLOUDS + 60;
	}

	for (int i = 0; i < LEDGES; i++) {
		ledge_obj[i] = plain_create(hopper_screen);
		lv_obj_set_height(ledge_obj[i], LEDGE_H);
		lv_obj_set_hidden(ledge_obj[i], true);
		lv_obj_add_event_cb(ledge_obj[i], ledge_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	}
	body_obj = plain_create(hopper_screen);
	lv_obj_add_event_cb(body_obj, body_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

	score_label = label_create(hopper_screen, &font_ui_36_bold, lv_color_white());
	lv_obj_align(score_label, LV_ALIGN_TOP_MID, 0, 62);
	best_label = label_create(hopper_screen, &font_ui_20, lv_color_white());
	lv_obj_align(best_label, LV_ALIGN_TOP_MID, 0, 108);
	hint_label = label_create(hopper_screen, &font_ui_22, lv_color_white());
	lv_obj_set_width(hint_label, panel_w - 60);
	lv_obj_align(hint_label, LV_ALIGN_TOP_MID, 0, 150);

	// Over the game when it has ended.
	over_box = plain_create(hopper_screen);
	lv_obj_set_size(over_box, panel_w - 96, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(over_box, lv_color_hex(0x0a0d18), 0);
	lv_obj_set_style_bg_opa(over_box, LV_OPA_80, 0);
	lv_obj_set_style_radius(over_box, 18, 0);
	lv_obj_set_style_pad_ver(over_box, 22, 0);
	lv_obj_set_style_pad_row(over_box, 10, 0);
	lv_obj_set_flex_flow(over_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(over_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_align(over_box, LV_ALIGN_TOP_MID, 0, 262);
	over_title = label_create(over_box, &font_ui_36_bold, lv_color_white());
	over_hint = label_create(over_box, &font_ui_22, lv_color_hex(0xc8d2ee));
	lv_obj_set_hidden(over_box, true);

	lv_obj_add_event_cb(hopper_screen, touch_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(hopper_screen, touch_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(hopper_screen, touch_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(hopper_screen, touch_cb, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_add_event_cb(hopper_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(hopper_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	tick_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	lv_timer_pause(tick_timer);
	power_pause_in_standby(tick_timer);
}
