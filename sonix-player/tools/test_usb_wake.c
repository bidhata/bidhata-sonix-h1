// The pure USB wake helpers, on the host. No hardware, no globals: just the
// config defaults, the RTC alarm arbitration and the awake-window calculation.
//
// Built and run by `make test-usb-wake`. Not part of host or target.

#include "src/system/device/usbwake.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

int main(void) {
	assert(usbwake_default_seconds(true) == 30);
	assert(usbwake_default_seconds(false) == 0);
	assert(usbwake_interval_valid(0) && usbwake_interval_valid(15));
	assert(usbwake_interval_valid(30) && usbwake_interval_valid(60));
	assert(!usbwake_interval_valid(10));

	long out = -1;
	assert(usbwake_pick(1200, 3600, 30, &out) == USBWAKE_USB_CHECK && out == 30);
	assert(usbwake_pick(20, 3600, 30, &out) == USBWAKE_AUTO_OFF && out == 20);
	assert(usbwake_pick(3600, 1200, 0, &out) == USBWAKE_BATTERY && out == 1200);
	assert(usbwake_pick(1200, 3600, 0, &out) == USBWAKE_AUTO_OFF && out == 1200);
	assert(usbwake_pick(0, 0, 0, &out) == USBWAKE_NONE);
	assert(usbwake_pick(30, 3600, 30, &out) == USBWAKE_AUTO_OFF);

	assert(usbwake_mem_active_after_wake(100000, 60000, true, false) == 46000);
	assert(usbwake_mem_active_after_wake(100000, 60000, true, true) == 100000);
	assert(usbwake_mem_active_after_wake(100000, 5000, true, false) == 100000);
	assert(usbwake_mem_active_after_wake(200000, 60000, false, false) == 200000);
	assert(usbwake_mem_active_after_wake(200000, 60000, true, false) == 146000);

	return 0;
}
