#include "powersettings.h"

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/axpcharge.h"
#include "src/system/device/led.h"
#include "src/system/device/power.h"
#include "src/system/device/system.h"

lv_obj_t *powersettings_screen;

// 80 to 100 in fives. Stopping short of full is what keeps a lithium cell
// healthy if the device mostly lives on the charger.
#define CHARGE_LIMIT_MIN 80
#define CHARGE_LIMIT_STEP 5
#define CHARGE_LIMIT_COUNT 5 // 80, 85, 90, 95, 100

static const struct {
	int minutes;
	const char *label;
} AUTO_OFF[] = {
	{15, "power_15_minutes"}, {30, "power_30_minutes"}, {60, "power_1_hour"}, {120, "power_2_hours"}, {180, "power_3_hours"}, {360, "power_6_hours"},
	{720, "power_12_hours"}, {1440, "power_24_hours"}, {2880, "power_48_hours"},
};
#define AUTO_OFF_COUNT ((int)(sizeof(AUTO_OFF) / sizeof(AUTO_OFF[0])))

// How long the screen stays dark before standby. Saved in seconds under
// [power] standby_mem_seconds, which power.c reads at startup.
static const struct {
	int seconds;
	const char *label;
} STANDBY_AFTER[] = {
	{60, "power_1_minute"}, {180, "power_3_minutes"}, {300, "power_5_minutes"}, {600, "power_10_minutes"},
};
#define STANDBY_AFTER_COUNT ((int)(sizeof(STANDBY_AFTER) / sizeof(STANDBY_AFTER[0])))

// How often the R1 wakes from standby to look for a USB cable, so storage can
// appear while the screen is dark. Saved in seconds under
// [power] usb_wake_check_seconds; 0 turns the check off.
static const struct {
	int seconds;
	const char *label;
} USB_WAKE_AFTER[] = {
	{0, "power_usb_wake_off"}, {15, "power_usb_wake_15s"}, {30, "power_usb_wake_30s"}, {60, "power_usb_wake_60s"},
};
#define USB_WAKE_AFTER_COUNT ((int)(sizeof(USB_WAKE_AFTER) / sizeof(USB_WAKE_AFTER[0])))

// The R1's default interval, and the one a switch-on lands on when the slider
// was left on "Off".
#define USB_WAKE_DEFAULT_INDEX 2

// The standby card while standby is off: as low as a toggle with pills under
// it when they are folded away ("Split artists" in the music settings), the
// switch and the card's padding and nothing else.
#define STANDBY_CLOSED_H 76

static lv_obj_t *charge_value, *charge_slider;
static lv_obj_t *auto_off_switch, *auto_off_value, *auto_off_slider, *auto_off_card;
static lv_obj_t *led_on_switch, *led_off_switch, *led_off_card;
static lv_obj_t *key_lock_switch;
static lv_obj_t *standby_switch, *standby_value, *standby_slider, *standby_card;
static lv_obj_t *usb_wake_switch, *usb_wake_value, *usb_wake_slider, *usb_wake_card;
static lv_obj_t *charge_note;
static lv_obj_t *axp_limit_switch, *axp_current_switch; // the R1's two, instead of the slider

// --- reading the saved values ---

static int charge_index(void) {
	int percent = (int)config_get_int("power", "charge_limit", 100);
	int index = (percent - CHARGE_LIMIT_MIN) / CHARGE_LIMIT_STEP;
	if (index < 0) {
		index = 0;
	}
	if (index >= CHARGE_LIMIT_COUNT) {
		index = CHARGE_LIMIT_COUNT - 1;
	}
	return index;
}

static int auto_off_index(void) {
	int minutes = (int)config_get_int("power", "auto_off_minutes", 30);
	for (int i = 0; i < AUTO_OFF_COUNT; i++) {
		if (AUTO_OFF[i].minutes == minutes) {
			return i;
		}
	}
	return 1;
}

// A wait saved by hand that is not one of the four lands on the next one up,
// or on the longest.
static int standby_after_index(void) {
	int seconds = (int)config_get_int("power", "standby_mem_seconds", STANDBY_AFTER[0].seconds);
	for (int i = 0; i < STANDBY_AFTER_COUNT; i++) {
		if (seconds <= STANDBY_AFTER[i].seconds) {
			return i;
		}
	}
	return STANDBY_AFTER_COUNT - 1;
}

// The first offered interval that is at least the saved one, or the longest
// when the saved value is past the table. On a player without the PMIC charger
// the key is absent and the answer is off, which keeps its path unchanged.
static int usb_wake_after_index(void) {
	long fallback = axpcharge_applies() ? USB_WAKE_AFTER[USB_WAKE_DEFAULT_INDEX].seconds : 0;
	int seconds = (int)config_get_int("power", "usb_wake_check_seconds", fallback);
	for (int i = 0; i < USB_WAKE_AFTER_COUNT; i++) {
		if (seconds <= USB_WAKE_AFTER[i].seconds) {
			return i;
		}
	}
	return USB_WAKE_AFTER_COUNT - 1;
}

// --- applying them ---

static void set_checked(lv_obj_t *sw, bool on) {
	if (on) {
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(sw, LV_STATE_CHECKED);
	}
}

void powersettings_apply(void) {
	if (axpcharge_applies()) {
		// The R1 has no percentage slider: a percentage limit saved on it
		// becomes the 80 % switch.
		if (config_get_int("power", "charge_limit", 100) < 100) {
			config_set_int("power", "charge_limit", 100);
			config_set_bool("power", "charge_limit_80", true);
			config_save();
		}
		power_set_charge_limit(100);
		axpcharge_set(config_get_bool("power", "charge_limit_80", false),
					  config_get_bool("power", "charge_500ma", false));
	} else {
		power_set_charge_limit(CHARGE_LIMIT_MIN + (charge_index() * CHARGE_LIMIT_STEP));
	}

	power_set_auto_off(config_get_bool("power", "auto_off", false), (uint32_t)AUTO_OFF[auto_off_index()].minutes);

	// Suspend-to-RAM standby, on by default: saves battery by suspending the
	// player shortly after the screen goes off.
	power_set_standby_enabled(config_get_bool("power", "standby_mem", true));
	power_set_standby_after((uint32_t)STANDBY_AFTER[standby_after_index()].seconds);

	// The cable check is a standby-only wake, so it follows the same saved
	// interval as the slider; 0 leaves it off.
	power_set_usb_wake_check((uint32_t)USB_WAKE_AFTER[usb_wake_after_index()].seconds);

	// The LED master switch comes first: with it off there is nothing for the
	// standby option to turn off, and led.c keeps the charge indicator alive
	// on its own regardless.
	led_set_enabled(config_get_bool("power", "led_on", true));
	led_set_idle_off(config_get_bool("power", "led_off_standby", true));

	input_set_key_lock(config_get_bool("power", "lock_keys_screen_off", false));
}

static void refresh_labels(void) {
	if (charge_value) {
		lv_label_set_text_fmt(charge_value, "%d%%", CHARGE_LIMIT_MIN + (charge_index() * CHARGE_LIMIT_STEP));
	}
	if (axp_limit_switch) {
		set_checked(axp_limit_switch, config_get_bool("power", "charge_limit_80", false));
		set_checked(axp_current_switch, config_get_bool("power", "charge_500ma", false));
	}
	lv_label_set_text(auto_off_value, tr(AUTO_OFF[auto_off_index()].label));

	// The LED master switch, and the standby option that hangs off it: with the
	// LED switched off entirely there is nothing left for the standby option to
	// do, so it greys out.
	bool led_on = config_get_bool("power", "led_on", true);
	if (led_on) {
		lv_obj_add_state(led_on_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(led_on_switch, LV_STATE_CHECKED);
	}

	if (config_get_bool("power", "led_off_standby", true)) {
		lv_obj_add_state(led_off_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(led_off_switch, LV_STATE_CHECKED);
	}

	if (led_on) {
		lv_obj_remove_state(led_off_switch, LV_STATE_DISABLED);
		lv_obj_set_style_opa(led_off_card, LV_OPA_COVER, 0);
	} else {
		lv_obj_add_state(led_off_switch, LV_STATE_DISABLED);
		lv_obj_set_style_opa(led_off_card, LV_OPA_40, 0);
	}

	set_checked(key_lock_switch, config_get_bool("power", "lock_keys_screen_off", false));

	// Standby and its wait, on one card like the automatic power-off below.
	bool standby = config_get_bool("power", "standby_mem", true);
	if (standby) {
		lv_obj_add_state(standby_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(standby_switch, LV_STATE_CHECKED);
	}
	lv_label_set_text(standby_value, tr(STANDBY_AFTER[standby_after_index()].label));
	settingsrow_toggle_slider_expanded(standby_card, standby);
	if (!standby) {
		lv_obj_set_height(standby_card, STANDBY_CLOSED_H);
	}

	// The cable check, like standby, keeps its interval on a slider that is
	// only meaningful while the switch is on. A saved 0 is off. The row only
	// exists on the player the check applies to, so there may be nothing here
	// to paint.
	if (usb_wake_card) {
		int usb_wake_index = usb_wake_after_index();
		bool usb_wake = USB_WAKE_AFTER[usb_wake_index].seconds > 0;
		if (usb_wake) {
			lv_obj_add_state(usb_wake_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(usb_wake_switch, LV_STATE_CHECKED);
		}
		lv_label_set_text(usb_wake_value, tr(USB_WAKE_AFTER[usb_wake_index].label));
		settingsrow_toggle_slider_expanded(usb_wake_card, usb_wake);
	}

	// One card: the slider is only on it while the toggle is on.
	bool auto_off = config_get_bool("power", "auto_off", false);
	if (auto_off) {
		lv_obj_add_state(auto_off_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(auto_off_switch, LV_STATE_CHECKED);
	}
	settingsrow_toggle_slider_expanded(auto_off_card, auto_off);
}

static void charge_changed_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(charge_slider);
	config_set_int("power", "charge_limit", CHARGE_LIMIT_MIN + (index * CHARGE_LIMIT_STEP));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void axp_limit_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "charge_limit_80", lv_obj_has_state(axp_limit_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void axp_current_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "charge_500ma", lv_obj_has_state(axp_current_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void auto_off_changed_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(auto_off_slider);
	config_set_int("power", "auto_off_minutes", AUTO_OFF[index].minutes);
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void led_off_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "led_off_standby", lv_obj_has_state(led_off_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

// The master switch. With the LED off the standby option has nothing left to
// do, so it greys out -- and the charger still lights the LED regardless, so
// a charge in progress is never invisible.
static void led_on_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "led_on", lv_obj_has_state(led_on_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void key_lock_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "lock_keys_screen_off", lv_obj_has_state(key_lock_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void auto_off_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "auto_off", lv_obj_has_state(auto_off_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void standby_after_changed_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(standby_slider);
	config_set_int("power", "standby_mem_seconds", STANDBY_AFTER[index].seconds);
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void standby_toggled_cb(lv_event_t *e) {
	(void)e;
	config_set_bool("power", "standby_mem", lv_obj_has_state(standby_switch, LV_STATE_CHECKED));
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void usb_wake_changed_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(usb_wake_slider);
	config_set_int("power", "usb_wake_check_seconds", USB_WAKE_AFTER[index].seconds);
	config_save();
	powersettings_apply();
	refresh_labels();
}

static void usb_wake_toggled_cb(lv_event_t *e) {
	(void)e;
	int index = (int)lv_slider_get_value(usb_wake_slider);
	int seconds = 0;
	if (lv_obj_has_state(usb_wake_switch, LV_STATE_CHECKED)) {
		// Switching on from "Off" would write 0 and leave the card looking
		// broken, so land on the usual interval and move the slider with it.
		if (USB_WAKE_AFTER[index].seconds == 0) {
			index = USB_WAKE_DEFAULT_INDEX;
			lv_slider_set_value(usb_wake_slider, index, LV_ANIM_OFF);
		}
		seconds = USB_WAKE_AFTER[index].seconds;
	}
	config_set_int("power", "usb_wake_check_seconds", seconds);
	config_save();
	powersettings_apply();
	refresh_labels();
}

void powersettings_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(powersettings_screen, cfg, "power");

	// Suspend-to-RAM standby: suspends the player a while after the screen goes
	// off to save battery, and playback resumes on wake. On by default. What it
	// waits for is the screen going dark, and that timeout lives on the Screen
	// page, beside the brightness it belongs with; how long it then waits is
	// the slider on this card, there only while standby is on.
	standby_card = settingsrow_toggle_slider(container, "power_standby_mem", STANDBY_AFTER_COUNT, &standby_switch,
											 &standby_value, &standby_slider, standby_toggled_cb,
											 standby_after_changed_cb);
	lv_slider_set_value(standby_slider, standby_after_index(), LV_ANIM_OFF);
	lv_obj_t *standby_note = lv_label_create(container);
	lv_label_set_long_mode(standby_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(standby_note, lv_pct(100));
	lv_obj_add_style(standby_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(standby_note, &font_ui_22, 0);
	lv_label_set_text(standby_note, tr("power_standby_mem_note"));

	// While it sleeps, the R1 wakes briefly every so often to see whether a
	// cable arrived, so storage can show up without pressing power. The shorter
	// the wait the more often it wakes, and the more battery that costs.
	//
	// Only on the R1. The other player's MP2731 wakes the board on insert by
	// itself (see rtc_alarm_arm() in power.c), so the check would cost battery
	// and buy nothing -- and a setting that does nothing is worse than no
	// setting. The same answer picks the interval's default in power_init().
	if (axpcharge_applies()) {
		usb_wake_card = settingsrow_toggle_slider(container, "power_usb_wake", USB_WAKE_AFTER_COUNT, &usb_wake_switch,
												  &usb_wake_value, &usb_wake_slider, usb_wake_toggled_cb,
												  usb_wake_changed_cb);
		lv_slider_set_value(usb_wake_slider, usb_wake_after_index(), LV_ANIM_OFF);
		lv_obj_t *usb_wake_note = lv_label_create(container);
		lv_label_set_long_mode(usb_wake_note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(usb_wake_note, lv_pct(100));
		lv_obj_add_style(usb_wake_note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(usb_wake_note, &font_ui_22, 0);
		lv_label_set_text(usb_wake_note, tr("power_usb_wake_note"));
	}

	// The R1 charges through its PMIC, which takes a lower target voltage and a
	// lower current rather than a percentage: two switches there, the slider
	// everywhere else.
	bool pmic = axpcharge_applies();
	if (pmic) {
		settingsrow_toggle(container, "power_charge_limit_80", &axp_limit_switch, axp_limit_toggled_cb);
		lv_obj_t *limit_note = lv_label_create(container);
		lv_label_set_long_mode(limit_note, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(limit_note, lv_pct(100));
		lv_obj_add_style(limit_note, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(limit_note, &font_ui_22, 0);
		lv_label_set_text(limit_note, tr("power_charge_limit_80_note"));
		settingsrow_toggle(container, "power_charge_500ma", &axp_current_switch, axp_current_toggled_cb);
	} else {
		settingsrow_slider(container, "power_charge_limit", CHARGE_LIMIT_COUNT, &charge_value, &charge_slider,
						   charge_changed_cb);
		lv_slider_set_value(charge_slider, charge_index(), LV_ANIM_OFF);
	}

	// The LED master switch, and the standby option that depends on it.
	settingsrow_toggle(container, "power_led_on", &led_on_switch, led_on_toggled_cb);
	led_off_card = settingsrow_toggle(container, "power_led_off_in_standby", &led_off_switch, led_off_toggled_cb);
	// No "turn Wi-Fi off in standby" entry: parking the radio is automatic, and
	// wifi_in_use() in power.c knows on its own when not to turn it off --
	// Qobuz, Tidal, podcasts, radio, AirPlay, DLNA, transfers.

	// The lock gesture for the buttons on the case (system.c).
	settingsrow_toggle(container, "power_lock_keys_screen_off", &key_lock_switch, key_lock_toggled_cb);
	lv_obj_t *key_lock_note = lv_label_create(container);
	lv_label_set_long_mode(key_lock_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(key_lock_note, lv_pct(100));
	lv_obj_add_style(key_lock_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(key_lock_note, &font_ui_22, 0);
	lv_label_set_text(key_lock_note, tr("power_lock_keys_screen_off_note"));

	// Toggle and "after how long" on one card: the slider is only there while
	// the option is on.
	auto_off_card = settingsrow_toggle_slider(container, "power_auto_power_off", AUTO_OFF_COUNT,
											  &auto_off_switch, &auto_off_value, &auto_off_slider,
											  auto_off_toggled_cb, auto_off_changed_cb);
	lv_slider_set_value(auto_off_slider, auto_off_index(), LV_ANIM_OFF);

	// If the charger driver has no control node there is nothing to enforce
	// the limit with, and saying so is better than a setting that silently
	// does nothing.
	charge_note = lv_label_create(container);
	lv_label_set_long_mode(charge_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(charge_note, lv_pct(100));
	lv_obj_add_style(charge_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(charge_note, &font_ui_22, 0);
	lv_label_set_text(charge_note, pmic || power_charge_limit_supported() ? tr("power_auto_off_note")
																	   : tr("power_charge_limit_unavailable"));

	// Everything saved becomes effective now, at startup -- including the LED
	// standby option, which lives in led.c rather than power.c.
	powersettings_apply();
	refresh_labels();
}
