#include "cardpage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/settings/fwupdate.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/device/cardtest.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"

lv_obj_t *cardpage_screen;

#define DASH "\xE2\x80\x94" // an em dash, where there is nothing to say

static lv_obj_t *used_value, *free_value, *fs_value, *state_value;
static lv_obj_t *unclean_note;
static lv_obj_t *speed_note; // what the test does, then what it found
static lv_obj_t *format_row, *format_note;

// A test or a format is running on its own thread. Both rows wait for it: the
// card can do one thing at a time, and a second format of a card that is being
// formatted is not a question worth asking.
static bool working;
// What stands under the speed row once a test has run, kept so that a refresh
// of the page does not wipe it: the figures written out, or a language key
// when the test did not give any. Empty before the first test.
static char speed_text[96];
static bool speed_text_is_key;

// ---------------------------------------------------------------------------
// what the page says
// ---------------------------------------------------------------------------

// The kernel's name for a filesystem as the name people know it by. vfat is
// every FAT there is; what the device formats, and what a card of any size
// sold today comes as when it is FAT at all, is FAT32.
static const char *fs_name(const char *type) {
	if (strcmp(type, "vfat") == 0 || strcmp(type, "msdos") == 0) {
		return "FAT32";
	}
	if (strcmp(type, "exfat") == 0) {
		return "exFAT";
	}
	if (strcmp(type, "fuseblk") == 0 || strncmp(type, "ntfs", 4) == 0) {
		return "NTFS";
	}
	return type;
}

static void refresh(void) {
	if (!state_value) {
		return;
	}

	bool with_computer = usb_storage_active();
	sysinfo_storage_t usage;
	bool present = !with_computer && storage_card_usable() && sysinfo_sd_usage(&usage) && usage.present;

	if (!present) {
		lv_label_set_text(used_value, DASH);
		lv_label_set_text(free_value, DASH);
		lv_label_set_text(fs_value, DASH);
		lv_label_set_text(state_value, tr(with_computer ? "cardpage_state_computer" : "system_no_card"));
		lv_obj_set_hidden(unclean_note, true);
	} else {
		char used[32], total[32], text[96];
		sysinfo_format_size(usage.used, used, sizeof(used));
		sysinfo_format_size(usage.total, total, sizeof(total));
		snprintf(text, sizeof(text), "%s / %s", used, total);
		lv_label_set_text(used_value, text);
		sysinfo_format_size(usage.free, text, sizeof(text));
		lv_label_set_text(free_value, text);

		char type[64];
		lv_label_set_text(fs_value, storage_card_fstype(type, sizeof(type)) ? fs_name(type) : DASH);

		bool unclean = storage_card_unclean() == 1;
		const char *state = !storage_sd_writable() ? "cardpage_state_readonly"
							: unclean			   ? "cardpage_state_unclean"
												   : "cardpage_state_ok";
		lv_label_set_text(state_value, tr(state));
		lv_obj_set_hidden(unclean_note, !unclean);
	}

	if (working) {
		return; // the row that is working says so itself
	}
	lv_label_set_text(speed_note, !speed_text[0] ? tr("cardpage_speed_note") : speed_text_is_key ? tr(speed_text) : speed_text);
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

// ---------------------------------------------------------------------------
// the speed test
// ---------------------------------------------------------------------------

static void test_done_cb(void *user) {
	cardtest_result_t *result = user;
	working = false;
	speed_text_is_key = result->status != CARDTEST_OK;

	switch (result->status) {
	case CARDTEST_OK: {
		// One decimal of megabytes a second, written without floating point.
		char w[16], r[16];
		snprintf(w, sizeof(w), "%d.%d", result->write_kb_s / 1000, result->write_kb_s % 1000 / 100);
		snprintf(r, sizeof(r), "%d.%d", result->read_kb_s / 1000, result->read_kb_s % 1000 / 100);
		snprintf(speed_text, sizeof(speed_text), tr("cardpage_speed_result"), w, r);
		break;
	}
	case CARDTEST_NO_CARD:
		snprintf(speed_text, sizeof(speed_text), "%s", "system_no_card");
		break;
	case CARDTEST_NO_ROOM:
		snprintf(speed_text, sizeof(speed_text), "%s", "cardpage_speed_no_room");
		break;
	case CARDTEST_MISMATCH:
		snprintf(speed_text, sizeof(speed_text), "%s", "cardpage_speed_mismatch");
		break;
	case CARDTEST_IO_ERROR:
	default:
		snprintf(speed_text, sizeof(speed_text), "%s", "cardpage_speed_failed");
		break;
	}
	free(result);
	refresh();
}

static void *test_thread(void *arg) {
	cardtest_result_t *result = arg;
	cardtest_run(result);
	if (!gui_post(test_done_cb, result)) {
		free(result); // the interface is gone; so is the page
	}
	return NULL;
}

static void speed_clicked_cb(lv_event_t *e) {
	(void)e;
	if (working) {
		return;
	}
	cardtest_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		return;
	}
	working = true;
	lv_label_set_text(speed_note, tr("cardpage_testing"));

	pthread_t thread;
	if (pthread_create(&thread, NULL, test_thread, result) != 0) {
		free(result);
		working = false;
		snprintf(speed_text, sizeof(speed_text), "%s", "cardpage_speed_failed");
		speed_text_is_key = true;
		refresh();
		return;
	}
	pthread_detach(thread);
}

// ---------------------------------------------------------------------------
// formatting
//
// Two confirmations in a row, as for the factory reset and for the same
// reason: the second, which says what is lost, only appears once the first
// has closed, so the two cannot be tapped through in one motion.
// ---------------------------------------------------------------------------

static void format_done_cb(void *user) {
	storage_format_result_t result = (storage_format_result_t)(intptr_t)user;
	working = false;
	speed_text[0] = '\0'; // a figure for the card as it was
	refresh();

	const char *message = "cardpage_format_failed";
	switch (result) {
	case STORAGE_FORMAT_OK:
		message = "cardpage_format_done";
		break;
	case STORAGE_FORMAT_NO_CARD:
		message = "system_no_card";
		break;
	case STORAGE_FORMAT_BUSY:
		message = "cardpage_format_busy";
		break;
	case STORAGE_FORMAT_NOT_MOUNTED:
		message = "cardpage_format_not_mounted";
		break;
	case STORAGE_FORMAT_UNSUPPORTED:
	case STORAGE_FORMAT_FAILED:
	default:
		break;
	}
	confirm_notice("cardpage_format", message);
}

static void *format_thread(void *arg) {
	(void)arg;
	storage_format_result_t result = storage_format_card();
	// The result travels in the pointer itself, so there is nothing to free
	// if the interface is gone and the post is refused.
	gui_post(format_done_cb, (void *)(intptr_t)result);
	return NULL;
}

static void format_second_confirmed(void *user) {
	(void)user;
	if (working) {
		return;
	}
	working = true;
	lv_label_set_text(state_value, tr("cardpage_formatting"));

	pthread_t thread;
	if (pthread_create(&thread, NULL, format_thread, NULL) != 0) {
		working = false;
		refresh();
		confirm_notice("cardpage_format", "cardpage_format_failed");
		return;
	}
	pthread_detach(thread);
}

static void format_first_confirmed(void *user) {
	(void)user;
	confirm_show("system_are_you_sure", "cardpage_format_confirm_2", "cardpage_format_button", format_second_confirmed,
				 NULL);
}

static void format_clicked_cb(lv_event_t *e) {
	(void)e;
	if (working) {
		return;
	}
	if (usb_storage_active() || !storage_card_usable()) {
		confirm_notice("cardpage_format", usb_storage_active() ? "cardpage_state_computer" : "system_no_card");
		return;
	}
	// The same bar as a firmware update: a card left half formatted by a
	// battery that gave out is a card that does not mount.
	if (!fwupdate_battery_ok()) {
		gui_notify_popup("system_battery_warning");
		return;
	}
	confirm_show("cardpage_format", "cardpage_format_confirm", "system_continue", format_first_confirmed, NULL);
}

// ---------------------------------------------------------------------------
// building the page
// ---------------------------------------------------------------------------

static lv_obj_t *info_row(lv_obj_t *parent, const char *name, lv_obj_t **value_out) {
	lv_obj_t *row = settingsrow_add(parent, name, value_out, NULL, NULL);
	lv_obj_set_clickable(row, false);
	return row;
}

static lv_obj_t *note(lv_obj_t *parent, const char *key) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_label_set_text(label, tr(key));
	return label;
}

void cardpage_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(cardpage_screen, cfg, "system_sd_card");

	info_row(container, "cardpage_used", &used_value);
	info_row(container, "cardpage_free", &free_value);
	info_row(container, "cardpage_filesystem", &fs_value);
	info_row(container, "cardpage_state", &state_value);
	unclean_note = note(container, "cardpage_unclean_note");
	lv_obj_set_hidden(unclean_note, true);

	// Actions: no chevron. The line under the test says what it does, and
	// then what it found: two speeds do not fit beside the row's name.
	settingsrow_action(container, "cardpage_speed_test", speed_clicked_cb, NULL);
	speed_note = note(container, "cardpage_speed_note");

	// Only where the card can be formatted at all: not on the simulator, whose
	// card is a folder, and not on a firmware without the tool.
	format_row = settingsrow_action(container, "cardpage_format", format_clicked_cb, NULL);
	format_note = note(container, "cardpage_format_note");
	if (!storage_format_supported()) {
		lv_obj_set_hidden(format_row, true);
		lv_obj_set_hidden(format_note, true);
	}

	refresh();
	lv_obj_add_event_cb(cardpage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(cardpage_screen);
	theme_register_refresh(refresh);
}
