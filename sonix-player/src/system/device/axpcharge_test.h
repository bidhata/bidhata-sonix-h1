#ifndef AXPCHARGE_TEST_H
#define AXPCHARGE_TEST_H

#include <stdbool.h>
#include <stdint.h>

// Test-only hooks into axpcharge.c, compiled in with AXPCHARGE_TEST beside
// HOST_BUILD. They reach the simulated PMIC register map and the charge state
// machine the player runs on the device; no product build defines AXPCHARGE_TEST.
//
// The tests are in tools/test_axpcharge.c, run by `make test-axpcharge`.

// Back to the state of a player just booted: the stock registers, no
// failures, the model gate open, 3.9 V on a connected cable, and every timer
// and confirmed state cleared.
void axpcharge_test_reset(void);

// Moves the re-assert timers `ms` milliseconds into the past, so the next
// axpcharge_tick() sees them due. It clears nothing: a pending release
// survives, unlike in axpcharge_test_reset().
void axpcharge_test_advance_ms(unsigned ms);

// One byte of the simulated PMIC register map, behind the read and write
// hooks axpcharge.c uses under HOST_BUILD.
void axpcharge_test_set_reg(uint8_t reg, uint8_t value);
uint8_t axpcharge_test_reg(uint8_t reg);

// Makes every simulated I2C write, or read, fail until cleared.
void axpcharge_test_fail_write(bool fail);
void axpcharge_test_fail_read(bool fail);

// The answer axpcharge_charging_control_supported() gives on the host.
void axpcharge_test_set_applies(bool applies);

// What the stubbed gauge and cable detect report. Both are implemented in
// tools/test_axpcharge.c, beside the stubs whose state they set; the other
// hooks below live in axpcharge.c.
void axpcharge_test_set_millivolts(int mv);
void axpcharge_test_set_vbus(bool present);

#endif /* AXPCHARGE_TEST_H */
