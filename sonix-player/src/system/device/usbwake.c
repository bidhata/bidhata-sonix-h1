// See usbwake.h. These helpers are pure on purpose: the RTC slot is shared
// between the shutdown deadline, the battery check and the cable check, and
// the arbitration is easier to pin down when it has no side effects.

#include "usbwake.h"

uint32_t usbwake_default_seconds(bool pmic_charger) {
	// Off on both players, for now, whatever the charger is.
	//
	// The check needs the R1 to wake from "mem" and carry on running, and that
	// resets the board: left to sleep unplugged with the check on, it comes
	// back up about thirty seconds later, over and over. Turning the check off
	// again leaves standby working, which is what pins it on this.
	//
	// It is not this feature's fault. The device log holds exactly one
	// successful wake from "mem" in its whole history, and that one powered
	// off immediately afterwards, as the automatic shutdown is meant to --
	// nothing has ever woken this board and gone on using it. The cable check
	// is simply the first thing to ask for it.
	//
	// The setting stays on the Power page so the resume can be worked on and
	// measured. When a wake is shown to survive, this goes back to 30 for a
	// PMIC-charged player.
	(void)pmic_charger;
	return 0u;
}

bool usbwake_interval_valid(uint32_t seconds) {
	return seconds == 0u || seconds == 15u || seconds == 30u || seconds == 60u;
}

usbwake_reason_t usbwake_pick(long auto_off_s, long battery_s, long usb_s,
							 long *out_s) {
	usbwake_reason_t reason = USBWAKE_NONE;
	long best_s = 0;

	if (auto_off_s > 0) {
		best_s = auto_off_s;
		reason = USBWAKE_AUTO_OFF;
	}

	// Strictly smaller wins; a zero means the check is disabled. Equality
	// keeps the earlier reason, so a shutdown deadline on a check boundary
	// still wins the slot.
	if (battery_s > 0 && (best_s == 0 || battery_s < best_s)) {
		best_s = battery_s;
		reason = USBWAKE_BATTERY;
	}
	if (usb_s > 0 && (best_s == 0 || usb_s < best_s)) {
		best_s = usb_s;
		reason = USBWAKE_USB_CHECK;
	}

	if (out_s) {
		*out_s = best_s;
	}
	return reason;
}

uint32_t usbwake_mem_active_after_wake(uint32_t now_ms, uint32_t mem_after_ms,
									   bool usb_check_wake, bool cable_found) {
	if (usb_check_wake && !cable_found && mem_after_ms > USBWAKE_AWAKE_MS) {
		return now_ms - (mem_after_ms - USBWAKE_AWAKE_MS);
	}
	return now_ms;
}
