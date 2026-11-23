#ifndef AXPCHARGE_H
#define AXPCHARGE_H

#include <stdbool.h>
#include <stdint.h>

// Charging on a player whose AXP2101 PMIC is also its charger: the R1, which
// has no MP2731. The percentage limit in power.c needs the MP2731's node, so on
// this player there are two switches instead, both written to the PMIC over
// I2C (bus 0, address 0x34):
//
//   * the charge limit: target voltage 4.20 V instead of the stock 4.40 V
//     (REG64h). The charge tapers off and ends on its own at the lower
//     ceiling, around 80%; the charger itself is never switched off.
//   * the current limit: charge current at most 500 mA (REG62h). Never raises
//     a current that is already lower.
//
// Switching either off writes back what was there before. Both are asserted
// again every few seconds from the power tick.

// True on a player charged by the PMIC alone: by the model table when
// system-info.json names one, otherwise by the MP2731's node being absent.
// Decides which controls the Power page shows.
bool axpcharge_applies(void);

// Applies both switches at once. Does nothing where axpcharge_applies() is
// false.
void axpcharge_set(bool limit_voltage, bool limit_current);

// Re-asserts the switches, at most every few seconds. Called from the power
// tick.
void axpcharge_tick(void);

// True while the voltage cap is on and the PMIC reports the charge as done:
// the cable is in and nothing more goes into the battery. As of the last tick.
bool axpcharge_holding(void);

// One register of the PMIC, read or written over the same bus. Both players
// have the AXP2101, whichever of them charges. False when it does not answer.
// On the host it reads and writes the simulator's register map.
bool axpcharge_reg_read(uint8_t reg, uint8_t *value);
bool axpcharge_reg_write(uint8_t reg, uint8_t value);

// ---------------------------------------------------------------------------
// Cell charging: REG18h[1], Cell Battery charge enable
// ---------------------------------------------------------------------------
//
// The USB DAC page stops cell charging while the player is a DAC: on the R1
// the same cable both feeds the system and the cell, and the host does not
// have the current for both. This is a destructive write, so it has a gate of
// its own rather than sharing axpcharge_applies()'s guess.

// True only where the model table says the PMIC is the charger (the R1). The
// host build answers true and simulates the write, so the simulator's toggle
// keeps working. Never a guess from an absent MP2731 node.
bool axpcharge_charging_control_supported(void);

// Stops (or allows) charging the cell while leaving VBUS feeding the system.
// A user request: on failure the requested state is put back to the last
// confirmed one and nothing is retried, so the UI cannot show one state while
// the next tick produces another. Returns true when the PMIC read back the
// requested state.
bool axpcharge_set_charging(bool on);

// True while the PMIC has confirmed that cell charging is stopped.
bool axpcharge_charging_blocked(void);

// Re-enables charging. Called at startup, at DAC exit, at poweroff, and from
// the crash handler. Unlike a user request this one is retried by the tick
// until the PMIC confirms it. Returns the result of the immediate attempt.
bool axpcharge_charging_release(void);

// The crash handler's variant: never blocks and never logs. False when the
// lock is held or the write fails, in which case the driver's own shutdown
// write and the next boot's release are the backstops.
bool axpcharge_charging_release_quiet(void);

#endif /* AXPCHARGE_H */
