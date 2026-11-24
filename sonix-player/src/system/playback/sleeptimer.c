#include "sleeptimer.h"

#include <stdio.h>
#include <time.h>

#include "src/system/core/config.h"
#include "src/system/device/clock.h"

// A whole day, which is the most the two wheels can be set to and well past
// anything anybody means by a sleep timer. The clamp is here so a hand-edited
// config cannot arm something that never expires.
#define SLEEP_MAX_MINUTES (23 * 60 + 59)

// Where each one is written down. The audiobook pair are the keys the audiobook
// timer has always used, so an existing config keeps its switch and its length.
// A length of nought there means no time chosen: "at the end of the chapter" is
// a switch of its own on the audiobook page, not a timer length.
static const struct {
	const char *section;
	const char *key_on;
	const char *key_minutes;
	const char *name;
} PROFILES[SLEEPTIMER_COUNT] = {
	[SLEEPTIMER_MUSIC] = {"player", "sleep_timer", "sleep_minutes", "music"},
	[SLEEPTIMER_AUDIOBOOK] = {"audiobook", "sleep_timer", "sleep_minutes", "audiobook"},
	[SLEEPTIMER_PODCAST] = {"podcast", "sleep_timer", "sleep_minutes", "podcast"},
};

// The automatic timer: its switch and its window, in minutes after midnight.
// Kept beside the music timer in the config because that is the page it is set
// on; it applies to all three kinds.
#define AUTO_SECTION "player"
#define AUTO_KEY_ON "sleep_auto"
#define AUTO_KEY_START "sleep_auto_start"
#define AUTO_KEY_END "sleep_auto_end"
#define AUTO_DEFAULT_START (22 * 60)
#define AUTO_DEFAULT_END (6 * 60)
#define DAY_MINUTES (24 * 60)

// What the automatic timer counts for a kind whose own length is nought: the
// wheels of a timer that was never switched on by hand are still at 00:00, and
// an automatic timer that armed nothing for that reason would look broken.
#define AUTO_FALLBACK_MINUTES 30

static bool auto_on;
static int auto_start = AUTO_DEFAULT_START;
static int auto_end = AUTO_DEFAULT_END;

// Set from whichever thread saw the button, read and cleared on the interface
// thread by the tick.
static bool activity_pending;

typedef struct {
	bool enabled;
	int minutes;
	bool counting;
	bool automatic; // the stretch counting was armed by the window, not by the switch
	long long remaining_ms;
	long long last_ms;
} timer_state_t;

static timer_state_t timers[SLEEPTIMER_COUNT];

// Which kind is playing, kept apart from the countdowns: "something is playing"
// and "a stretch is running" are not the same thing, and folding them into one
// flag is how switching the option off and on again while the music played
// armed nothing at all -- the first call cleared the flag the second one needed
// to read.
static sleeptimer_kind_t playing_kind = SLEEPTIMER_COUNT;

static bool in_range(sleeptimer_kind_t kind) { return kind >= 0 && kind < SLEEPTIMER_COUNT; }

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int clamp_minutes(int minutes) {
	if (minutes < 0) {
		return 0;
	}
	return minutes > SLEEP_MAX_MINUTES ? SLEEP_MAX_MINUTES : minutes;
}

static int clamp_day_minute(int minute) { return (minute < 0 || minute >= DAY_MINUTES) ? 0 : minute; }

// True while the automatic timer has something to say: switched on, and the
// local time inside the window. The start is inside and the end is not.
static bool auto_window_open(void) {
	if (!auto_on || !clock_is_set()) {
		return false;
	}
	time_t now = time(NULL);
	struct tm local;
	if (!localtime_r(&now, &local)) {
		return false;
	}
	int minute = local.tm_hour * 60 + local.tm_min;
	if (auto_start == auto_end) {
		return true; // no gap between the two: the whole day
	}
	if (auto_start < auto_end) {
		return minute >= auto_start && minute < auto_end;
	}
	return minute >= auto_start || minute < auto_end; // across midnight
}

void sleeptimer_init(void) {
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		timers[i].enabled = config_get_int(PROFILES[i].section, PROFILES[i].key_on, 0) != 0;
		timers[i].minutes = clamp_minutes((int)config_get_int(PROFILES[i].section, PROFILES[i].key_minutes, 0));
		timers[i].counting = false;
		timers[i].automatic = false;
		timers[i].remaining_ms = 0;
	}
	auto_on = config_get_int(AUTO_SECTION, AUTO_KEY_ON, 0) != 0;
	auto_start = clamp_day_minute((int)config_get_int(AUTO_SECTION, AUTO_KEY_START, AUTO_DEFAULT_START));
	auto_end = clamp_day_minute((int)config_get_int(AUTO_SECTION, AUTO_KEY_END, AUTO_DEFAULT_END));
	playing_kind = SLEEPTIMER_COUNT;
}

bool sleeptimer_enabled(sleeptimer_kind_t kind) { return in_range(kind) && timers[kind].enabled; }

int sleeptimer_minutes(sleeptimer_kind_t kind) { return in_range(kind) ? timers[kind].minutes : 0; }

// Armed means there is a stretch to count: switched on and longer than nothing.
static bool armed(sleeptimer_kind_t kind) { return timers[kind].enabled && timers[kind].minutes > 0; }

// Starts a full stretch, or stops counting, from the state as it stands. Every
// change goes through here, so there is one place that decides and one place
// where `remaining_ms` is ever set.
//
// The switch comes first: a kind armed by hand counts as it always did, and
// the window is only asked about the kind that is playing and is not.
static void restart(sleeptimer_kind_t kind) {
	timer_state_t *t = &timers[kind];
	int minutes = t->minutes;
	t->counting = playing_kind == kind && armed(kind);
	t->automatic = false;
	if (playing_kind == kind && !t->counting && auto_window_open()) {
		t->counting = true;
		t->automatic = true;
		if (minutes <= 0) {
			minutes = AUTO_FALLBACK_MINUTES;
		}
	}
	t->remaining_ms = t->counting ? (long long)minutes * 60000 : 0;
	t->last_ms = now_ms();
	// A press from before this stretch says nothing about it.
	__atomic_store_n(&activity_pending, false, __ATOMIC_SEQ_CST);
}

void sleeptimer_set_enabled(sleeptimer_kind_t kind, bool on) {
	if (!in_range(kind) || on == timers[kind].enabled) {
		return;
	}
	timers[kind].enabled = on;
	config_set_int(PROFILES[kind].section, PROFILES[kind].key_on, on ? 1 : 0);
	config_save();
	// Switching it off in the middle of a stretch disarms it now, and switching
	// it on while something is already playing starts one: otherwise the setting
	// would only take effect at the next pause.
	restart(kind);
}

void sleeptimer_set_minutes(sleeptimer_kind_t kind, int minutes) {
	if (!in_range(kind)) {
		return;
	}
	minutes = clamp_minutes(minutes);
	if (minutes == timers[kind].minutes) {
		return;
	}
	timers[kind].minutes = minutes;
	config_set_int(PROFILES[kind].section, PROFILES[kind].key_minutes, minutes);
	config_save();
	// Moving the wheel is choosing a new stretch, not editing the one running:
	// whoever just set thirty minutes means thirty minutes from now.
	restart(kind);
}

void sleeptimer_note_playing(sleeptimer_kind_t kind) {
	if (!in_range(kind)) {
		kind = SLEEPTIMER_COUNT; // nothing any of them counts
	}
	if (kind == playing_kind) {
		// Nothing changed in the player, but the window opens and closes on
		// the wall clock: a kind not armed by hand starts counting when it
		// opens over what is already playing, and stops when it closes.
		if (in_range(kind) && !armed(kind) && timers[kind].counting != auto_window_open()) {
			restart(kind);
			fprintf(stderr, "sleeptimer: %s, automatic %s\n", PROFILES[kind].name, timers[kind].counting ? "on" : "off");
		}
		return;
	}
	playing_kind = kind;
	// All of them, not only the one named: whatever stopped playing has a
	// countdown to put away, and going from an album to a podcast is both at
	// once.
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		restart((sleeptimer_kind_t)i);
		if (timers[i].counting) {
			fprintf(stderr, "sleeptimer: %s, %d min\n", PROFILES[i].name, timers[i].minutes);
		}
	}
}

bool sleeptimer_running(sleeptimer_kind_t kind) { return in_range(kind) && timers[kind].counting; }

bool sleeptimer_any_running(void) {
	for (int i = 0; i < SLEEPTIMER_COUNT; i++) {
		if (timers[i].counting) {
			return true;
		}
	}
	return false;
}

bool sleeptimer_expired(sleeptimer_kind_t kind) {
	if (!sleeptimer_running(kind)) {
		return false;
	}
	timer_state_t *t = &timers[kind];

	// A button pressed since the last tick: whoever pressed it is awake, and
	// the automatic stretch starts over. Looked at before the time, so a press
	// in the last seconds is not answered with a pause.
	if (t->automatic && __atomic_exchange_n(&activity_pending, false, __ATOMIC_SEQ_CST)) {
		restart(kind);
		return false;
	}

	long long now = now_ms();
	long long elapsed = now - t->last_ms;
	t->last_ms = now;
	if (elapsed < 0) {
		elapsed = 0; // the clock is monotonic, but do not trust a negative step
	}

	if (elapsed >= t->remaining_ms) {
		t->counting = false;
		t->remaining_ms = 0;
		fprintf(stderr, "sleeptimer: %s, time is up; stopping\n", PROFILES[kind].name);
		return true;
	}
	t->remaining_ms -= elapsed;
	return false;
}

int sleeptimer_remaining(sleeptimer_kind_t kind) {
	if (!sleeptimer_running(kind)) {
		return 0;
	}
	return (int)((timers[kind].remaining_ms + 999) / 1000);
}

bool sleeptimer_auto_enabled(void) { return auto_on; }

int sleeptimer_auto_start(void) { return auto_start; }

int sleeptimer_auto_end(void) { return auto_end; }

// The setting takes effect now for what is playing, as the switch of a kind
// does. A kind armed by hand has nothing to do with the window and keeps the
// stretch it is on.
static void auto_changed(void) {
	if (in_range(playing_kind) && !armed(playing_kind)) {
		restart(playing_kind);
	}
}

void sleeptimer_set_auto_enabled(bool on) {
	if (on == auto_on) {
		return;
	}
	auto_on = on;
	config_set_int(AUTO_SECTION, AUTO_KEY_ON, on ? 1 : 0);
	config_save();
	auto_changed();
}

void sleeptimer_set_auto_window(int start_minute, int end_minute) {
	start_minute = clamp_day_minute(start_minute);
	end_minute = clamp_day_minute(end_minute);
	if (start_minute == auto_start && end_minute == auto_end) {
		return;
	}
	auto_start = start_minute;
	auto_end = end_minute;
	config_set_int(AUTO_SECTION, AUTO_KEY_START, start_minute);
	config_set_int(AUTO_SECTION, AUTO_KEY_END, end_minute);
	config_save();
	auto_changed();
}

void sleeptimer_note_activity(void) { __atomic_store_n(&activity_pending, true, __ATOMIC_SEQ_CST); }
