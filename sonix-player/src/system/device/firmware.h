#ifndef FIRMWARE_H
#define FIRMWARE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/core/sha256.h"
#include "src/system/device/ota.h"

// Firmware update from the microSD card -- the same two steps the stock
// player does, and nothing more. An update from the internet (ota.h) puts the
// file on the card first and then takes the same path.
//
// The recovery kernel performs the update itself. All the player has to do is
// (1) confirm there is a .upt file on the card and (2) tell the bootloader to
// come up in recovery next time, then reboot. The recovery image finds the
// file by itself, exactly as it does under the stock firmware.
//
// How the stock binary does it, from the disassembly:
//
//   name = config("firmware_name"); if empty -> config("device")  ("R3PROII")
//   tolower(name); sprintf(path, "a:\\%s.upt", name)
//   look for "a:\update.upt" first, then "a:\<name>.upt"
//   if neither exists   -> error: no system update file found
//   otherwise           -> av_close(); sync(); sleep(1);
//                          /usr/bin/bootmode.sh Recovery; reboot; for(;;)
//
// The "update.upt" half of that search is deliberately dropped: the recovery
// kernel writes whatever it is handed, so a file named after the model is the
// only thing standing between a wrong .upt and a device that does not come
// back.
//
// "a:" is its name for the card's mount point. bootmode.sh Recovery is two
// commands: erase the first block of /dev/mtd5 and write "ota:kernel2" into
// it -- the boot selector U-Boot reads. Nothing else on the flash is touched,
// so a device that is rebooted before the update starts simply comes back up
// in recovery and can be sent home again.

// The name of this model's update file without ".upt": [firmware] name in
// the config when set, otherwise the model table's update_stem ("r3proii",
// "r1"). NULL when system-info.json names no model this build knows.
const char *firmware_update_stem(void);

// Looks for the update file on the card. Returns true and fills `out` with its
// full path when one is there. The name must be the device's own --
// "r3proii.upt" -- in any mixture of upper and lower case; nothing else is
// accepted, "update.upt" included.
bool firmware_update_file_find(char *out, size_t out_size);

// Checks an image before recovery is armed. The file name must be this
// model's, and the image must declare this model: the recovery kernel flashes
// whatever it is handed, so a wrong image renamed to the right file name would
// otherwise brick the player.
//
// OTA_OK means the image passed. OTA_MODEL_MISMATCH names a file for another
// player, OTA_NO_MODEL a build that cannot name its own image.
ota_result_t firmware_update_model_check(const char *path);

// SHA-256 of a file, lower-case hex in `out`. False when the file cannot be
// read. The manual update path uses this to record the digest of the image it
// is about to flash, the same hash the OTA path checks against GitHub.
bool firmware_file_sha256(const char *path, char out[SHA256_HEX_LEN]);

// The one check both update paths run before recovery is armed. It performs
// the model check and logs the image's SHA-256. OTA_OK means recovery may be
// armed; any other result names why it must wait.
ota_result_t firmware_update_prepare(const char *path);

// Arms recovery and reboots into it. Does not return on the device, except
// with false when it refuses: the battery reads under 30% with no cable in, a
// level a flash must not be started on. The refusal shows its own message.
//
// Stops playback and flushes the filesystems first: the card is about to be
// read by the recovery kernel, and an unflushed write on it is how a good
// .upt file turns into a failed update.
//
// A no-op that only logs on the host build -- writing to /dev/mtd5 on the
// developer's own machine is not a mistake worth being able to make.
bool firmware_update_start(void);

#endif /* FIRMWARE_H */
