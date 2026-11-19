#include "firmware.h"

#include <ctype.h>
#include <dirent.h>
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

// The part of the model name that survives every image build: "HiBy R1" and
// "HiBy R3", from system-info.json's "HiBy R1" and "HiBy R3 Pro II". The
// stock R3 image spells it "HiBy R3PROII", so the full name would miss a
// stock image and refuse a legitimate rollback.
static void model_marker(const char *name, char *out, size_t size) {
	const char *first = strchr(name, ' ');
	if (!first) {
		snprintf(out, size, "%s", name);
		return;
	}
	const char *second = strchr(first + 1, ' ');
	size_t len = second ? (size_t)(second - name) : strlen(name);
	if (len >= size) {
		len = size - 1;
	}
	memcpy(out, name, len);
	out[len] = '\0';
}

// Case-insensitive search across chunk boundaries. An image is tens of
// megabytes, so it is read a piece at a time; the last need-1 bytes stay in
// the buffer to catch a name split between two reads.
static bool file_contains(const char *path, const char *needle) {
	size_t need = strlen(needle);
	if (need == 0) {
		return false;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}

	char buf[8192];
	size_t have = 0;
	bool found = false;
	while (!found) {
		size_t n = fread(buf + have, 1, sizeof(buf) - have, f);
		if (n == 0) {
			break;
		}
		have += n;
		for (size_t i = 0; i + need <= have; i++) {
			size_t j = 0;
			while (j < need && tolower((unsigned char)buf[i + j]) == tolower((unsigned char)needle[j])) {
				j++;
			}
			if (j == need) {
				found = true;
				break;
			}
		}
		if (found || have < need - 1) {
			break;
		}
		memmove(buf, buf + have - (need - 1), need - 1);
		have = need - 1;
	}
	fclose(f);
	return found;
}

ota_result_t firmware_update_model_check(const char *path) {
	if (!path || !*path) {
		return OTA_MODEL_MISMATCH;
	}

	const char *stem = firmware_update_stem();
	if (!stem || !*stem) {
		return OTA_NO_MODEL;
	}

	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	char wanted[80];
	snprintf(wanted, sizeof(wanted), "%s.upt", stem);
	if (strcasecmp(base, wanted) != 0) {
		return OTA_MODEL_MISMATCH;
	}

	const sysinfo_model_t *model = sysinfo_model();
	if (!model || !model->name[0]) {
		return OTA_NO_MODEL;
	}
	char marker[32];
	model_marker(model->name, marker, sizeof(marker));
	return file_contains(path, marker) ? OTA_OK : OTA_MODEL_MISMATCH;
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
