#ifndef SYSTEM_H
#define SYSTEM_H

#include <stddef.h>
#include <stdbool.h>

typedef struct {
	// Preferred block device, e.g. "/dev/mmcblk0p1". May be NULL: the player
	// then probes the nodes the stock firmware knows about (mmcblk0p1,
	// mmcblk1p1, and the whole-disk nodes for cards without a partition table).
	const char *device;

	// Preferred mount point. May be NULL, in which case the player picks
	// whichever of HiBy OS's three names for the same directory actually exists
	// on this firmware (/mnt/sd_0, /data/mnt/sd_0, /usr/data/mnt/sd_0 -- two of
	// them are symlinks, but which two differs between builds).
	const char *mount_point;

	// Whether the player mounts the card itself.
	//
	// This has to be true on the device. Nothing in HiBy OS's boot sequence
	// mounts the microSD: mdev has no mmcblk rule, and sys_server only mounts
	// when something asks it to over its socket. The thing that asks is the
	// stock hiby_player, which doubles as the hotplug manager -- so replacing
	// it means taking over that job too, which is exactly what this does.
	bool manage_mount;
} storage_config_t;

typedef struct {
	const char *battery_capacity_file;
	// sysfs "status" node: "Charging", "Discharging", "Full", "Not charging".
	// May be NULL, in which case charging always reads as false.
	const char *battery_status_file;
} battery_config_t;

typedef struct {
	storage_config_t *storage_cfg;
	battery_config_t *battery_cfg;
} system_config_t;

typedef void (*system_notification_cb_t)(const char *message, void *user_data);

// Re-reads the battery level and charging state from sysfs into the cache the
// two readers below serve.
void sync_battery_from_sysfs(void);
char *read_battery_percent();

// True while the charger is plugged in (sysfs status "Charging" or "Full").
bool read_battery_charging(void);

// The cell's voltage in millivolts, from the supply the level is read from, or
// -1 when that supply does not say.
int read_battery_millivolts(void);

// Hides the charging indication (battery icon, status LED) while something has
// deliberately turned the charger off -- DAC mode with charging disabled.
void system_suppress_charging(bool suppressed);

void system_start_services(system_config_t *cfg, system_notification_cb_t notification_cb, void *user_data);

// The directory the card ended up mounted on, valid once
// system_start_services() has returned. This is what the file browser should
// read: hardcoding a path guesses wrong on firmwares that arrange the
// /mnt -> /data/mnt -> /usr/data/mnt symlinks differently.
const char *storage_sd_root(void);

// Whether the card can be written to. False for a card mounted read-only -- an
// NTFS one where ntfs-3g is not on the device, or an adapter with its
// write-protect notch closed. Everything that keeps state on the card asks
// this rather than discovering it as a silent failure.
bool storage_sd_writable(void);

// The SD block device the storage layer settled on (e.g. /dev/mmcblk0p1),
// or NULL when no card was found. Used by the USB mass-storage export.
const char *storage_sd_device(void);

// Whether the card is in the slot and everything that lives on it is open.
// False from the moment a removal is seen until a card is mounted again, which
// is what anything asking "is there a card" wants -- the block device name
// outlives the card that was pulled out.
bool storage_card_attached(void);

// Whether the card's root can be written to as the card right now: attached,
// really mounted, and not handed to a computer over USB. storage_sd_root()
// names the mount point even with nothing mounted on it, and a file written
// there then lands on the internal flash.
bool storage_card_usable(void);

// Whether the card was not unmounted cleanly at some time before this mount:
// 1 yes, 0 no, -1 not known (no card, a filesystem that keeps no such flag, or
// a card this player did not mount itself). Decided before each mount, which
// is the only moment the flag on the card says anything. On FAT a mark stays
// until the card is checked on a computer, so "the time before" can be long
// ago.
int storage_card_unclean(void);

// Reads that flag from `device`, which must not be mounted. mount_sd() does it
// by itself; this is for the one other place that mounts the card, the USB
// export handing it back (usb.c).
void storage_probe_unclean(const char *device);

// Called by the firmware update just before it hands the card to the recovery
// system. Recovery reads the image off the card and restarts without
// unmounting it, which leaves the card marked as not unmounted, and on FAT
// that mark never goes away by itself. When the card was clean as this run
// mounted it, its serial number is noted in the configuration, and at the next
// start the mark on that same card is lowered on the card before it is
// mounted. Writes the configuration.
void storage_note_update_handover(void);

// The filesystem the card is mounted as, the kernel's name for it ("vfat",
// "exfat", "fuseblk" for ntfs-3g). False, with an empty string, when nothing
// is mounted on the card's root.
bool storage_card_fstype(char *out, size_t out_size);

// Formatting the card as FAT32, which is the one filesystem the device has a
// tool to make.
typedef enum {
	STORAGE_FORMAT_OK,
	STORAGE_FORMAT_UNSUPPORTED, // no mkdosfs, or a card this player does not mount itself (the simulator)
	STORAGE_FORMAT_NO_CARD,		// no card, or it is with a computer over USB
	STORAGE_FORMAT_BUSY,		// something has a file open on it; nothing was changed
	STORAGE_FORMAT_FAILED,		// mkdosfs failed; the card is mounted again if it still can be
	STORAGE_FORMAT_NOT_MOUNTED, // formatted, but it would not mount afterwards
} storage_format_result_t;

bool storage_format_supported(void);

// Erases the card. Stops playback, closes everything the player keeps on the
// card, unmounts it, formats it, mounts it and opens everything again on the
// empty card. Blocks for as long as that takes (seconds): never on the UI
// thread. Any thread otherwise.
storage_format_result_t storage_format_card(void);

// True while storage_format_card() has the card. The USB watcher does not
// offer the card to a computer meanwhile.
bool storage_format_active(void);

// Reopens whichever of the card's databases -- music, audiobooks, radio,
// podcasts, thumbnails -- was deleted or replaced since it was opened. Called
// after something on the device may have removed files: the file manager, the
// Wi-Fi transfer page. Interface thread; a database that changed is closed and
// opened again, an unchanged one costs a stat().
void storage_card_files_recheck(void);

// Looks at the card and puts it back if it has gone missing: a mount that has
// died under it -- the card was pulled, or a USB export unmounted it and the
// remount did not take -- is cleared, the card is mounted again, and
// everything that lives on it is reopened.
//
// Safe to call at any time and from any thread. It does nothing when the card
// is fine, and nothing while a USB export deliberately has the card away.
//
// A supervisor thread calls this every few seconds, and that is what makes the
// card come back on its own rather than staying dark until a reboot: the paths
// that can lose it are many (hotplug, USB export, ADB rearranging the gadget
// under the export) and hardening each of them one at a time has not worked.
void storage_recheck_card(void);

// Before the machine is powered off or rebooted: playback stops, everything
// kept open on the card is closed, and the card is unmounted, or remounted
// read-only when something still holds it. From then on nothing mounts it
// again. init's own `umount -a -r` runs while this process is alive and finds
// the card busy, which leaves FAT and exFAT with their volume-dirty flag set.
// Call it last, after anything that still writes to the card.
void storage_release_for_shutdown(void);

// "Lock keys while the screen is off": with it on, volume down + power pressed
// with the screen off locks the buttons on the case, and pressed again unlocks
// them. Locked, they are ignored while the screen is off; the power key and the
// headphone remote keep working. Turning the option off drops the lock. Off
// until set.
void input_set_key_lock(bool on);

// How far one press of a volume key moves the volume, in percent: on the case,
// on the headphone cable and from Bluetooth headphones alike. Clamped to
// VOLUME_STEP_MIN..VOLUME_STEP_MAX; VOLUME_STEP_MIN until set.
#define VOLUME_STEP_MIN 1
#define VOLUME_STEP_MAX 5
void input_set_volume_step(int percent);

#ifdef HOST_BUILD
// Feeds one key press or release to the button thread, as if it had come off
// an evdev node. `code` is a KEY_* from linux/input.h.
void input_host_button(int code, bool down);
#endif

#endif /* SYSTEM_H */
