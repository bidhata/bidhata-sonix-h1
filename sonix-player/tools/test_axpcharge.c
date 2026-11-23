// The AXP2101 charge state machine, on the host, against the register map
// axpcharge.c keeps under HOST_BUILD (it is the same file the player builds,
// with AXPCHARGE_TEST adding the hooks).
//
// Built and run by `make test-axpcharge`. Not part of host or target.

#include "src/system/device/axpcharge.h"
#include "src/system/device/axpcharge_test.h"

#include "src/system/device/sysinfo.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// The rest of axpcharge.c's dependencies. A request reads one config key, and
// asks the model table, the cable and the gauge whether a stop is safe.
static int battery_mv = 3900;
static bool vbus_present = true;

long config_get_int(const char *section, const char *key, long fallback) {
	(void)section;
	(void)key;
	return fallback;
}

void config_set_int(const char *section, const char *key, long value) {
	(void)section;
	(void)key;
	(void)value;
}

bool config_save(void) { return true; }

const sysinfo_model_t *sysinfo_model(void) { return NULL; }

bool usb_vbus_present(void) { return vbus_present; }

int read_battery_millivolts(void) { return battery_mv; }

// The two hooks that belong with the stubs above: axpcharge_test_reset() calls
// them to put the defaults back.
void axpcharge_test_set_millivolts(int mv) { battery_mv = mv; }

void axpcharge_test_set_vbus(bool present) { vbus_present = present; }

#define CHECK(cond, name) \
	do { \
		if (cond) { \
			printf("ok   %s\n", name); \
		} else { \
			printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__); \
			exit(1); \
		} \
	} while (0)

#define REG18 0x18

// 1. The strict gate answers true on the model that has the PMIC as charger.
static void case_supported_r1(void) {
	axpcharge_test_set_applies(true);
	axpcharge_test_reset();
	axpcharge_test_set_reg(REG18, 0x0e);
	CHECK(axpcharge_charging_control_supported(), "supported_r1");
}

// 2. A stop clears bit 1 and nothing else.
static void case_stop_clears_only_bit1(void) {
	axpcharge_test_reset();
	axpcharge_test_set_reg(REG18, 0x0e);
	CHECK(axpcharge_set_charging(false), "stop_clears_only_bit1 returns true");
	CHECK(axpcharge_test_reg(REG18) == 0x0c, "stop_clears_only_bit1 clears only bit 1");
	CHECK(axpcharge_charging_blocked(), "stop_clears_only_bit1 reports blocked");
}

// 3. A resume puts the bit back, and clears the blocked report.
static void case_resume_sets_bit1(void) {
	CHECK(axpcharge_set_charging(true), "resume_sets_bit1 returns true");
	CHECK(axpcharge_test_reg(REG18) == 0x0e, "resume_sets_bit1 sets bit 1");
	CHECK(!axpcharge_charging_blocked(), "resume_sets_bit1 reports unblocked");
}

// 4. A refused stop leaves the register alone and is not retried behind the UI.
//    charge_known is made true first, so this is the kind of stop the tick
//    would retry; the failure is cleared before the tick, so a retry would
//    really clear bit 1.
static void case_refused_off_reverts(void) {
	axpcharge_test_reset();
	axpcharge_test_set_reg(REG18, 0x0e);
	CHECK(axpcharge_set_charging(false), "refused_off_reverts stops first");
	CHECK(axpcharge_set_charging(true), "refused_off_reverts resumes first");
	axpcharge_test_fail_write(true);
	CHECK(!axpcharge_set_charging(false), "refused_off_reverts returns false");
	CHECK(axpcharge_test_reg(REG18) == 0x0e, "refused_off_reverts leaves the bit");
	CHECK(!axpcharge_charging_blocked(), "refused_off_reverts not blocked");
	axpcharge_test_fail_write(false);
	axpcharge_test_advance_ms(6000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x0e, "refused_off_reverts not retried by the tick");
}

// 5. A refused resume leaves the stop in place and is not retried either.
static void case_refused_on_reverts(void) {
	axpcharge_test_reset();
	CHECK(axpcharge_set_charging(false), "refused_on_reverts stops first");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "refused_on_reverts starts stopped");
	axpcharge_test_fail_write(true);
	CHECK(!axpcharge_set_charging(true), "refused_on_reverts returns false");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "refused_on_reverts leaves the bit clear");
	CHECK(axpcharge_charging_blocked(), "refused_on_reverts still blocked");
	axpcharge_test_fail_write(false);
	axpcharge_test_advance_ms(6000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x08, "refused_on_reverts not retried by the tick");
}

// 6. Unlike a user request, a failed release stays queued and the tick retries
//    it until the PMIC takes it, on the usual five-second period.
static void case_release_retries(void) {
	axpcharge_test_reset();
	CHECK(axpcharge_set_charging(false), "release_retries stops first");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "release_retries starts stopped");
	axpcharge_test_fail_write(true);
	CHECK(!axpcharge_charging_release(), "release_retries immediate attempt fails");
	axpcharge_test_advance_ms(6000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x08, "release_retries still stopped after a failed tick");
	axpcharge_test_fail_write(false);
	axpcharge_test_advance_ms(6000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "release_retries tick restores the bit");
	CHECK(!axpcharge_charging_blocked(), "release_retries tick clears blocked");
}

// 6b. A failed release waits the five seconds; only a requested stop earns the
//     one-second period.
static void case_release_waits_5s(void) {
	axpcharge_test_reset();
	CHECK(axpcharge_set_charging(false), "release_waits_5s stops first");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "release_waits_5s starts stopped");
	axpcharge_test_fail_write(true);
	CHECK(!axpcharge_charging_release(), "release_waits_5s immediate attempt fails");
	// Clear the failure before the first tick: with a one-second retry the tick
	// would now succeed and set the bit, so this check can fail if the cadence
	// regresses.
	axpcharge_test_fail_write(false);
	axpcharge_test_advance_ms(2000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x08, "release_waits_5s not retried after two seconds");
	axpcharge_test_advance_ms(4000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "release_waits_5s retried after five seconds");
	CHECK(!axpcharge_charging_blocked(), "release_waits_5s clears blocked");
}

// 6c. A release that succeeds at once re-enables charging and reports it.
static void case_release_enables(void) {
	axpcharge_test_reset();
	CHECK(axpcharge_set_charging(false), "release_enables stops first");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "release_enables starts stopped");
	CHECK(axpcharge_charging_release(), "release_enables returns true");
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "release_enables restores the bit");
	CHECK(!axpcharge_charging_blocked(), "release_enables reports unblocked");
}

// 7. A nearly flat cell with VBUS in is the one case a stop is refused.
static void case_low_cell_refuses(void) {
	axpcharge_test_reset();
	axpcharge_test_set_millivolts(3200);
	axpcharge_test_set_vbus(true);
	CHECK(!axpcharge_set_charging(false), "low_cell_refuses returns false");
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "low_cell_refuses leaves the register");
}

// 8. A model the table does not know is never written: the gate is the model,
//    not the absent MP2731 node.
static void case_unknown_model_no_write(void) {
	axpcharge_test_reset();
	axpcharge_test_set_applies(false);
	CHECK(!axpcharge_set_charging(false), "unknown_model_no_write returns false");
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "unknown_model_no_write leaves the register");
	CHECK(!axpcharge_charging_control_supported(), "unknown_model_no_write gate is false");
}

// 9. The driver can set the bit again after a gauge reset; the tick bounds it.
static void case_tick_reclears_driver_write(void) {
	axpcharge_test_reset();
	CHECK(axpcharge_set_charging(false), "tick_reclears_driver_write stops first");
	CHECK(axpcharge_test_reg(REG18) == 0x08, "tick_reclears_driver_write starts stopped");
	axpcharge_test_set_reg(REG18, 0x0e);
	axpcharge_test_advance_ms(2000);
	axpcharge_tick();
	CHECK(axpcharge_test_reg(REG18) == 0x0c, "tick_reclears_driver_write clears bit 1 again");
}

// 10. A register that cannot be read is not written blind.
static void case_read_failure_reverts(void) {
	axpcharge_test_reset();
	axpcharge_test_fail_read(true);
	CHECK(!axpcharge_set_charging(false), "read_failure_reverts returns false");
	CHECK(axpcharge_test_reg(REG18) == 0x0a, "read_failure_reverts leaves the register");
	axpcharge_test_fail_read(false);
}

int main(void) {
	case_supported_r1();
	case_stop_clears_only_bit1();
	case_resume_sets_bit1();
	case_refused_off_reverts();
	case_refused_on_reverts();
	case_release_retries();
	case_release_waits_5s();
	case_release_enables();
	case_low_cell_refuses();
	case_unknown_model_no_write();
	case_tick_reclears_driver_write();
	case_read_failure_reverts();
	printf("all axpcharge cases passed\n");
	return 0;
}
