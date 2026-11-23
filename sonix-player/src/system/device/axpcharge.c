#include "axpcharge.h"

#include "src/system/core/config.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef AXPCHARGE_TEST
#include "axpcharge_test.h"
#endif

#define MP2731_NODE "/sys/class/power_supply/mp2731-charger"

#define AXP_BUS "/dev/i2c-0"
#define AXP_ADDR 0x34

// REG01h[2:0], charging status: 4 is "charge done".
#define AXP_REG_STATUS 0x01
#define AXP_STATUS_DONE 4

// REG18h[1], Cell Battery charge enable. Clearing it stops the cell charging
// while VBUS still feeds the system; that is what DAC mode with charging
// disabled needs.
#define AXP_REG_MODULE 0x18
#define AXP_MODULE_CHARGE 0x02u

// REG62h[4:0], constant charge current: N*25 mA up to 8 (200 mA), then
// 200 + (N-8)*100 mA. 11 is 500 mA.
#define AXP_REG_CURRENT 0x62
#define AXP_CURRENT_MASK 0x1Fu
#define AXP_CURRENT_CAP 11u

// REG64h[2:0], charge target voltage: 1 4.00 V, 2 4.10 V, 3 4.20 V, 4 4.35 V,
// 5 4.40 V. The R1's axp2101.sh loads the driver with
// charge_voltage_limit=4400, which the driver writes here at every boot.
#define AXP_REG_VOLTAGE 0x64
#define AXP_VOLTAGE_MASK 0x07u
#define AXP_VOLTAGE_CAP 3u
#define AXP_VOLTAGE_STOCK 5u

// How often the tick re-asserts the switches, and retries after a failure.
#define REASSERT_MS 5000

// How often the tick re-asserts a stopped charger: the stock driver can set
// the bit again after a gauge reset, and one second bounds that pulse.
#define CHARGE_REASSERT_MS 1000

// Below this cell voltage a stop with VBUS present is refused: DAC mode shares
// the cable with the host, and the system may need to draw from a nearly flat
// cell.
#define CHARGE_MIN_MV 3400

// The charge current found before the cap first lowered it, kept in the
// config: after a restart with the cap on, the register holds the cap.
#define CURRENT_KEY "axp_charge_current"

// All I2C and state changes take this, the charge write and the cap writes
// included, so they cannot interleave. The crash handler only tries it.
static pthread_mutex_t axp_lock = PTHREAD_MUTEX_INITIALIZER;

static bool configured;
static bool want_voltage, want_current;
static bool holding;
static long long next_ms;

// The answer from the first call: the model table and the MP2731 node do not
// change while the player runs. -1 until asked.
static int applies_answer = -1;

// What the PMIC held at first contact, so the log says where the switches
// started from. A file static rather than a function static so that
// axpcharge_test_reset() can clear it.
static bool logged_contact;

// The voltage to go back to: the register as first found, unless it already
// held the cap, left there by an earlier run.
static uint8_t voltage_stock = AXP_VOLTAGE_STOCK;
static bool voltage_seen;

// The charge bit. charge_known is set by a user request that succeeded and by
// every release; the tick re-asserts the bit only then, so opening the Power
// page is not required for the DAC toggle to work.
static bool charge_known;
static bool charge_wanted_off;		 // the state the tick keeps enforcing
static bool charge_saved_bit = true; // REG18h[1] before the first stop; always 1 (see below)
static bool charge_blocked;			 // last state the PMIC confirmed
static long long charge_next_ms;

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool axpcharge_applies(void) {
	if (applies_answer < 0) {
		const sysinfo_model_t *model = sysinfo_model();
		applies_answer = model ? model->pmic_charger : access(MP2731_NODE, F_OK) != 0;
	}
	return applies_answer != 0;
}

#ifdef HOST_BUILD

// The simulator has no PMIC: a register map stands in for it, with the stock
// values the R1 boots with, so the switches and the charge bit behave as they
// do on the device. AXPCHARGE_TEST reaches it through axpcharge_test.h.
static uint8_t host_regs[256] = {
	[AXP_REG_MODULE] = 0x0a,
	[AXP_REG_CURRENT] = 0x0c,
	[AXP_REG_VOLTAGE] = 0x05,
};
static bool host_fail_read;
static bool host_fail_write;
static bool host_applies = true;

static bool axp_read(uint8_t reg, uint8_t *value) {
	if (host_fail_read) {
		return false;
	}
	*value = host_regs[reg];
	return true;
}

static bool axp_write(uint8_t reg, uint8_t value) {
	if (host_fail_write) {
		return false;
	}
	host_regs[reg] = value;
	return true;
}

#else

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>

// One SMBus byte transfer. I2C_SLAVE_FORCE because the PMIC's own driver holds
// the address.
static bool xfer(uint8_t reg, uint8_t *value, bool write) {
	int fd = open(AXP_BUS, O_RDWR);
	if (fd < 0) {
		return false;
	}
	bool ok = false;
	if (ioctl(fd, I2C_SLAVE_FORCE, AXP_ADDR) >= 0) {
		union i2c_smbus_data data;
		data.byte = write ? *value : 0;
		struct i2c_smbus_ioctl_data args = {
			.read_write = write ? I2C_SMBUS_WRITE : I2C_SMBUS_READ,
			.command = reg,
			.size = I2C_SMBUS_BYTE_DATA,
			.data = &data,
		};
		if (ioctl(fd, I2C_SMBUS, &args) >= 0) {
			if (!write) {
				*value = data.byte;
			}
			ok = true;
		}
	}
	close(fd);
	return ok;
}

static bool axp_read(uint8_t reg, uint8_t *value) { return xfer(reg, value, false); }
static bool axp_write(uint8_t reg, uint8_t value) { return xfer(reg, &value, true); }

#endif /* HOST_BUILD */

// Writes `target` into the bits of `mask`, keeping the others, and reads it
// back.
static bool set_field(uint8_t reg, uint8_t mask, uint8_t target) {
	uint8_t v;
	if (!axp_read(reg, &v)) {
		return false;
	}
	if ((v & mask) == target) {
		return true;
	}
	if (!axp_write(reg, (uint8_t)((v & ~mask) | target)) || !axp_read(reg, &v)) {
		return false;
	}
	return (v & mask) == target;
}

static int current_ma(unsigned n) { return n <= 8 ? (int)n * 25 : 200 + ((int)n - 8) * 100; }

static bool apply_voltage(void) {
	uint8_t v;
	if (!axp_read(AXP_REG_VOLTAGE, &v)) {
		return false;
	}
	if (!voltage_seen) {
		voltage_seen = true;
		if ((v & AXP_VOLTAGE_MASK) != AXP_VOLTAGE_CAP) {
			voltage_stock = v & AXP_VOLTAGE_MASK;
		}
	}
	return set_field(AXP_REG_VOLTAGE, AXP_VOLTAGE_MASK, want_voltage ? AXP_VOLTAGE_CAP : voltage_stock);
}

// On: a current above the cap is saved, then lowered; one already at or below
// it is left. Off: only a register holding the cap is touched, and only put
// back to a saved value, so a player that never used the switch keeps its own.
static bool apply_current(void) {
	uint8_t v;
	if (!axp_read(AXP_REG_CURRENT, &v)) {
		return false;
	}
	unsigned field = v & AXP_CURRENT_MASK;
	long saved = config_get_int("power", CURRENT_KEY, -1);

	if (want_current) {
		if (field <= AXP_CURRENT_CAP) {
			return true;
		}
		if (saved != (long)field) {
			config_set_int("power", CURRENT_KEY, (long)field);
			config_save();
		}
		return set_field(AXP_REG_CURRENT, AXP_CURRENT_MASK, AXP_CURRENT_CAP);
	}

	if (field != AXP_CURRENT_CAP || saved <= (long)AXP_CURRENT_CAP || saved > (long)AXP_CURRENT_MASK) {
		return true;
	}
	return set_field(AXP_REG_CURRENT, AXP_CURRENT_MASK, (uint8_t)saved);
}

static bool apply(void) {
	bool ok = apply_voltage();
	ok = apply_current() && ok;

	uint8_t status;
	holding = want_voltage && axp_read(AXP_REG_STATUS, &status) && (status & 7u) == AXP_STATUS_DONE;
	return ok;
}

// What the PMIC holds, once, so the log says what the switches started from.
static void log_first_contact(void) {
	if (logged_contact) {
		return;
	}
	logged_contact = true;
	uint8_t volt, cur;
	if (!axp_read(AXP_REG_VOLTAGE, &volt) || !axp_read(AXP_REG_CURRENT, &cur)) {
		printf("axpcharge: the PMIC does not answer on %s at 0x%02x\n", AXP_BUS, AXP_ADDR);
		return;
	}
	printf("axpcharge: found REG64h=0x%02x, REG62h=0x%02x (%d mA)\n", volt, cur,
		   current_ma(cur & AXP_CURRENT_MASK));
}

#ifdef HOST_BUILD

// The host build answers true and simulates the write, so the simulator's
// toggle keeps working; AXPCHARGE_TEST decides which answer the tests see.
bool axpcharge_charging_control_supported(void) { return host_applies; }

#else

// True only where the model table says the PMIC is the charger (the R1). The
// host build answers true and simulates the write, so the simulator's toggle
// keeps working. Never a guess from an absent MP2731 node.
bool axpcharge_charging_control_supported(void) {
	const sysinfo_model_t *model = sysinfo_model();
	return model && model->pmic_charger;
}

#endif

// The one bit the charge code writes, as the state it wants: 0 stops the cell,
// the saved bit allows it again.
static uint8_t charge_bit_value(void) {
	return charge_wanted_off || !charge_saved_bit ? 0 : AXP_MODULE_CHARGE;
}

// Writes the wanted state and updates the confirmed one. `reassert` is the
// tick's periodic pass: only there does a set bit mean the driver put it back
// behind us, and only there is that worth a line. `quiet` is the crash
// handler's: it must not print, and a lock it could not take never gets here.
static bool charge_apply_locked(bool reassert, bool quiet) {
	uint8_t v;
	if (!axp_read(AXP_REG_MODULE, &v)) {
		return false;
	}
	bool reenabled = reassert && charge_wanted_off && (v & AXP_MODULE_CHARGE) != 0;
	if (!set_field(AXP_REG_MODULE, AXP_MODULE_CHARGE, charge_bit_value())) {
		return false;
	}
	if (reenabled) {
		printf("axpcharge: driver re-enabled cell charging; stopped again\n");
	}
	bool was_blocked = charge_blocked;
	charge_blocked = charge_wanted_off;
	if (!quiet && charge_blocked != was_blocked) {
		if (axp_read(AXP_REG_MODULE, &v)) {
			printf("axpcharge: cell charging %s (REG18h=0x%02x)\n", charge_blocked ? "stopped" : "allowed", v);
		} else {
			printf("axpcharge: cell charging %s\n", charge_blocked ? "stopped" : "allowed");
		}
	}
	return true;
}

// The release both variants share, with the lock already held. A failed write
// stays queued: the deadline is the usual re-assert period away, and the tick
// keeps trying until the PMIC confirms it.
static bool charge_release_locked(bool quiet) {
	charge_wanted_off = false;
	charge_known = true;
	bool ok = charge_apply_locked(false, quiet);
	charge_next_ms = now_ms() + REASSERT_MS;
	return ok;
}

bool axpcharge_set_charging(bool on) {
	if (!axpcharge_charging_control_supported()) {
		return false;
	}

	pthread_mutex_lock(&axp_lock);

	// A nearly flat cell with VBUS present is the one case where a stop is
	// refused; an unreadable gauge is not, or the toggle would fail on a
	// healthy player.
	if (!on && usb_vbus_present()) {
		int mv = read_battery_millivolts();
		if (mv > 0 && mv < CHARGE_MIN_MV) {
			printf("axpcharge: cell at %d mV, charging stays on\n", mv);
			pthread_mutex_unlock(&axp_lock);
			return false;
		}
		if (mv <= 0) {
			printf("axpcharge: cell voltage unknown, stopping the charger anyway\n");
		}
	}

	// First stop: remember the bit, so allowing charging again can put back
	// what was there. A clear bit found before the first stop is stale, not
	// healthy (startup releases charging before any DAC mode can exist), so it
	// still counts as set: charge_saved_bit therefore always ends up 1.
	if (!on && !charge_known) {
		uint8_t v;
		if (!axp_read(AXP_REG_MODULE, &v)) {
			pthread_mutex_unlock(&axp_lock);
			return false;
		}
		charge_saved_bit = (v & AXP_MODULE_CHARGE) != 0;
		if (!charge_saved_bit) {
			charge_saved_bit = true;
		}
	}

	bool confirmed_off = charge_blocked;
	charge_wanted_off = !on;
	bool ok = charge_apply_locked(false, false);
	if (ok) {
		charge_known = true;
		charge_next_ms = now_ms() + (charge_wanted_off ? CHARGE_REASSERT_MS : REASSERT_MS);
	} else {
		// Nothing is retried behind the UI: put the wanted state back to what
		// the PMIC last confirmed, which is what the icon shows.
		charge_wanted_off = confirmed_off;
	}

	pthread_mutex_unlock(&axp_lock);
	return ok;
}

bool axpcharge_charging_blocked(void) {
	pthread_mutex_lock(&axp_lock);
	bool blocked = charge_blocked;
	pthread_mutex_unlock(&axp_lock);
	return blocked;
}

bool axpcharge_charging_release(void) {
	if (!axpcharge_charging_control_supported()) {
		return false;
	}
	pthread_mutex_lock(&axp_lock);
	bool ok = charge_release_locked(false);
	pthread_mutex_unlock(&axp_lock);
	return ok;
}

bool axpcharge_charging_release_quiet(void) {
	if (!axpcharge_charging_control_supported()) {
		return false;
	}
	if (pthread_mutex_trylock(&axp_lock) != 0) {
		return false;
	}
	bool ok = charge_release_locked(true);
	pthread_mutex_unlock(&axp_lock);
	return ok;
}

void axpcharge_set(bool limit_voltage, bool limit_current) {
	if (!axpcharge_applies()) {
		return;
	}
	pthread_mutex_lock(&axp_lock);
	log_first_contact();
	configured = true;
	want_voltage = limit_voltage;
	want_current = limit_current;
	bool ok = apply();
	next_ms = now_ms() + REASSERT_MS;
	printf("axpcharge: charge limit %s, 500 mA limit %s%s\n", want_voltage ? "on" : "off",
		   want_current ? "on" : "off", ok ? "" : " (the PMIC did not take it, retrying)");
	pthread_mutex_unlock(&axp_lock);
}

void axpcharge_tick(void) {
	pthread_mutex_lock(&axp_lock);
	long long now = now_ms();

	if (configured && now >= next_ms) {
		next_ms = now + REASSERT_MS;
		apply();
	}

	// Its own pass: the caps keep their five seconds, but a stop is re-asserted
	// every second, because the driver can set the bit again after a gauge
	// reset. Everything else, a failed release included, waits the usual five:
	// only a requested stop earns the fast period.
	if (charge_known && now >= charge_next_ms) {
		charge_apply_locked(true, false);
		charge_next_ms = now_ms() + (charge_wanted_off ? CHARGE_REASSERT_MS : REASSERT_MS);
	}

	pthread_mutex_unlock(&axp_lock);
}

bool axpcharge_holding(void) { return holding; }

bool axpcharge_reg_read(uint8_t reg, uint8_t *value) {
	pthread_mutex_lock(&axp_lock);
	bool ok = axp_read(reg, value);
	pthread_mutex_unlock(&axp_lock);
	return ok;
}

bool axpcharge_reg_write(uint8_t reg, uint8_t value) {
	pthread_mutex_lock(&axp_lock);
	bool ok = axp_write(reg, value);
	pthread_mutex_unlock(&axp_lock);
	return ok;
}

#ifdef AXPCHARGE_TEST

// The test harness compiles this file with HOST_BUILD as well; these hooks
// reach the simulated map and the state machine above. They are not locked:
// the tests are single-threaded and reset everything between cases.

void axpcharge_test_reset(void) {
	memset(host_regs, 0, sizeof host_regs);
	host_regs[AXP_REG_MODULE] = 0x0a;
	host_regs[AXP_REG_CURRENT] = 0x0c;
	host_regs[AXP_REG_VOLTAGE] = 0x05;
	host_fail_read = false;
	host_fail_write = false;
	host_applies = true;

	configured = false;
	want_voltage = false;
	want_current = false;
	holding = false;
	next_ms = 0;
	logged_contact = false;
	applies_answer = -1;

	voltage_stock = AXP_VOLTAGE_STOCK;
	voltage_seen = false;

	charge_known = false;
	charge_wanted_off = false;
	charge_saved_bit = true;
	charge_blocked = false;
	charge_next_ms = 0;

	axpcharge_test_set_millivolts(3900);
	axpcharge_test_set_vbus(true);
}

void axpcharge_test_advance_ms(unsigned ms) {
	next_ms -= (long long)ms;
	charge_next_ms -= (long long)ms;
}

void axpcharge_test_set_reg(uint8_t reg, uint8_t value) { host_regs[reg] = value; }

uint8_t axpcharge_test_reg(uint8_t reg) { return host_regs[reg]; }

void axpcharge_test_fail_write(bool fail) { host_fail_write = fail; }

void axpcharge_test_fail_read(bool fail) { host_fail_read = fail; }

void axpcharge_test_set_applies(bool applies) { host_applies = applies; }

#endif /* AXPCHARGE_TEST */
