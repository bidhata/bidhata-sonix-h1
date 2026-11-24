#include "videoplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/volume_overlay.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/video/subtitles.h"
#include "src/system/video/video.h"
#include "src/system/video/videoresume.h"

lv_obj_t *videoplay_screen;

// The page's tick. A frame is due every 33 to 42 ms and the tick decides when
// it is seen, so it is short: at 10 ms a frame is at most that late, and a tick
// with nothing due costs one lock and a comparison.
#define POLL_MS 10

// LVGL's refresh period while a video is up. Its usual 33 ms would add up to a
// frame of delay of its own and beat against a 24 or 25 frame film.
#define VIDEO_REFRESH_MS 10

// How long the controls stay up while the film is running. Paused, they stay.
#define CONTROLS_SHOWN_MS 3000

// How often the progress bar is brought up to date.
#define PROGRESS_MS 500

// How far one tap on either side button moves.
#define SEEK_STEP_S 10

// Carrying on where a film was left. It starts a little before, to pick up the
// thread; a film left in its first half minute or its last twentieth (the
// credits; ten seconds at least) was not really left part way through, and
// opens from the beginning.
#define RESUME_BACK_MS 5000
#define RESUME_MIN_MS 30000
#define RESUME_END_MIN_MS 10000

// The round buttons, and how far apart the three in the middle sit.
#define BTN_SIZE 84
#define BTN_BIG 108
#define BTN_SPREAD 150
#define EDGE_GAP 20

#define BAR_THICK 8
#define BAR_MARGIN 44
// How wide the strip around the bar is that takes a finger: the bar itself is
// too thin to hit.
#define BAR_TOUCH 64
// The two times: a box wide enough for "0:00:00", and how far its middle sits
// from the bar and from the end of it.
#define TIME_W 96
#define TIME_H 26
#define TIME_OFF 26

static int panel_w = 480, panel_h = 800;

static lv_obj_t *picture;
static lv_image_dsc_t picture_dsc;
static lv_obj_t *veil_label;

static lv_obj_t *controls; // everything below, shown and hidden as one
static lv_obj_t *btn_close, *btn_play, *btn_back10, *btn_fwd10;
static lv_obj_t *img_close, *img_play, *img_back10, *img_fwd10;
static lv_obj_t *bar, *bar_fill; // a track and the part of it already played
static lv_obj_t *bar_touch;		 // the strip over the bar that takes the finger
static lv_obj_t *time_at, *time_total; // where the film is and how long it is, at the two ends of the bar
// A finger is on the bar: the fill follows it, in thousandths of the film, and
// the film moves there when it lifts.
static bool bar_dragging;
static int bar_drag_value;

static lv_timer_t *poll_timer;
static uint32_t controls_shown_ms;
static uint32_t progress_ms;
static bool laid_out_turned;
static bool laid_out;
static bool leaving;

static char pending_path[1024];
static char playing_path[1024];
static bool on_screen;

// ---------------------------------------------------------------------------
// drawing direct
// ---------------------------------------------------------------------------
//
// With nothing over the picture, a frame goes straight into the framebuffer
// page that is off screen and the pages are swapped (display_direct_page in
// power.h). LVGL would copy the whole panel once more for every frame; while
// this lasts it is given nothing to draw, and draws nothing. Measured on the
// R1 with Bluetooth headphones, where the sound's encoder takes a sixth of the
// core as well, that copy is the difference between dropping a frame in five
// and not.
//
// LVGL takes over again -- the frame redrawn into the image, the whole screen
// invalidated -- the moment anything is to be drawn that is not the film: the
// controls, a message, and whatever else puts something on the screen by
// itself (the volume pill, a notice, the power menu). Those are not known
// here, so they are noticed: LVGL announces every area it is asked to redraw,
// and any that this page did not ask for itself is somebody else wanting the
// screen. Direct drawing starts again once it has been left alone for
// DIRECT_QUIET_MS.

#define DIRECT_QUIET_MS 1000

static bool direct;
static int direct_to_clear;	  // pages not yet cleared to black around the picture
static uint32_t disturbed_ms; // when something other than the film last drew
static bool own_drawing;	  // the area being invalidated is the picture's

// ---------------------------------------------------------------------------
// subtitles
// ---------------------------------------------------------------------------
//
// A line of text over the film has to be there in both ways of drawing, and
// the direct one has no LVGL in it to draw a label. So a line is made into a
// small picture once, when it comes up: an ordinary label, kept off the panel
// where nothing draws it, is drawn into a buffer of its own
// (lv_snapshot_take) and copied from there into `subs_pixels`, already turned
// when the film is. From then on the direct path mixes that picture into each
// frame after the film's own pixels, and the LVGL path shows the same buffer
// in an image. Neither draws text again until the line changes.
//
// The buffer is the largest a line can need and is kept for the whole film:
// the image points into it, and LVGL must never be left holding a pointer to
// memory that has been freed and handed out again.

#define SUBS_SIDE 32	  // kept clear at either end of a line, as the viewer sees it
#define SUBS_BOTTOM 84	  // from the viewer's bottom edge: above the bar and its times
#define SUBS_GAP 10		  // under a picture with a wide black band below it
#define SUBS_MAX_H 150	  // four lines; what a longer text is cut to

static bool subs_have;	   // this film has subtitles
static lv_obj_t *subs_label; // the label the picture is taken of; never on the panel
static lv_obj_t *subs_image; // the picture, for when LVGL draws
static lv_image_dsc_t subs_dsc;
static uint8_t *subs_pixels; // RGB565A8: the colours, then one byte of cover per pixel
static size_t subs_room;	 // how many pixels it holds
static int subs_cue = -1;	 // the line the picture is of; -1 for none
static int subs_x, subs_y, subs_w, subs_h; // where it goes on the panel
static unsigned subs_layout;			   // what it was laid out for: see subs_layout_now()
static int subs_image_cue = -2;			   // the line the image was last given
static unsigned subs_image_layout;

// A line that has gone leaves its text on whatever lies outside the picture,
// which no frame draws over. The place is blacked out on each of the two
// framebuffer pages before the next frame goes into it.
static int subs_wipe;
static int wipe_x, wipe_y, wipe_w, wipe_h;

static void invalidated_cb(lv_event_t *e) {
	(void)e;
	if (!own_drawing) {
		disturbed_ms = lv_tick_get();
	}
}

static void subs_sync_image(void);

static void direct_enter(void) {
	if (direct || !display_direct_page()) {
		return;
	}
	direct = true;
	direct_to_clear = 2;
	subs_wipe = 0; // both pages are about to be cleared whole
	printf("videoplay: drawing straight into the framebuffer (%dx%d)\n", panel_w, panel_h);
}

// Back to LVGL, with the frame on screen in its image and everything redrawn:
// neither framebuffer page holds what LVGL last drew there.
static void direct_leave(void) {
	if (!direct) {
		return;
	}
	direct = false;
	printf("videoplay: drawing through LVGL (%dx%d)\n", panel_w, panel_h);
	video_repaint(NULL);
	subs_sync_image(); // the line that was being drawn by hand is LVGL's again
	lv_obj_invalidate(lv_screen_active());
	lv_obj_invalidate(lv_layer_top());
	lv_obj_invalidate(lv_layer_sys());
}

// Everything a line's picture depends on besides its text: which way up the
// film is and where on the panel it lies. Known from the first frame on, so a
// line that was made before it is made again.
static unsigned subs_layout_now(void) {
	int x = 0, y = 0, w = 0, h = 0;
	video_picture_area(&x, &y, &w, &h);
	return (unsigned)video_turned() | (unsigned)x << 1 | (unsigned)y << 11 | (unsigned)w << 21;
}

// The black band under the picture, as the viewer sees it: nothing when the
// picture reaches the edge or has not been seen yet.
static int band_below_picture(bool turned) {
	int x, y, w, h;
	if (!video_picture_area(&x, &y, &w, &h)) {
		return 0;
	}
	// A turned picture has its bottom along the panel's left edge.
	return turned ? x : panel_h - (y + h);
}

// Remembers that the place the picture is in now has to be blacked out, on
// top of whatever was still waiting to be.
static void subs_wipe_current(void) {
	if (subs_cue < 0 || subs_w <= 0) {
		return;
	}
	if (subs_wipe > 0) {
		int x2 = wipe_x + wipe_w, y2 = wipe_y + wipe_h;
		int nx2 = subs_x + subs_w, ny2 = subs_y + subs_h;
		wipe_x = subs_x < wipe_x ? subs_x : wipe_x;
		wipe_y = subs_y < wipe_y ? subs_y : wipe_y;
		wipe_w = (nx2 > x2 ? nx2 : x2) - wipe_x;
		wipe_h = (ny2 > y2 ? ny2 : y2) - wipe_y;
	} else {
		wipe_x = subs_x, wipe_y = subs_y, wipe_w = subs_w, wipe_h = subs_h;
	}
	subs_wipe = 2;
}

// Makes the picture of line `cue`, or of nothing.
static void subs_make(int cue) {
	subs_wipe_current();
	subs_cue = -1;
	subs_layout = subs_layout_now();
	if (cue < 0 || !subs_pixels) {
		return;
	}

	bool turned = video_turned();
	int across = turned ? panel_h : panel_w; // the viewer's width
	lv_obj_set_style_max_width(subs_label, across - 2 * SUBS_SIDE, 0);
	lv_label_set_text(subs_label, subtitles_text(cue));
	lv_draw_buf_t *shot = lv_snapshot_take(subs_label, LV_COLOR_FORMAT_ARGB8888);
	if (!shot) {
		return; // out of memory for a moment: the film goes on without this line
	}

	// As the viewer sees it, cut to what the buffer holds.
	int w = (int)shot->header.w, h = (int)shot->header.h;
	if (w > across - 2 * SUBS_SIDE) {
		w = across - 2 * SUBS_SIDE;
	}
	if (h > SUBS_MAX_H) {
		h = SUBS_MAX_H;
	}
	if (w <= 0 || h <= 0 || (size_t)w * (size_t)h > subs_room) {
		lv_draw_buf_destroy(shot);
		return;
	}

	// On the panel a turned line stands on its end: as wide as it was tall.
	int out_w = turned ? h : w, out_h = turned ? w : h;
	uint16_t *colour = (uint16_t *)subs_pixels;
	uint8_t *cover = subs_pixels + (size_t)out_w * out_h * 2;
	for (int y = 0; y < h; y++) {
		const uint8_t *in = shot->data + (size_t)y * shot->header.stride; // blue, green, red, cover
		for (int x = 0; x < w; x++, in += 4) {
			// A quarter turn clockwise, as the film itself is turned: the
			// line's left end at the top of the panel, its foot to the left.
			size_t at = turned ? (size_t)x * out_w + (size_t)(h - 1 - y) : (size_t)y * out_w + (size_t)x;
			colour[at] = (uint16_t)((in[2] >> 3) << 11 | (in[1] >> 2) << 5 | in[0] >> 3);
			cover[at] = in[3];
		}
	}
	lv_draw_buf_destroy(shot);

	// Above the controls' bar; or, when the picture leaves a wide black band
	// below it, straight under the picture rather than far away at the edge.
	int band = band_below_picture(turned);
	int from_edge = band - h - SUBS_GAP > SUBS_BOTTOM ? band - h - SUBS_GAP : SUBS_BOTTOM;
	if (turned) {
		subs_x = from_edge;
		subs_y = (panel_h - out_h) / 2;
	} else {
		subs_x = (panel_w - out_w) / 2;
		subs_y = panel_h - from_edge - out_h;
	}
	if (subs_x < 0 || subs_y < 0 || subs_x + out_w > panel_w || subs_y + out_h > panel_h) {
		return; // a panel too small for it; nothing is drawn outside the page
	}
	subs_w = out_w;
	subs_h = out_h;
	subs_cue = cue;
}

// Brings the picture up to date with where the film is. Every tick.
static void subs_update(void) {
	if (!subs_have) {
		return;
	}
	int cue = subtitles_cue_at(video_position_ms());
	if (cue != subs_cue || (cue >= 0 && subs_layout != subs_layout_now())) {
		subs_make(cue);
	}
}

// Gives LVGL's image the picture as it is now. Only while LVGL is the one
// drawing: anything invalidated during direct drawing would have it paint
// into a framebuffer page behind the film's back.
static void subs_sync_image(void) {
	if (!subs_image || (subs_image_cue == subs_cue && subs_image_layout == subs_layout)) {
		return;
	}
	subs_image_cue = subs_cue;
	subs_image_layout = subs_layout;

	// The film's own drawing, as far as "who else wants the screen" goes: a
	// line changing must not be what keeps direct drawing from starting.
	own_drawing = true;
	if (subs_cue < 0) {
		lv_obj_set_hidden(subs_image, true);
	} else {
		subs_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
		subs_dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
		subs_dsc.header.w = (uint32_t)subs_w;
		subs_dsc.header.h = (uint32_t)subs_h;
		subs_dsc.header.stride = (uint32_t)subs_w * 2;
		subs_dsc.data_size = (uint32_t)subs_w * (uint32_t)subs_h * 3;
		subs_dsc.data = subs_pixels;
		lv_image_set_src(subs_image, &subs_dsc);
		lv_obj_set_pos(subs_image, subs_x, subs_y);
		lv_obj_set_size(subs_image, subs_w, subs_h);
		lv_obj_set_hidden(subs_image, false);
		lv_obj_invalidate(subs_image); // the same buffer with other pixels in it
	}
	own_drawing = false;
}

// Blacks out where a line that has gone was, in a framebuffer page about to
// take a frame. The frame then draws its part of that again.
static void subs_wipe_page(uint16_t *page) {
	for (int y = 0; y < wipe_h; y++) {
		memset(page + (size_t)(wipe_y + y) * panel_w + wipe_x, 0, (size_t)wipe_w * 2);
	}
}

// Mixes the line into a framebuffer page that has just taken a frame. Over
// the picture it is mixed with the film. Beside the picture it is mixed with
// black instead of with what is there, which is this same line from two
// frames ago: mixing with that over and over would fatten the letters' soft
// edges a little more with every frame.
static void subs_blend_page(uint16_t *page) {
	if (subs_cue < 0) {
		return;
	}
	int px = 0, py = 0, pw = 0, ph = 0;
	video_picture_area(&px, &py, &pw, &ph);
	const uint16_t *colour = (const uint16_t *)subs_pixels;
	const uint8_t *cover = subs_pixels + (size_t)subs_w * subs_h * 2;

	for (int y = 0; y < subs_h; y++) {
		int panel_y = subs_y + y;
		bool row_in_picture = panel_y >= py && panel_y < py + ph;
		uint16_t *out = page + (size_t)panel_y * panel_w + subs_x;
		for (int x = 0; x < subs_w; x++, colour++, cover++, out++) {
			unsigned a = *cover;
			if (a == 0) {
				continue;
			}
			if (a >= 250) {
				*out = *colour;
				continue;
			}
			int panel_x = subs_x + x;
			unsigned under = row_in_picture && panel_x >= px && panel_x < px + pw ? *out : 0;
			unsigned c = *colour;
			// The three colours each on their own, 5, 6 and 5 bits wide.
			unsigned r = ((c >> 11) * a + (under >> 11) * (255 - a)) / 255;
			unsigned g = (((c >> 5) & 63) * a + ((under >> 5) & 63) * (255 - a)) / 255;
			unsigned b = ((c & 31) * a + (under & 31) * (255 - a)) / 255;
			*out = (uint16_t)(r << 11 | g << 5 | b);
		}
	}
}

// A film is opening: its subtitles, if it has any, and the buffer for them.
static void subs_open(const char *path) {
	subs_cue = -1;
	subs_wipe = 0;
	subs_image_cue = -2;
	subs_have = subtitles_load(path);
	if (!subs_have) {
		return;
	}
	// The longest side less the margins, by the tallest a line may be: enough
	// for the film either way up.
	int longest = panel_w > panel_h ? panel_w : panel_h;
	subs_room = (size_t)(longest - 2 * SUBS_SIDE) * SUBS_MAX_H;
	subs_pixels = malloc(subs_room * 3);
	if (!subs_pixels) {
		subs_have = false;
		subtitles_unload();
	}
}

// The film is over. The image lets go of the buffer before it is freed.
static void subs_close(void) {
	lv_image_set_src(subs_image, NULL);
	lv_obj_set_hidden(subs_image, true);
	subs_image_cue = -2;
	subs_cue = -1;
	subs_wipe = 0;
	subs_have = false;
	free(subs_pixels);
	subs_pixels = NULL;
	subs_room = 0;
	subtitles_unload();
}

// ---------------------------------------------------------------------------
// the controls
// ---------------------------------------------------------------------------
//
// Laid out for whoever is watching. A turned picture has its top along the
// panel's right edge, so "across" for the viewer is down the panel: the three
// buttons run top to bottom, the close button sits in the panel's top right
// (the viewer's top left), the bar runs down the panel's left edge (along the
// viewer's bottom), and every icon is turned a quarter turn. An upright
// picture gets the same arrangement the ordinary way round.

static void place(lv_obj_t *obj, int cx, int cy) { lv_obj_set_pos(obj, cx - lv_obj_get_width(obj) / 2, cy - lv_obj_get_height(obj) / 2); }

static void layout_controls(bool turned) {
	if (laid_out && turned == laid_out_turned) {
		return;
	}
	laid_out = true;
	laid_out_turned = turned;

	int32_t angle = turned ? 900 : 0;
	lv_obj_t *const icons[] = {img_close, img_play, img_back10, img_fwd10};
	for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); i++) {
		lv_image_set_rotation(icons[i], angle);
	}

	lv_obj_update_layout(controls);
	int cx = panel_w / 2, cy = panel_h / 2;

	if (turned) {
		place(btn_back10, cx, cy - BTN_SPREAD);
		place(btn_play, cx, cy);
		place(btn_fwd10, cx, cy + BTN_SPREAD);
		place(btn_close, panel_w - EDGE_GAP - BTN_SIZE / 2, EDGE_GAP + BTN_SIZE / 2);

		// Down the left edge, filling from the top: the viewer's left to right.
		lv_obj_set_size(bar, BAR_THICK, panel_h - 2 * BAR_MARGIN);
		lv_obj_set_pos(bar, EDGE_GAP, BAR_MARGIN);
		lv_obj_set_size(bar_touch, BAR_TOUCH, panel_h);
		lv_obj_set_pos(bar_touch, 0, 0);

		// Beside the bar, which for the viewer is above it; turned like the
		// icons, about their own middles.
		lv_obj_set_style_transform_rotation(time_at, angle, 0);
		lv_obj_set_style_transform_rotation(time_total, angle, 0);
		lv_obj_set_style_text_align(time_at, LV_TEXT_ALIGN_LEFT, 0);
		lv_obj_set_style_text_align(time_total, LV_TEXT_ALIGN_RIGHT, 0);
		place(time_at, EDGE_GAP + BAR_THICK + TIME_OFF, BAR_MARGIN + TIME_W / 2);
		place(time_total, EDGE_GAP + BAR_THICK + TIME_OFF, panel_h - BAR_MARGIN - TIME_W / 2);
	} else {
		place(btn_back10, cx - BTN_SPREAD, cy);
		place(btn_play, cx, cy);
		place(btn_fwd10, cx + BTN_SPREAD, cy);
		place(btn_close, EDGE_GAP + BTN_SIZE / 2, EDGE_GAP + BTN_SIZE / 2);

		lv_obj_set_size(bar, panel_w - 2 * BAR_MARGIN, BAR_THICK);
		lv_obj_set_pos(bar, BAR_MARGIN, panel_h - EDGE_GAP - BAR_THICK);
		lv_obj_set_size(bar_touch, panel_w, BAR_TOUCH);
		lv_obj_set_pos(bar_touch, 0, panel_h - BAR_TOUCH);

		lv_obj_set_style_transform_rotation(time_at, angle, 0);
		lv_obj_set_style_transform_rotation(time_total, angle, 0);
		lv_obj_set_style_text_align(time_at, LV_TEXT_ALIGN_LEFT, 0);
		lv_obj_set_style_text_align(time_total, LV_TEXT_ALIGN_RIGHT, 0);
		place(time_at, BAR_MARGIN + TIME_W / 2, panel_h - EDGE_GAP - BAR_THICK - TIME_OFF);
		place(time_total, panel_w - BAR_MARGIN - TIME_W / 2, panel_h - EDGE_GAP - BAR_THICK - TIME_OFF);
	}
}

// "4:07", and "1:04:07" from the first hour on.
static void format_time(int64_t ms, char *out, size_t size) {
	long s = ms > 0 ? (long)(ms / 1000) : 0;
	if (s >= 3600) {
		snprintf(out, size, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
	} else {
		snprintf(out, size, "%ld:%02ld", s / 60, s % 60);
	}
}

static void refresh_progress(void) {
	int64_t total = video_duration_ms();
	int64_t at = video_position_ms();
	int value = total > 0 ? (int)(at * 1000 / total) : 0;
	if (value > 1000) {
		value = 1000;
	}
	// Under a finger the bar shows where the finger is, not where the film is.
	if (bar_dragging) {
		value = bar_drag_value;
		at = total * value / 1000;
	}
	char text[40]; // room for any long: the compiler checks the worst case
	format_time(at, text, sizeof(text));
	if (strcmp(lv_label_get_text(time_at), text) != 0) {
		lv_label_set_text(time_at, text);
	}
	format_time(total, text, sizeof(text));
	if (strcmp(lv_label_get_text(time_total), text) != 0) {
		lv_label_set_text(time_total, text);
	}
	// Grows from the panel's top when turned, which is the viewer's left.
	if (laid_out_turned) {
		lv_obj_set_size(bar_fill, BAR_THICK, (panel_h - 2 * BAR_MARGIN) * value / 1000);
	} else {
		lv_obj_set_size(bar_fill, (panel_w - 2 * BAR_MARGIN) * value / 1000, BAR_THICK);
	}
	progress_ms = lv_tick_get();
}

static void refresh_play_icon(void) { lv_image_set_src(img_play, video_paused() ? &icon_play_large : &icon_pause_large); }

static void controls_show(void) {
	layout_controls(video_turned());
	refresh_play_icon();
	refresh_progress();
	lv_obj_set_hidden(controls, false);
	controls_shown_ms = lv_tick_get();
}

static void controls_hide(void) { lv_obj_set_hidden(controls, true); }

static bool controls_visible(void) { return !lv_obj_is_hidden(controls); }

// Paused, the screen is allowed to go dark like anywhere else; playing, nobody
// touches it for two hours and it must stay on.
static void hold_screen(void) { power_hold_screen_for_view(video_running() && !video_paused() && !video_failed() && !video_ended()); }

static void picture_tap_cb(lv_event_t *e) {
	(void)e;
	if (controls_visible() && !video_paused() && !video_failed()) {
		controls_hide();
	} else {
		controls_show();
	}
}

// A finger on the bar, moving along it, and lifting. The film is moved once,
// when the finger lifts: every seek throws away what is decoded and starts at
// a key frame, and doing that all the way along a drag would show nothing.
static void bar_touch_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	if (video_failed() || video_duration_ms() <= 0) {
		bar_dragging = false;
		return;
	}

	if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
		lv_indev_t *indev = lv_indev_active();
		if (!indev) {
			return;
		}
		lv_point_t point;
		lv_indev_get_point(indev, &point);
		// Along the bar: down the panel when turned, across it otherwise.
		int along = laid_out_turned ? point.y : point.x;
		int length = (laid_out_turned ? panel_h : panel_w) - 2 * BAR_MARGIN;
		int value = length > 0 ? (along - BAR_MARGIN) * 1000 / length : 0;
		bar_drag_value = value < 0 ? 0 : value > 1000 ? 1000 : value;
		bar_dragging = true;
		refresh_progress();
		controls_shown_ms = lv_tick_get(); // not hidden from under the finger
	} else if (code == LV_EVENT_RELEASED) {
		if (bar_dragging) {
			video_seek_to(video_duration_ms() * bar_drag_value / 1000);
			bar_dragging = false;
			controls_shown_ms = lv_tick_get();
		}
	} else if (code == LV_EVENT_PRESS_LOST) {
		bar_dragging = false;
		refresh_progress();
	}
}

static void play_cb(lv_event_t *e) {
	(void)e;
	if (video_failed()) {
		return;
	}
	video_set_paused(!video_paused());
	hold_screen();
	controls_show();
}

static void seek_cb(lv_event_t *e) {
	int step = (int)(intptr_t)lv_event_get_user_data(e);
	if (video_failed()) {
		return;
	}
	video_seek_relative(step);
	controls_show();
}

static void close_cb(lv_event_t *e) {
	(void)e;
	if (leaving) {
		return;
	}
	leaving = true;
	direct_leave();	   // the page change is LVGL's to draw
	back_btn_cb(NULL); // unloading the page stops the film (screen_unloaded_cb)
}

// A round, dark, half-transparent button with a white icon: readable over any
// picture, and not much in the way of it.
static lv_obj_t *round_button(lv_obj_t *parent, int size, const lv_image_dsc_t *icon, lv_event_cb_t cb, void *user, lv_obj_t **icon_out) {
	lv_obj_t *btn = lv_obj_create(parent);
	lv_obj_remove_style_all(btn);
	lv_obj_set_size(btn, size, size);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_80, LV_STATE_PRESSED);
	lv_obj_set_clickable(btn, true);
	lv_obj_set_scrollable(btn, false);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);

	lv_obj_t *img = lv_image_create(btn);
	lv_image_set_src(img, icon);
	lv_obj_set_style_image_recolor(img, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
	lv_obj_center(img);
	*icon_out = img;
	return btn;
}

// ---------------------------------------------------------------------------
// the tick
// ---------------------------------------------------------------------------

static void show_message(const char *key) {
	lv_label_set_text(veil_label, tr(key));
	lv_obj_set_hidden(veil_label, false);
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;

	if (leaving) {
		return;
	}

	if (video_failed()) {
		direct_leave();
		// Says why and waits: the close button is the way out.
		if (lv_obj_is_hidden(veil_label)) {
			show_message(video_error());
			hold_screen();
			controls_show();
		}
		return;
	}

	// The screen was switched off with the film running: nobody is watching,
	// and decoding in the dark only empties the battery.
	if (!power_screen_is_on()) {
		direct_leave();
		if (video_running() && !video_paused()) {
			video_set_paused(true);
			hold_screen();
			controls_show();
		}
		return;
	}

	const uint16_t *pixels = video_screen();
	if (pixels && picture_dsc.data == NULL) {
		picture_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
		picture_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
		picture_dsc.header.w = (uint32_t)panel_w;
		picture_dsc.header.h = (uint32_t)panel_h;
		picture_dsc.header.stride = (uint32_t)panel_w * 2;
		picture_dsc.data_size = (uint32_t)panel_w * (uint32_t)panel_h * 2;
		picture_dsc.data = (const uint8_t *)pixels;
		lv_image_set_src(picture, &picture_dsc);
		lv_obj_set_hidden(picture, false);
	}

	subs_update();

	// Who draws this tick (see "drawing direct").
	bool film_only = video_running() && !video_paused() && !controls_visible() && lv_obj_is_hidden(veil_label) && picture_dsc.data != NULL && lv_tick_get() - disturbed_ms > DIRECT_QUIET_MS;
	if (direct && !film_only) {
		direct_leave();
	} else if (!direct && film_only) {
		direct_enter();
	}

	// The conversion happens here, on the thread that then shows it (see
	// video.h): a frame is never on screen half made.
	if (direct) {
		uint16_t *page = display_direct_page();
		// Black around the picture, once for each of the two pages: the first
		// before the first frame goes into it, the second when the first swap
		// has taken it off screen.
		if (direct_to_clear == 2) {
			memset(page, 0, (size_t)panel_w * (size_t)panel_h * 2);
			direct_to_clear = 1;
		}
		// Before the frame, so that the frame puts back what part of the
		// picture this takes out. A tick with no frame due leaves the page
		// wiped and unseen, and the next one does it again: it only counts
		// once a frame has gone in and the page has been shown.
		if (subs_wipe > 0) {
			subs_wipe_page(page);
		}
		if (video_present(page)) {
			subs_blend_page(page);
			display_direct_flip();
			if (subs_wipe > 0) {
				subs_wipe--;
			}
			if (direct_to_clear == 1) {
				memset(display_direct_page(), 0, (size_t)panel_w * (size_t)panel_h * 2);
				direct_to_clear = 0;
			}
		}
	} else {
		subs_sync_image();
		if (video_present(NULL)) {
			lv_obj_set_hidden(veil_label, true);
			own_drawing = true;
			lv_obj_invalidate(picture);
			own_drawing = false;
		}
	}

	if (video_ended()) {
		close_cb(NULL);
		return;
	}

	if (controls_visible()) {
		uint32_t now = lv_tick_get();
		if (!video_paused() && now - controls_shown_ms > CONTROLS_SHOWN_MS) {
			controls_hide();
		} else if (now - progress_ms > PROGRESS_MS) {
			refresh_progress();
		}
	}
}

// ---------------------------------------------------------------------------
// entering and leaving
// ---------------------------------------------------------------------------

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;

	// The whole panel is the picture: no status bar, no back arrow (the close
	// button among the controls is the way out), and the volume keys get the
	// player's own pill since there is no bar to show the level in.
	topbar_set_hidden(true);
	back_btn_force_hidden(true);
	volume_overlay_allow_outside_player(true);

	leaving = false;
	laid_out = false;
	direct = false;
	bar_dragging = false;
	disturbed_ms = lv_tick_get();
	controls_hide();
	show_message("video_opening");

	if (pending_path[0]) {
		int64_t at = videoresume_get(pending_path);
		snprintf(playing_path, sizeof(playing_path), "%s", pending_path);
		subs_open(pending_path);
		video_start(pending_path, at > RESUME_BACK_MS ? at - RESUME_BACK_MS : 0);
		pending_path[0] = '\0';
	}
	on_screen = true;
	hold_screen();

	lv_timer_resume(poll_timer);
	lv_timer_ready(poll_timer);
	gui_refresh_period_set(VIDEO_REFRESH_MS);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;

	lv_timer_pause(poll_timer);
	direct_leave();

	// The image lets go of the pixels BEFORE video_stop() frees them: LVGL
	// redraws when it likes, and a descriptor pointing at freed memory is a
	// crash somewhere else half a second later.
	lv_image_set_src(picture, NULL);
	picture_dsc.data = NULL;
	lv_obj_set_hidden(picture, true);
	subs_close();

	// Where it was left, read before the thread goes. A film that ran to its
	// end, or was left close to either end, is forgotten instead.
	on_screen = false;
	if (playing_path[0] && !video_failed()) {
		int64_t at = video_position_ms();
		int64_t total = video_duration_ms();
		int64_t tail = total / 20 > RESUME_END_MIN_MS ? total / 20 : RESUME_END_MIN_MS;
		bool part_way = !video_ended() && at >= RESUME_MIN_MS && (total <= 0 || at < total - tail);
		videoresume_set(playing_path, part_way ? at : 0);
	}
	playing_path[0] = '\0';

	video_stop();
	power_hold_screen_for_view(false);

	gui_refresh_period_restore();
	controls_hide();

	volume_overlay_allow_outside_player(false);
	back_btn_force_hidden(false);
	topbar_set_hidden(false);
}

void videoplay_open(const char *path) {
	if (!path || !path[0]) {
		return;
	}
	snprintf(pending_path, sizeof(pending_path), "%s", path);
	switch_screen(videoplay_screen);
}

bool videoplay_key(gui_key_t key) {
	if (!on_screen || leaving) {
		return false;
	}
	// Taken even when there is nothing to do with it: a key falling through to
	// the music player would start a song under the film.
	if (video_failed() || !video_running()) {
		return true;
	}
	switch (key) {
	case GUI_KEY_PLAY_PAUSE:
		play_cb(NULL);
		break;
	case GUI_KEY_NEXT:
		video_seek_relative(SEEK_STEP_S);
		controls_show();
		break;
	case GUI_KEY_PREV:
		video_seek_relative(-SEEK_STEP_S);
		controls_show();
		break;
	}
	return true;
}

void videoplay_init(gui_config_t *cfg) {
	videoresume_set_root(cfg->sd_root_path);
	panel_w = (int)cfg->screen_width;
	panel_h = (int)cfg->screen_height;
	video_set_output(panel_w, panel_h);

	lv_obj_add_style(videoplay_screen, &theme_style_screen, 0);
	lv_obj_set_style_bg_color(videoplay_screen, lv_color_black(), 0);
	lv_obj_set_scrollable(videoplay_screen, false);

	picture = lv_image_create(videoplay_screen);
	lv_obj_set_pos(picture, 0, 0);
	lv_obj_set_size(picture, panel_w, panel_h);
	lv_obj_set_hidden(picture, true);

	// The subtitle line, over the picture and under everything else. And the
	// label its picture is taken of (see "subtitles"), parked far off the
	// panel: it is laid out like any other, and never drawn there.
	subs_image = lv_image_create(videoplay_screen);
	lv_obj_set_hidden(subs_image, true);
	subs_label = lv_label_create(videoplay_screen);
	lv_obj_set_pos(subs_label, 0, -4 * panel_h);
	lv_obj_set_size(subs_label, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(subs_label, SUBS_MAX_H, 0);
	lv_label_set_long_mode(subs_label, LV_LABEL_LONG_WRAP);
	lv_label_set_text(subs_label, "");
	lv_obj_set_style_text_align(subs_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(subs_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(subs_label, lv_color_white(), 0);
	// A dark plate behind the words: white letters on a white shirt are not
	// read otherwise.
	lv_obj_set_style_bg_color(subs_label, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(subs_label, LV_OPA_60, 0);
	lv_obj_set_style_radius(subs_label, 8, 0);
	lv_obj_set_style_pad_hor(subs_label, 12, 0);
	lv_obj_set_style_pad_ver(subs_label, 6, 0);

	// What is said while the file opens, and when it does not.
	veil_label = lv_label_create(videoplay_screen);
	lv_label_set_text(veil_label, "");
	lv_label_set_long_mode(veil_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(veil_label, panel_w - 64);
	lv_obj_set_style_text_align(veil_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(veil_label, &font_ui_20, 0);
	lv_obj_set_style_text_color(veil_label, lv_color_make(170, 170, 175), 0);
	lv_obj_align(veil_label, LV_ALIGN_CENTER, 0, -BTN_BIG);

	// The whole panel takes the tap that brings the controls up and sends them
	// away. Under the controls, so their buttons get their own taps first.
	{
		lv_obj_t *zone = lv_obj_create(videoplay_screen);
		lv_obj_remove_style_all(zone);
		lv_obj_set_pos(zone, 0, 0);
		lv_obj_set_size(zone, panel_w, panel_h);
		lv_obj_set_clickable(zone, true);
		lv_obj_set_scrollable(zone, false);
		lv_obj_add_event_cb(zone, picture_tap_cb, LV_EVENT_CLICKED, NULL);
	}

	controls = lv_obj_create(videoplay_screen);
	lv_obj_remove_style_all(controls);
	lv_obj_set_pos(controls, 0, 0);
	lv_obj_set_size(controls, panel_w, panel_h);
	lv_obj_set_scrollable(controls, false);
	lv_obj_set_clickable(controls, false); // taps between the buttons fall through to the zone

	btn_back10 = round_button(controls, BTN_SIZE, &icon_prev_10, seek_cb, (void *)(intptr_t)-SEEK_STEP_S, &img_back10);
	btn_play = round_button(controls, BTN_BIG, &icon_pause_large, play_cb, NULL, &img_play);
	btn_fwd10 = round_button(controls, BTN_SIZE, &icon_next_10, seek_cb, (void *)(intptr_t)SEEK_STEP_S, &img_fwd10);
	btn_close = round_button(controls, BTN_SIZE, &icon_close, close_cb, NULL, &img_close);

	// Two plain rectangles rather than an lv_bar: the turned one fills from
	// the top down, which a bar does not do without being told its range
	// backwards.
	bar = lv_obj_create(controls);
	lv_obj_remove_style_all(bar);
	lv_obj_set_style_radius(bar, BAR_THICK / 2, 0);
	lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_30, 0);
	lv_obj_set_scrollable(bar, false);

	bar_fill = lv_obj_create(bar);
	lv_obj_remove_style_all(bar_fill);
	lv_obj_set_style_radius(bar_fill, BAR_THICK / 2, 0);
	lv_obj_set_style_bg_color(bar_fill, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(bar_fill, LV_OPA_COVER, 0);
	lv_obj_set_pos(bar_fill, 0, 0);
	lv_obj_set_size(bar_fill, 0, 0);

	// Digits only, so nothing here is a text to translate.
	lv_obj_t **const times[] = {&time_at, &time_total};
	for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
		lv_obj_t *label = lv_label_create(controls);
		lv_label_set_text(label, "");
		lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
		lv_obj_set_size(label, TIME_W, TIME_H);
		lv_obj_set_style_text_font(label, &font_ui_20, 0);
		lv_obj_set_style_text_color(label, lv_color_white(), 0);
		lv_obj_set_style_transform_pivot_x(label, TIME_W / 2, 0);
		lv_obj_set_style_transform_pivot_y(label, TIME_H / 2, 0);
		*times[i] = label;
	}

	// Over the bar and wider than it. Sized and placed with the bar, in
	// layout_controls().
	bar_touch = lv_obj_create(controls);
	lv_obj_remove_style_all(bar_touch);
	lv_obj_set_scrollable(bar_touch, false);
	lv_obj_set_clickable(bar_touch, true);
	lv_obj_add_event_cb(bar_touch, bar_touch_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(bar_touch, bar_touch_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(bar_touch, bar_touch_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(bar_touch, bar_touch_cb, LV_EVENT_PRESS_LOST, NULL);

	lv_obj_set_hidden(controls, true);

	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	// Every area LVGL is asked to redraw, on any page: see "drawing direct".
	lv_display_add_event_cb(lv_display_get_default(), invalidated_cb, LV_EVENT_INVALIDATE_AREA, NULL);

	lv_obj_add_event_cb(videoplay_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(videoplay_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	// No back swipe: a finger across a film is reaching for the controls, and
	// leaving by accident loses the place.
}
