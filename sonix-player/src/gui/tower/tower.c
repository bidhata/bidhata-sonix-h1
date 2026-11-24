#include "tower.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/volume_overlay.h"
#include "src/system/audio/gamesound.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

lv_obj_t *tower_screen;

// ---------------------------------------------------------------------------
// the world
// ---------------------------------------------------------------------------
//
// Everything is in panel pixels. The top of the tower always rests on the same
// line of the screen (`landing_y`): when a block lands, the whole tower sinks
// by one block, so only the highest few are ever in sight and those are all
// that is kept.
//
// The picture is made here and not loaded: the sky is worked out into a
// buffer when the game is opened (and again when the time of day turns), the
// floors of the tower are drawn by a callback, and the hills, the clouds and
// the sparks are plain LVGL objects.

#define BLOCK_H 44
#define BASE_W 260
// The blocks of the tower that are drawn. More than fit under the landing
// line on either player.
#define SHOWN_MAX 12
// Pieces cut off and falling at the same time. One is made per drop and it is
// gone in well under a second; a fourth drop inside that time reuses the oldest.
#define FALLING_MAX 3

#define TICK_MS 16
// A gap between two ticks this long means the panel was dark or the player was
// busy with something else: the block does not jump ahead by that much.
#define STALL_US 250000
// The longest stretch of time a single tick moves the world by.
#define STEP_MAX_US 50000

// The block's speed across the screen, in pixels a second: what it starts at,
// what each block placed adds, and where it stops rising.
#define SPEED_START 210.0f
#define SPEED_PER_BLOCK 7.0f
#define SPEED_MAX 560.0f

// A drop this close to the block below counts as exact: the block is put
// squarely on it, nothing is cut off and it is worth two points.
#define EXACT_PX 5
// What is left of a block has to be at least this wide to stay.
#define KEEP_MIN_PX 12

#define SINK_US 180000
#define GRAVITY 2400.0f
// How long "Perfect!" stays up, and how long after the end a tap is ignored, so
// that the tap meant for the last block does not start the next game.
#define EXACT_SHOWN_US 700000
#define OVER_HOLD_US 600000

// The time of day moves on every so many blocks: day, dusk, night, dawn.
#define BLOCKS_PER_SKY 10
#define CLOUDS 3
// The sparks thrown up by an exact drop.
#define SPARKS 12
#define SPARK_US 650000
// How far above the foot of the tower the hills reach, and how deep the
// ground under it is drawn.
#define HILLS_ABOVE 120
#define GROUND_DEPTH 360

typedef struct {
	uint32_t top, bottom; // the sky, from the top of the screen to the bottom
	uint32_t orb;		  // the sun, or the moon
	bool night;			  // a crescent and stars
	uint32_t cloud;
	lv_opa_t cloud_opa; // 0: no clouds
} sky_t;

static const sky_t skies[] = {
	{0x2f8fe8, 0xbfe9ff, 0xfff2a8, false, 0xffffff, LV_OPA_90}, // day
	{0x3b2473, 0xff9e6b, 0xffd98a, false, 0xffc9d6, LV_OPA_60}, // dusk
	{0x060a1f, 0x27337a, 0xf2f0dc, true, 0, LV_OPA_TRANSP},		// night
	{0x2c7da6, 0xffdcb8, 0xfff0c8, false, 0xffffff, LV_OPA_80}, // dawn
};
#define SKIES ((int)(sizeof(skies) / sizeof(skies[0])))

typedef struct {
	int x, w;
	int level;
} block_t;

typedef struct {
	bool live;
	float x, y, vy;
	int w, level;
} falling_t;

typedef enum {
	ST_READY, // the first block is crossing; nothing dropped yet
	ST_PLAY,
	ST_OVER,
} game_state_t;

static int panel_w, panel_h;
static int landing_y;

static uint16_t *sky; // panel_w * panel_h, RGB565; NULL while the game is shut
static lv_image_dsc_t sky_dsc;
static int sky_drawn = -1; // which of `skies` is in the buffer
static lv_obj_t *sky_image;
static lv_obj_t *cloud_obj[CLOUDS];
static float cloud_x[CLOUDS];
static lv_obj_t *ground_obj;
static lv_obj_t *cable_obj;
static lv_obj_t *spark_obj[SPARKS];
static struct {
	float x, y, vx, vy;
	int64_t left_us;
} spark[SPARKS];
static lv_obj_t *block_obj[SHOWN_MAX];
static lv_obj_t *mover_obj;
static lv_obj_t *falling_obj[FALLING_MAX];
static lv_obj_t *score_label, *best_label, *hint_label, *exact_label;
static lv_obj_t *over_box, *over_title, *over_hint;
static lv_timer_t *tick_timer;

static bool active;
static game_state_t state;
static int64_t last_us;

static block_t shown[SHOWN_MAX]; // [0] is the top of the tower
static int shown_n;
static int placed; // blocks put on the base in this game
static int score, best;
static bool best_unsaved;

static float mover_x;
static int mover_w;
static int mover_dir;

static falling_t falling[FALLING_MAX];
static int falling_next;

static int64_t sink_left_us;  // what is left of the tower's sinking after a drop
static int64_t exact_left_us; // what is left of "Perfect!"
static int64_t over_since_us;

static int64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// The colour of the floor at `level`: the hue takes a good step with every
// floor, so no two neighbours look alike and the tower runs through the lot.
static lv_color_t level_colour(int level) { return lv_color_hsv_to_rgb((uint16_t)((200 + level * 23) % 360), 68, 96); }

// ---------------------------------------------------------------------------
// the sky
// ---------------------------------------------------------------------------

// Works the sky `which` out into the buffer: a gradient from top to bottom, the
// sun or the moon with a glow around it, and at night the stars. The panel
// shows 5 or 6 bits a colour, in which a plain gradient this tall comes out in
// stripes, so the colours are dithered on the way down to 16 bits.
static void sky_render(int which) {
	static const uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
	const sky_t *s = &skies[which];
	const int cx = panel_w - 96, cy = 150, r = 40, glow = 84;
	const int tr_ = (s->top >> 16) & 255, tg = (s->top >> 8) & 255, tb = s->top & 255;
	const int br = (s->bottom >> 16) & 255, bg = (s->bottom >> 8) & 255, bb = s->bottom & 255;
	const int or_ = (s->orb >> 16) & 255, og = (s->orb >> 8) & 255, ob = s->orb & 255;

	for (int y = 0; y < panel_h; y++) {
		int rr = tr_ + (br - tr_) * y / (panel_h - 1);
		int rg = tg + (bg - tg) * y / (panel_h - 1);
		int rb = tb + (bb - tb) * y / (panel_h - 1);
		uint16_t *row = sky + (size_t)y * panel_w;
		bool near_orb = y >= cy - glow && y <= cy + glow;
		for (int x = 0; x < panel_w; x++) {
			int pr = rr, pg = rg, pb = rb;
			if (near_orb && x >= cx - glow) {
				int d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
				bool disc = d2 <= r * r;
				if (disc && s->night) {
					// A crescent: the part of the disc a second, offset disc
					// does not cover.
					int ex = x - (cx + 17), ey = y - (cy - 9);
					disc = ex * ex + ey * ey > (r - 4) * (r - 4);
				}
				if (disc) {
					pr = or_, pg = og, pb = ob;
				} else if (d2 < glow * glow && !s->night) {
					// The glow thins out with the square of the distance
					// from the rim.
					int far = glow * glow - r * r;
					int k = (glow * glow - d2) * 256 / far; // 256 at the rim, 0 at the edge
					k = k * k / 256 * 90 / 256;				// at most about a third
					pr += (or_ - pr) * k / 256;
					pg += (og - pg) * k / 256;
					pb += (ob - pb) * k / 256;
				}
			}
			int t = bayer[y & 3][x & 3] * 16 + 8;
			int r5 = (pr * 31 + t) / 255, g6 = (pg * 63 + t) / 255, b5 = (pb * 31 + t) / 255;
			row[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
		}
	}

	if (s->night) {
		// The same stars every night: a fixed sequence, not rand().
		uint32_t seed = 0x51ed270b;
		for (int i = 0; i < 90; i++) {
			seed = seed * 1664525u + 1013904223u;
			int x = (int)((seed >> 8) % (uint32_t)(panel_w - 2));
			seed = seed * 1664525u + 1013904223u;
			int y = (int)((seed >> 8) % (uint32_t)(panel_h * 2 / 3));
			uint16_t *p = sky + (size_t)y * panel_w + x;
			p[0] = 0xffff;
			if (i % 4 == 0) { // one in four is a larger one
				p[1] = p[panel_w] = p[panel_w + 1] = 0xffff;
			}
		}
	}
	sky_drawn = which;
}

// The sky that goes with the height of the tower, and the clouds to match.
static void sky_set(int which) {
	if (!sky || which == sky_drawn) {
		return;
	}
	sky_render(which);
	lv_obj_invalidate(sky_image);
	for (int i = 0; i < CLOUDS; i++) {
		lv_obj_set_hidden(cloud_obj[i], skies[which].cloud_opa == LV_OPA_TRANSP);
		lv_obj_set_style_opa(cloud_obj[i], skies[which].cloud_opa, 0);
		uint32_t n = lv_obj_get_child_count(cloud_obj[i]);
		for (uint32_t c = 0; c < n; c++) {
			lv_obj_set_style_bg_color(lv_obj_get_child(cloud_obj[i], (int32_t)c), lv_color_hex(skies[which].cloud), 0);
		}
	}
}

// ---------------------------------------------------------------------------
// a floor of the tower
// ---------------------------------------------------------------------------

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

// Draws the object as one floor: a body in the floor's colour with a darker
// lip under it and a light edge along the roof, and a row of windows, some of
// them lit. Which are lit follows from the floor's number alone, so a floor
// looks the same wherever it is drawn. The number is the object's user data.
static void floor_draw_cb(lv_event_t *e) {
	lv_obj_t *obj = lv_event_get_current_target(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	if (!layer) {
		return;
	}
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	int level = (int)(intptr_t)lv_obj_get_user_data(obj);
	int w = lv_area_get_width(&a);
	lv_color_t body = level_colour(level);
	lv_color_t dark = lv_color_darken(body, 70);

	fill(layer, a.x1, a.y1, a.x2, a.y2, 6, dark, LV_OPA_COVER);
	fill(layer, a.x1, a.y1, a.x2, a.y2 - 5, 6, body, LV_OPA_COVER);
	fill(layer, a.x1 + 6, a.y1 + 3, a.x2 - 6, a.y1 + 5, 1, lv_color_white(), LV_OPA_50);

	const int win_w = 12, win_h = 15, pitch = 24, margin = 10;
	int n = (w - 2 * margin + (pitch - win_w)) / pitch;
	if (n < 1) {
		return;
	}
	int x = a.x1 + (w - (n * pitch - (pitch - win_w))) / 2;
	int y = a.y1 + 14;
	for (int i = 0; i < n; i++, x += pitch) {
		uint32_t h = (uint32_t)level * 2654435761u ^ (uint32_t)i * 40503u;
		bool lit = ((h >> 9) & 3) != 0;
		fill(layer, x, y, x + win_w - 1, y + win_h - 1, 2, lit ? lv_color_hex(0xfff3b0) : dark, LV_OPA_COVER);
	}
}

static float speed_now(void) {
	float s = SPEED_START + SPEED_PER_BLOCK * (float)placed;
	return s > SPEED_MAX ? SPEED_MAX : s;
}

// The block crosses with a third of itself allowed past either edge, so even
// a wide one has somewhere to be that is not over the tower.
static float mover_lo(void) { return -(float)mover_w / 3.0f; }
static float mover_hi(void) { return (float)(panel_w - mover_w) + (float)mover_w / 3.0f; }

static void mover_enter(void) {
	// From alternate sides, so the rhythm of one drop does not serve the next.
	mover_dir = (placed % 2 == 0) ? 1 : -1;
	mover_x = mover_dir > 0 ? mover_lo() : mover_hi();
}

static void falling_add(float x, int w, int level) {
	falling_t *f = &falling[falling_next];
	falling_next = (falling_next + 1) % FALLING_MAX;
	f->live = true;
	f->x = x;
	f->y = (float)(landing_y - BLOCK_H);
	f->vy = 0;
	f->w = w;
	f->level = level;
}

static void show_score(void) {
	lv_label_set_text_fmt(score_label, "%d", score);
	lv_label_set_text_fmt(best_label, tr("tower_best"), best);
}

static void new_game(void) {
	state = ST_READY;
	placed = 0;
	score = 0;
	shown_n = 1;
	shown[0] = (block_t){.x = (panel_w - BASE_W) / 2, .w = BASE_W, .level = 0};
	mover_w = BASE_W;
	mover_enter();
	memset(falling, 0, sizeof(falling));
	falling_next = 0;
	sink_left_us = 0;
	exact_left_us = 0;

	for (int i = 0; i < SPARKS; i++) {
		spark[i].left_us = 0;
	}
	sky_set(0);

	show_score();
	lv_obj_set_hidden(over_box, true);
	lv_obj_set_hidden(exact_label, true);
	lv_obj_set_hidden(hint_label, false);
}

// Sparks fly up from along the roof of the block just put down.
static void sparks_throw(int x, int w) {
	for (int i = 0; i < SPARKS; i++) {
		spark[i].x = (float)x + (float)w * ((float)i + 0.5f) / SPARKS;
		spark[i].y = (float)(landing_y - BLOCK_H);
		spark[i].vx = ((float)(rand() % 200) - 100.0f) * 1.6f;
		spark[i].vy = -260.0f - (float)(rand() % 260);
		spark[i].left_us = SPARK_US;
	}
}

static void game_over(void) {
	state = ST_OVER;
	over_since_us = now_us();
	if (score > best) {
		best = score;
		config_set_int("tower", "best", best);
		best_unsaved = true; // written out when the game is left
	}
	show_score();
	lv_obj_set_hidden(exact_label, true);
	lv_obj_set_hidden(over_box, false);
}

// The moving block is let go over the tower.
static void drop(void) {
	const block_t top = shown[0];
	int level = placed + 1;
	int x = (int)(mover_x + (mover_x >= 0 ? 0.5f : -0.5f));
	int left = x > top.x ? x : top.x;
	int right = (x + mover_w < top.x + top.w) ? x + mover_w : top.x + top.w;
	bool exact = abs(x - top.x) <= EXACT_PX;

	if (exact) {
		left = top.x;
		right = top.x + mover_w;
	} else if (right - left < KEEP_MIN_PX) {
		// Beside the tower, or on it by a sliver: the whole block goes.
		falling_add((float)x, mover_w, level);
		lv_obj_set_hidden(mover_obj, true);
		gamesound_play(GAMESOUND_OVER);
		game_over();
		return;
	} else if (x < top.x) {
		falling_add((float)x, left - x, level);
	} else {
		falling_add((float)right, x + mover_w - right, level);
	}

	memmove(&shown[1], &shown[0], sizeof(shown[0]) * (SHOWN_MAX - 1));
	shown[0] = (block_t){.x = left, .w = right - left, .level = level};
	if (shown_n < SHOWN_MAX) {
		shown_n++;
	}
	placed++;
	score += exact ? 2 : 1;
	mover_w = right - left;
	mover_enter();
	sink_left_us = SINK_US;
	state = ST_PLAY;

	show_score();
	lv_obj_set_hidden(hint_label, true);
	lv_obj_set_hidden(exact_label, !exact);
	exact_left_us = exact ? EXACT_SHOWN_US : 0;
	if (exact) {
		sparks_throw(left, right - left);
	}
	gamesound_play(exact ? GAMESOUND_EXACT : GAMESOUND_DROP);
	sky_set((placed / BLOCKS_PER_SKY) % SKIES);
}

static void step(int64_t us) {
	float dt = (float)us / 1e6f;

	if (state != ST_OVER) {
		mover_x += (float)mover_dir * speed_now() * dt;
		if (mover_x > mover_hi()) {
			mover_x = mover_hi();
			mover_dir = -1;
		} else if (mover_x < mover_lo()) {
			mover_x = mover_lo();
			mover_dir = 1;
		}
	}
	for (int i = 0; i < FALLING_MAX; i++) {
		falling_t *f = &falling[i];
		if (!f->live) {
			continue;
		}
		f->vy += GRAVITY * dt;
		f->y += f->vy * dt;
		if (f->y > (float)panel_h) {
			f->live = false;
		}
	}
	for (int i = 0; i < SPARKS; i++) {
		if (spark[i].left_us <= 0) {
			continue;
		}
		spark[i].left_us -= us;
		spark[i].vy += GRAVITY * 0.5f * dt;
		spark[i].x += spark[i].vx * dt;
		spark[i].y += spark[i].vy * dt;
	}
	// The clouds drift whatever the game is doing, each at its own pace, and
	// come round again from the left.
	for (int i = 0; i < CLOUDS; i++) {
		cloud_x[i] += (7.0f + 4.0f * (float)i) * dt;
		if (cloud_x[i] > (float)panel_w) {
			cloud_x[i] = -(float)lv_obj_get_width(cloud_obj[i]);
		}
	}
	if (sink_left_us > 0) {
		sink_left_us = sink_left_us > us ? sink_left_us - us : 0;
	}
	if (exact_left_us > 0) {
		exact_left_us = exact_left_us > us ? exact_left_us - us : 0;
		if (exact_left_us == 0) {
			lv_obj_set_hidden(exact_label, true);
		}
	}
}

static void place(lv_obj_t *obj, int x, int y, int w, int h, int level) {
	lv_obj_set_pos(obj, x, y);
	lv_obj_set_size(obj, w, h);
	if ((int)(intptr_t)lv_obj_get_user_data(obj) != level) {
		lv_obj_set_user_data(obj, (void *)(intptr_t)level);
		lv_obj_invalidate(obj); // the same place, another floor
	}
	lv_obj_set_hidden(obj, false);
}

// Puts every object where the world has it. LVGL redraws only what moved.
static void render(void) {
	// Just after a drop the tower stands one block high and eases down onto
	// the landing line; the block that is crossing rides down with it.
	int sink = 0;
	if (sink_left_us > 0) {
		float p = (float)sink_left_us / (float)SINK_US;
		sink = -(int)((float)BLOCK_H * p * p);
	}

	for (int i = 0; i < SHOWN_MAX; i++) {
		if (i >= shown_n) {
			lv_obj_set_hidden(block_obj[i], true);
			continue;
		}
		place(block_obj[i], shown[i].x, landing_y + i * BLOCK_H + sink, shown[i].w, BLOCK_H, shown[i].level);
	}

	// The tower stands on the ground, which goes down with it and out of
	// sight: it is drawn for as long as the first floor itself is.
	const block_t *lowest = &shown[shown_n - 1];
	int under = landing_y + shown_n * BLOCK_H + sink;
	if (lowest->level == 0 && under - HILLS_ABOVE < panel_h) {
		lv_obj_set_y(ground_obj, under - HILLS_ABOVE);
		lv_obj_set_hidden(ground_obj, false);
	} else {
		lv_obj_set_hidden(ground_obj, true);
	}

	// The block that is crossing hangs from a cable.
	if (state != ST_OVER) {
		int top = landing_y - BLOCK_H + sink;
		place(mover_obj, (int)mover_x, top, mover_w, BLOCK_H, placed + 1);
		lv_obj_set_pos(cable_obj, (int)mover_x + mover_w / 2 - 2, 0);
		lv_obj_set_height(cable_obj, top);
		lv_obj_set_hidden(cable_obj, false);
	} else {
		lv_obj_set_hidden(cable_obj, true);
	}

	for (int i = 0; i < CLOUDS; i++) {
		lv_obj_set_x(cloud_obj[i], (int)cloud_x[i]);
	}
	for (int i = 0; i < SPARKS; i++) {
		if (spark[i].left_us <= 0) {
			lv_obj_set_hidden(spark_obj[i], true);
			continue;
		}
		lv_obj_set_pos(spark_obj[i], (int)spark[i].x, (int)spark[i].y);
		lv_obj_set_hidden(spark_obj[i], false);
	}

	for (int i = 0; i < FALLING_MAX; i++) {
		if (!falling[i].live) {
			lv_obj_set_hidden(falling_obj[i], true);
			continue;
		}
		place(falling_obj[i], (int)falling[i].x, (int)falling[i].y, falling[i].w, BLOCK_H, falling[i].level);
	}
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
		return; // the block is where it was when the screen went dark
	}
	step(elapsed > STEP_MAX_US ? STEP_MAX_US : elapsed);
	render();
}

static void press_cb(lv_event_t *e) {
	(void)e;
	if (!active) {
		return;
	}
	if (state == ST_OVER) {
		if (now_us() - over_since_us >= OVER_HOLD_US) {
			new_game();
			render();
		}
		return;
	}
	drop();
	render();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	topbar_set_hidden(true);
	volume_overlay_allow_outside_player(true);

	// Without the buffer the game is played against the screen's plain colour.
	sky = malloc((size_t)panel_w * panel_h * sizeof(uint16_t));
	sky_drawn = -1;
	if (sky) {
		sky_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
		sky_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
		sky_dsc.header.w = (uint32_t)panel_w;
		sky_dsc.header.h = (uint32_t)panel_h;
		sky_dsc.header.stride = (uint32_t)panel_w * 2;
		sky_dsc.data_size = (uint32_t)panel_w * panel_h * 2;
		sky_dsc.data = (const uint8_t *)sky;
		lv_image_set_src(sky_image, &sky_dsc);
	}
	srand((unsigned)now_us());
	for (int i = 0; i < CLOUDS; i++) {
		cloud_x[i] = (float)(30 + i * 170);
	}

	best = (int)config_get_int("tower", "best", 0);
	lv_label_set_text(hint_label, tr("tower_tap_to_drop"));
	lv_label_set_text(exact_label, tr("tower_perfect"));
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
	lv_image_set_src(sky_image, NULL);
	if (sky) {
		lv_image_cache_drop(&sky_dsc); // LVGL's image cache is keyed by the descriptor's address
	}
	memset(&sky_dsc, 0, sizeof(sky_dsc));
	free(sky);
	sky = NULL;

	volume_overlay_allow_outside_player(false);
	topbar_set_hidden(false);
}

// An object drawn as a floor of the tower (floor_draw_cb).
static lv_obj_t *floor_create(lv_obj_t *parent) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_clickable(obj, false);
	lv_obj_set_scrollable(obj, false);
	lv_obj_set_hidden(obj, true);
	lv_obj_add_event_cb(obj, floor_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	return obj;
}

// A plain filled rectangle that takes no touches: the screen under it gets them.
static lv_obj_t *rect_create(lv_obj_t *parent, int radius) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_remove_style_all(obj);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(obj, radius, 0);
	lv_obj_set_clickable(obj, false);
	lv_obj_set_scrollable(obj, false);
	lv_obj_set_hidden(obj, true);
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

void tower_init(gui_config_t *cfg) {
	panel_w = (int)cfg->screen_width;
	panel_h = (int)cfg->screen_height;
	landing_y = panel_h * 66 / 100;

	// The game has its own colours whatever the theme. The screen's own is
	// what shows should the sky's buffer not be had.
	lv_obj_set_style_bg_color(tower_screen, lv_color_hex(0x2f8fe8), 0);
	lv_obj_set_style_bg_opa(tower_screen, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(tower_screen, false);

	sky_image = lv_image_create(tower_screen);
	lv_obj_set_pos(sky_image, 0, 0);
	lv_obj_set_size(sky_image, panel_w, panel_h);
	lv_obj_set_clickable(sky_image, false);

	// Three clouds, each three rounds side by side with the middle one raised.
	for (int i = 0; i < CLOUDS; i++) {
		static const int tops[CLOUDS] = {250, 318, 58};
		int unit = 34 + i * 8;
		lv_obj_t *c = lv_obj_create(tower_screen);
		lv_obj_remove_style_all(c);
		lv_obj_set_size(c, unit * 3, unit * 3 / 2);
		lv_obj_set_pos(c, 0, tops[i]);
		lv_obj_set_clickable(c, false);
		lv_obj_set_scrollable(c, false);
		for (int k = 0; k < 3; k++) {
			lv_obj_t *puff = rect_create(c, LV_RADIUS_CIRCLE);
			int d = k == 1 ? unit * 3 / 2 : unit;
			lv_obj_set_size(puff, d, d);
			lv_obj_set_pos(puff, k == 0 ? 0 : k == 1 ? unit * 3 / 4 : unit * 2, k == 1 ? 0 : unit / 2);
			lv_obj_set_hidden(puff, false);
		}
		cloud_obj[i] = c;
	}

	// The ground: two hills behind, grass, and the earth under it. One object,
	// moved as a whole; what of the hills reaches below the grass line is
	// covered by the grass.
	ground_obj = lv_obj_create(tower_screen);
	lv_obj_remove_style_all(ground_obj);
	lv_obj_set_size(ground_obj, panel_w, HILLS_ABOVE + GROUND_DEPTH);
	lv_obj_set_pos(ground_obj, 0, panel_h);
	lv_obj_set_clickable(ground_obj, false);
	lv_obj_set_scrollable(ground_obj, false);
	static const struct {
		int x, y, d;
		uint32_t colour;
	} hills[] = {{-150, 34, 420, 0x4fb75e}, {250, 8, 360, 0x67c873}, {110, 70, 300, 0x3fa652}};
	for (int i = 0; i < 3; i++) {
		lv_obj_t *hill = rect_create(ground_obj, LV_RADIUS_CIRCLE);
		lv_obj_set_size(hill, hills[i].d, hills[i].d);
		lv_obj_set_pos(hill, hills[i].x, hills[i].y);
		lv_obj_set_style_bg_color(hill, lv_color_hex(hills[i].colour), 0);
		lv_obj_set_hidden(hill, false);
	}
	static const struct {
		int y, h;
		uint32_t colour;
	} bands[] = {{HILLS_ABOVE, GROUND_DEPTH, 0x8a5a3c}, {HILLS_ABOVE + 150, GROUND_DEPTH - 150, 0x744a31}, {HILLS_ABOVE, 46, 0x35963f}, {HILLS_ABOVE, 8, 0x7fdc6a}};
	for (int i = 0; i < 4; i++) {
		lv_obj_t *band = rect_create(ground_obj, 0);
		lv_obj_set_size(band, panel_w, bands[i].h);
		lv_obj_set_pos(band, 0, bands[i].y);
		lv_obj_set_style_bg_color(band, lv_color_hex(bands[i].colour), 0);
		lv_obj_set_hidden(band, false);
	}

	cable_obj = rect_create(tower_screen, 0);
	lv_obj_set_width(cable_obj, 4);
	lv_obj_set_style_bg_color(cable_obj, lv_color_hex(0x263042), 0);
	lv_obj_set_style_bg_opa(cable_obj, LV_OPA_70, 0);

	for (int i = SHOWN_MAX - 1; i >= 0; i--) {
		block_obj[i] = floor_create(tower_screen);
	}
	for (int i = 0; i < FALLING_MAX; i++) {
		falling_obj[i] = floor_create(tower_screen);
	}
	mover_obj = floor_create(tower_screen);
	for (int i = 0; i < SPARKS; i++) {
		spark_obj[i] = rect_create(tower_screen, 2);
		lv_obj_set_size(spark_obj[i], 8, 8);
		lv_obj_set_style_bg_color(spark_obj[i], lv_color_hex(i % 2 ? 0xffffff : 0xffe066), 0);
	}

	score_label = label_create(tower_screen, &font_ui_64_bold, lv_color_white());
	lv_obj_align(score_label, LV_ALIGN_TOP_MID, 0, 96);
	best_label = label_create(tower_screen, &font_ui_20, lv_color_white());
	lv_obj_align(best_label, LV_ALIGN_TOP_MID, 0, 178);
	exact_label = label_create(tower_screen, &font_ui_24_bold, lv_color_hex(0xffe066));
	lv_obj_align(exact_label, LV_ALIGN_TOP_MID, 0, 214);
	hint_label = label_create(tower_screen, &font_ui_22, lv_color_white());
	lv_obj_align(hint_label, LV_ALIGN_TOP_MID, 0, 214);

	// Over the tower when a game has ended.
	over_box = lv_obj_create(tower_screen);
	lv_obj_remove_style_all(over_box);
	lv_obj_set_size(over_box, panel_w - 96, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(over_box, lv_color_hex(0x0a0d18), 0);
	lv_obj_set_style_bg_opa(over_box, LV_OPA_80, 0);
	lv_obj_set_style_radius(over_box, 18, 0);
	lv_obj_set_style_pad_ver(over_box, 22, 0);
	lv_obj_set_style_pad_row(over_box, 10, 0);
	lv_obj_set_flex_flow(over_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(over_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_clickable(over_box, false);
	lv_obj_set_scrollable(over_box, false);
	lv_obj_align(over_box, LV_ALIGN_TOP_MID, 0, 262);
	over_title = label_create(over_box, &font_ui_36_bold, lv_color_white());
	over_hint = label_create(over_box, &font_ui_22, lv_color_hex(0xc8d2ee));
	lv_obj_set_hidden(over_box, true);

	lv_obj_add_event_cb(tower_screen, press_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(tower_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(tower_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	tick_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	lv_timer_pause(tick_timer);
	power_pause_in_standby(tick_timer);
}
