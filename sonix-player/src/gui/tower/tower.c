#include "tower.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/volume_overlay.h"
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

static lv_obj_t *pedestal_obj;
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

// The colour of the block at `level`: the hue turns a little with every block,
// so the tower shades from one colour into the next on its way up.
static lv_color_t level_colour(int level) { return lv_color_hsv_to_rgb((uint16_t)((205 + level * 9) % 360), 62, 94); }

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

	show_score();
	lv_obj_set_hidden(over_box, true);
	lv_obj_set_hidden(exact_label, true);
	lv_obj_set_hidden(hint_label, false);
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
	lv_obj_set_style_bg_color(obj, level_colour(level), 0);
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

	// The base stands on a pedestal that runs off the bottom of the screen,
	// for as long as the base itself is in sight.
	const block_t *lowest = &shown[shown_n - 1];
	int under = landing_y + shown_n * BLOCK_H + sink;
	if (lowest->level == 0 && under < panel_h) {
		lv_obj_set_pos(pedestal_obj, lowest->x, under);
		lv_obj_set_size(pedestal_obj, lowest->w, panel_h - under);
		lv_obj_set_hidden(pedestal_obj, false);
	} else {
		lv_obj_set_hidden(pedestal_obj, true);
	}

	if (state != ST_OVER) {
		place(mover_obj, (int)mover_x, landing_y - BLOCK_H + sink, mover_w, BLOCK_H, placed + 1);
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

	best = (int)config_get_int("tower", "best", 0);
	lv_label_set_text(hint_label, tr("tower_tap_to_drop"));
	lv_label_set_text(exact_label, tr("tower_perfect"));
	lv_label_set_text(over_title, tr("tower_game_over"));
	lv_label_set_text(over_hint, tr("tower_tap_again"));
	new_game();
	render();

	active = true;
	last_us = now_us();
	lv_timer_resume(tick_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	active = false;
	lv_timer_pause(tick_timer);
	if (best_unsaved) {
		best_unsaved = false;
		config_save();
	}
	volume_overlay_allow_outside_player(false);
	topbar_set_hidden(false);
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
	landing_y = panel_h * 56 / 100;

	// The game has its own colours whatever the theme: a night sky, darker
	// towards the ground.
	lv_obj_set_style_bg_color(tower_screen, lv_color_hex(0x1c2748), 0);
	lv_obj_set_style_bg_grad_color(tower_screen, lv_color_hex(0x0a0d18), 0);
	lv_obj_set_style_bg_grad_dir(tower_screen, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_opa(tower_screen, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(tower_screen, false);

	pedestal_obj = rect_create(tower_screen, 0);
	lv_obj_set_style_bg_color(pedestal_obj, lv_color_hex(0x2a3558), 0);
	for (int i = SHOWN_MAX - 1; i >= 0; i--) {
		block_obj[i] = rect_create(tower_screen, 5);
	}
	for (int i = 0; i < FALLING_MAX; i++) {
		falling_obj[i] = rect_create(tower_screen, 5);
		lv_obj_set_style_bg_opa(falling_obj[i], LV_OPA_70, 0);
	}
	mover_obj = rect_create(tower_screen, 5);

	score_label = label_create(tower_screen, &font_ui_64_bold, lv_color_white());
	lv_obj_align(score_label, LV_ALIGN_TOP_MID, 0, 96);
	best_label = label_create(tower_screen, &font_ui_20, lv_color_hex(0x9aa6c8));
	lv_obj_align(best_label, LV_ALIGN_TOP_MID, 0, 178);
	exact_label = label_create(tower_screen, &font_ui_24_bold, lv_color_hex(0xffd866));
	lv_obj_align(exact_label, LV_ALIGN_TOP_MID, 0, 214);
	hint_label = label_create(tower_screen, &font_ui_22, lv_color_hex(0x9aa6c8));
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
	over_hint = label_create(over_box, &font_ui_22, lv_color_hex(0x9aa6c8));
	lv_obj_set_hidden(over_box, true);

	lv_obj_add_event_cb(tower_screen, press_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(tower_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(tower_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	tick_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	lv_timer_pause(tick_timer);
	power_pause_in_standby(tick_timer);
}
