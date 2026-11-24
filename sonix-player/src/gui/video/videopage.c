#include "videopage.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/video/videoplay.h"
#include "src/system/core/lang.h"
#include "src/system/video/video.h"

lv_obj_t *videopage_screen;

// Where the videos live on the card, beside Music and Games.
#define VIDEO_DIR "Video"

#define NAME_MAX_LEN 256

typedef struct {
	char name[NAME_MAX_LEN]; // the file name, extension and all
} video_entry_t;

static char video_dir[512];
static lv_obj_t *list_box;
static lv_obj_t *status_label;

// The list the rows are showing. Rows carry the index, not the pointer, so the
// array can be freed and rebuilt without a surviving row dangling.
static video_entry_t *entries;
static int entry_count;

static int by_name(const void *a, const void *b) { return strcasecmp(((const video_entry_t *)a)->name, ((const video_entry_t *)b)->name); }

// One directory, read where it is asked for: a listing of a folder of films is
// a few dozen names, and unlike the game list nothing has to be opened to show
// them.
static void scan(void) {
	free(entries);
	entries = NULL;
	entry_count = 0;

	DIR *dir = video_dir[0] ? opendir(video_dir) : NULL;
	if (!dir) {
		return;
	}

	int cap = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' || !video_is_video_file(de->d_name)) {
			continue;
		}
		if (entry_count == cap) {
			int grown = cap ? cap * 2 : 32;
			video_entry_t *bigger = realloc(entries, (size_t)grown * sizeof(*entries));
			if (!bigger) {
				break;
			}
			entries = bigger;
			cap = grown;
		}
		snprintf(entries[entry_count].name, sizeof(entries[entry_count].name), "%s", de->d_name);
		entry_count++;
	}
	closedir(dir);

	if (entry_count > 1) {
		qsort(entries, (size_t)entry_count, sizeof(*entries), by_name);
	}
}

static void open_cb(lv_event_t *e) {
	// The release of a back swipe lands on whichever row is under the finger:
	// without this, swiping back from here would start a film.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}

	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= entry_count || !entries) {
		return;
	}

	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", video_dir, entries[index].name);
	videoplay_open(path);
}

static void rebuild_rows(void) {
	lv_obj_clean(list_box);

	for (int i = 0; i < entry_count; i++) {
		// Shown without the extension: every row would end in the same four
		// characters. tr() passes a file name through unchanged.
		char shown[NAME_MAX_LEN];
		snprintf(shown, sizeof(shown), "%s", entries[i].name);
		char *dot = strrchr(shown, '.');
		if (dot && dot != shown) {
			*dot = '\0';
		}
		lv_obj_t *row = settingsrow_action(list_box, shown, open_cb, (void *)(intptr_t)i);
		settingsrow_name_lines(row, 2);
	}

	if (entry_count > 0) {
		lv_obj_set_hidden(status_label, true);
		return;
	}
	lv_label_set_text(status_label, tr(gui_card_available() ? "video_empty_note" : "video_no_card"));
	lv_obj_set_hidden(status_label, false);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	// Read again on every entry: the card may have been filled elsewhere, or
	// taken out.
	scan();
	rebuild_rows();
}

void videopage_init(gui_config_t *cfg) {
	if (cfg->sd_root_path && cfg->sd_root_path[0]) {
		snprintf(video_dir, sizeof(video_dir), "%s/%s", cfg->sd_root_path, VIDEO_DIR);
	}

	lv_obj_add_style(videopage_screen, &theme_style_screen, 0);
	settingsrow_title(videopage_screen, cfg, "video");

	int content_top = settingsrow_content_top(cfg);

	list_box = lv_obj_create(videopage_screen);
	lv_obj_set_size(list_box, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(list_box, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(list_box, 0, 0);
	lv_obj_set_style_border_width(list_box, 0, 0);
	lv_obj_set_style_radius(list_box, 0, 0);
	lv_obj_set_style_pad_hor(list_box, cfg->padding, 0);
	lv_obj_set_style_pad_top(list_box, 0, 0);
	lv_obj_set_style_pad_bottom(list_box, cfg->padding, 0);
	lv_obj_set_style_pad_gap(list_box, 10, 0);
	lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(list_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
	switcher_attach_back_gesture(list_box);

	status_label = lv_label_create(videopage_screen);
	lv_label_set_text(status_label, "");
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(status_label, lv_pct(80));
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_20, 0);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_center(status_label);

	lv_obj_add_event_cb(videopage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(videopage_screen);
}
