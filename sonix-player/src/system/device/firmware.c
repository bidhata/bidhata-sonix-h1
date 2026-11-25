#include "firmware.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/device/clock.h"
#include "src/system/device/power.h"
#include "src/system/device/sysinfo.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/remote/dlna.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/tidalcache.h"

#include "src/gui/shell/gui.h"

// Under this level, with no cable in, the update is not started: recovery
// rewrites the kernel and the rootfs, and a cell that runs out half way leaves
// neither.
#define UPDATE_MIN_BATTERY_PERCENT 30

// The card is scanned once and every name compared case-insensitively,
// rather than guessing at spellings: FAT is case-insensitive but exFAT and
// ext4 are not, and a file written by Windows can land as UPDATE.UPT while
// the stock player only ever looks for update.upt. One readdir covers them
// all -- and there is no cheaper way to be right about it.
static bool find_in_dir(const char *root, const char *stem, char *out, size_t out_size) {
	char wanted[80];
	snprintf(wanted, sizeof(wanted), "%s.upt", stem);

	DIR *dir = opendir(root);
	if (!dir) {
		return false;
	}

	bool found = false;
	struct dirent *de;
	while (!found && (de = readdir(dir)) != NULL) {
		if (strcasecmp(de->d_name, wanted) != 0) {
			continue;
		}
		snprintf(out, out_size, "%s/%s", root, de->d_name);

		struct stat st;
		if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
			found = true;
		}
	}

	closedir(dir);
	if (!found) {
		out[0] = '\0';
	}
	return found;
}

const char *firmware_update_stem(void) {
	// ONLY the name of THIS model -- deliberately NOT the stock binary's
	// "update.upt" fallback.
	//
	// The recovery kernel does not check what it is given: a .upt built for
	// another HiBy model, dropped on the card under the generic name, would be
	// written to the flash exactly the same way, and the device would not come
	// back. "update.upt" is a name anyone might use for anything; "r3proii.upt"
	// is a claim about which player the file is for.
	//
	// An unrecognised player therefore gets no name at all: a fallback would
	// hand an R1 the R3 Pro II's image. The config key is the way out for
	// anyone who knows what they are doing.
	const char *name = config_get("firmware", "name", "");
	if (name && *name) {
		return name;
	}
	const sysinfo_model_t *model = sysinfo_model();
	if (!model) {
		fprintf(stderr,
				"firmware: device-name in system-info.json names no player this build knows"
				" ('%s'); no update file will be looked for\n",
				sysinfo_device_name());
		return NULL;
	}
	return model->update_stem;
}

bool firmware_update_file_find(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return false;
	}
	out[0] = '\0';

	const char *root = storage_sd_root();
	if (!root || !*root) {
		return false;
	}

	const char *name = firmware_update_stem();
	if (!name) {
		return false;
	}
	return find_in_dir(root, name, out, out_size);
}

// ---------------------------------------------------------------------------
// Which player an image is for
// ---------------------------------------------------------------------------
//
// A .upt is an ISO9660 image, and the one member that names the hardware is
// /OTA_V0/OTA_COMPATIBILITY.IN -- "OTA_COMP.IN;1" once the name is cut to 8.3,
// which is how both the stock images and the packer's own write it. It holds
// one line, "compatibility=hiby_r1_x1600_auto_rf_auto" on the R1.
//
// It is read by walking the directory rather than by searching the image for
// the string. The payload is a squashfs compressed with LZO, which leaves
// stretches of its input as literal bytes, so a string can appear anywhere in
// it by accident -- and both players' rootfs hold the other one's name,
// sonix_player's own model table among them. A search would be right or wrong
// depending on how one build happened to compress, in both directions: letting
// a renamed image through, or refusing a release the player should install.
// The installed firmware judges the next one, so a wrong refusal would shut
// the card and the network paths together.

#define ISO_SECTOR 2048
#define ISO_PVD_SECTOR 16
#define ISO_DIR_RECORD_FLAG 0x02 // the record describes a directory
#define ISO_MAX_DIR_BYTES (256 * 1024)
#define ISO_COMPAT_MAX 512

typedef enum {
	IMAGE_ID_READ,	// the image was read and `out` holds what it declares ("" for none)
	IMAGE_ID_UNREADABLE, // not an ISO9660 image, or damaged: never treated as "declares none"
} image_id_t;

static uint32_t le32(const unsigned char *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const unsigned char *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

// One directory record's name, without the ";1" version ISO9660 appends to
// files, compared without regard to case. The length byte comes out of the
// image, so it is checked against the record it sits in: a damaged or hostile
// image must not be able to read past the sector.
static bool record_name_is(const unsigned char *rec, const char *want) {
	size_t len = rec[32];
	if (33 + len > rec[0]) {
		return false;
	}
	const char *name = (const char *)rec + 33;
	const char *semi = memchr(name, ';', len);
	if (semi) {
		len = (size_t)(semi - name);
	}
	return len == strlen(want) && strncasecmp(name, want, len) == 0;
}

// Walks one directory's extent looking for `want`. Fills the extent and length
// of what it finds. Records never straddle a sector: a zero length byte means
// the rest of the sector is padding.
static bool dir_find(FILE *f, uint32_t extent, uint32_t bytes, const char *want, bool want_dir, uint32_t *out_extent,
					 uint32_t *out_len) {
	if (bytes > ISO_MAX_DIR_BYTES) {
		bytes = ISO_MAX_DIR_BYTES;
	}
	unsigned char sector[ISO_SECTOR];
	for (uint32_t off = 0; off < bytes; off += ISO_SECTOR) {
		if (fseek(f, (long)extent * ISO_SECTOR + (long)off, SEEK_SET) != 0) {
			return false;
		}
		if (fread(sector, 1, sizeof(sector), f) != sizeof(sector)) {
			return false;
		}
		size_t i = 0;
		while (i < sizeof(sector)) {
			unsigned char len = sector[i];
			if (len == 0) {
				break; // padding to the end of the sector
			}
			if (len < 34 || i + len > sizeof(sector)) {
				return false; // a record that does not fit is a damaged image
			}
			const unsigned char *rec = sector + i;
			bool is_dir = (rec[25] & ISO_DIR_RECORD_FLAG) != 0;
			if (is_dir == want_dir && rec[32] > 0 && record_name_is(rec, want)) {
				*out_extent = le32(rec + 2);
				*out_len = le32(rec + 10);
				return true;
			}
			i += len;
		}
	}
	return false;
}

// What the image says it is for: the value after "compatibility=" in that one
// member, or "" when the member is not there, which is itself an answer.
static image_id_t image_declared_id(const char *path, char *out, size_t out_size) {
	out[0] = '\0';

	FILE *f = fopen(path, "rb");
	if (!f) {
		return IMAGE_ID_UNREADABLE;
	}

	image_id_t result = IMAGE_ID_UNREADABLE;
	unsigned char pvd[ISO_SECTOR];
	if (fseek(f, ISO_PVD_SECTOR * ISO_SECTOR, SEEK_SET) == 0 && fread(pvd, 1, sizeof(pvd), f) == sizeof(pvd) &&
		pvd[0] == 1 && memcmp(pvd + 1, "CD001", 5) == 0 && le16(pvd + 128) == ISO_SECTOR) {
		// The root's own record sits inside the volume descriptor.
		uint32_t root_extent = le32(pvd + 156 + 2);
		uint32_t root_len = le32(pvd + 156 + 10);
		uint32_t dir_extent = 0, dir_len = 0;
		if (!dir_find(f, root_extent, root_len, "OTA_V0", true, &dir_extent, &dir_len)) {
			// No OTA_V0 at all: not one of these images.
			fclose(f);
			return IMAGE_ID_UNREADABLE;
		}

		uint32_t file_extent = 0, file_len = 0;
		if (!dir_find(f, dir_extent, dir_len, "OTA_COMP.IN", false, &file_extent, &file_len)) {
			fclose(f);
			return IMAGE_ID_READ; // a sound image that names no hardware
		}

		if (file_len > ISO_COMPAT_MAX) {
			file_len = ISO_COMPAT_MAX;
		}
		char buf[ISO_COMPAT_MAX + 1];
		if (fseek(f, (long)file_extent * ISO_SECTOR, SEEK_SET) == 0 && fread(buf, 1, file_len, f) == file_len) {
			buf[file_len] = '\0';
			const char *key = strstr(buf, "compatibility=");
			if (key) {
				key += strlen("compatibility=");
				size_t n = 0;
				while (key[n] && key[n] != '\n' && key[n] != '\r' && n + 1 < out_size) {
					n++;
				}
				memcpy(out, key, n);
				out[n] = '\0';
			}
			result = IMAGE_ID_READ; // the member was read, empty value included
		}
	}

	fclose(f);
	return result;
}

ota_result_t firmware_update_model_check(const char *path) {
	if (!path || !*path) {
		return OTA_MODEL_MISMATCH;
	}

	const char *stem = firmware_update_stem();
	if (!stem || !*stem) {
		return OTA_NO_MODEL;
	}

	// The name first, which is free. Recovery flashes whatever it is handed,
	// so the name alone settles nothing -- but a file named for the other
	// player need not be opened.
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	char wanted[80];
	snprintf(wanted, sizeof(wanted), "%s.upt", stem);
	if (strcasecmp(base, wanted) != 0) {
		return OTA_MODEL_MISMATCH;
	}

	const sysinfo_model_t *model = sysinfo_model();
	if (!model) {
		return OTA_NO_MODEL;
	}

	char declared[64];
	if (image_declared_id(path, declared, sizeof(declared)) != IMAGE_ID_READ) {
		printf("firmware: %s is not an update image this player can read\n", base);
		return OTA_MODEL_MISMATCH;
	}

	const char *expected = model->compat_id ? model->compat_id : "";
	if (strcasecmp(declared, expected) != 0) {
		printf("firmware: image declares '%s', this player wants '%s'\n", declared, expected);
		return OTA_MODEL_MISMATCH;
	}
	printf("firmware: image is for this player ('%s')\n", declared[0] ? declared : "no compatibility id, as expected");
	return OTA_OK;
}

bool firmware_file_sha256(const char *path, char out[SHA256_HEX_LEN]) {
	if (!path || !out) {
		return false;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}

	sha256_ctx sha;
	sha256_init(&sha);
	unsigned char buf[64 * 1024];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		sha256_update(&sha, buf, n);
	}
	bool ok = !ferror(f);
	fclose(f);

	if (ok) {
		sha256_final_hex(&sha, out);
	}
	return ok;
}

ota_result_t firmware_update_prepare(const char *path) {
	ota_result_t r = firmware_update_model_check(path);
	if (r != OTA_OK) {
		return r;
	}

	char hex[SHA256_HEX_LEN];
	if (firmware_file_sha256(path, hex)) {
		printf("firmware: update image sha256 %s\n", hex);
	} else {
		printf("firmware: cannot hash %s\n", path);
	}
	return OTA_OK;
}

bool firmware_update_start(void) {
#ifdef HOST_BUILD
	printf("firmware: (host build) would arm recovery and reboot\n");
	return true;
#else
	// A level read now, not the status bar's last one. No reading at all is
	// not a refusal: a gauge that does not answer must not lock updates out.
	sync_battery_from_sysfs();
	const char *level = read_battery_percent();
	if (level && level[0] >= '0' && level[0] <= '9' && atoi(level) < UPDATE_MIN_BATTERY_PERCENT &&
		!usb_vbus_present()) {
		printf("firmware: battery at %s%% and no cable in; the update is not started\n", level);
		gui_notify_popup("system_battery_warning");
		return false;
	}

	printf("firmware: arming recovery\n");

	// The screen fades out the way the power key puts it out, first: what
	// follows holds this thread for seconds, and a frozen page reads as a
	// device that has hung. A dark one reads as a device that is restarting.
	power_screen_off();

	// Playback down first: the card is about to be handed to the recovery
	// kernel, and the decoder still has it open.
	audio_stop();

	// The streaming caches (tracks and covers) survive no power cycle, the
	// update's included.
	qobuzcache_clear_on_exit();
	tidalcache_clear_on_exit();
	podcastcache_clear_on_exit();
	dlna_clear_on_exit();

	// The clock goes into the RTC on the way out, like every other exit path
	// -- recovery reboots the device a second time when it is done, and the
	// time would otherwise be whatever the RTC last knew.
	clock_shutdown();

	// Recovery will leave the card marked as not unmounted; the next start is
	// told not to take that for a fault. Before the card is let go, since the
	// note is decided by how the card was found when this run mounted it.
	storage_note_update_handover();

	// The recovery kernel reads the update off the card: it finds it clean.
	storage_release_for_shutdown();
	sync();
	sleep(1);

	// Two commands: erase the first block of /dev/mtd5, write "ota:kernel2"
	// into it. That is the whole of the stock firmware's bootmode.sh, and it
	// is what U-Boot reads to decide which kernel to start.
	int rc = system("/usr/bin/bootmode.sh Recovery");
	if (rc != 0) {
		printf("firmware: bootmode.sh Recovery failed (rc=%d)\n", rc);
	}

	sync();
	sleep(1);

	rc = system("reboot");
	(void)rc;

	// Spins here rather than returning, as the stock binary does: with recovery
	// already armed there is nothing sensible left to do, and going back to the
	// settings page would leave the device one power-cycle away from an update
	// the user is no longer expecting.
	sleep(10);
	reboot(RB_AUTOBOOT);
	for (;;) {
		sleep(1);
	}
#endif
}
