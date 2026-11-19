// SD-card update checks, on the host, against the firmware.c the player
// builds. HOST_BUILD keeps firmware_update_start() a no-op, so the only
// dependencies to stub are config, sysinfo and the card mount point.
//
// Built and run by `make test-firmware-update`. Not part of host or target.

#include "src/system/device/firmware.h"
#include "src/system/device/sysinfo.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const sysinfo_model_t MODEL_R1 = {
	.name = "HiBy R1",
	.update_stem = "r1",
	.serial_prefix = "R1",
};

static const sysinfo_model_t MODEL_R3 = {
	.name = "HiBy R3 Pro II",
	.update_stem = "r3proii",
	.serial_prefix = "R3PII",
};

static const sysinfo_model_t *active_model = &MODEL_R1;
static char config_name[64] = "";

const char *config_get(const char *section, const char *key, const char *fallback) {
	(void)section;
	(void)key;
	return config_name[0] ? config_name : fallback;
}

const sysinfo_model_t *sysinfo_model(void) { return active_model; }

const char *sysinfo_device_name(void) { return active_model ? active_model->name : ""; }

const char *storage_sd_root(void) { return "."; }

#define CHECK(cond, name)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              \
	do {                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               \
		if (cond) {                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    \
			printf("ok   %s\n", name);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 \
		} else {                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       \
			printf("FAIL %s (%s:%d)\n", name, __FILE__, __LINE__);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     \
			exit(1);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   \
		}                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              \
	} while (0)

static char dir[] = "/tmp/firmware-update-test.XXXXXX";

static void write_image(const char *name, const char *content) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		printf("FAIL cannot write %s\n", path);
		exit(1);
	}
	fputs(content, f);
	fclose(f);
}

static const char *image_path(const char *name) {
	static char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return path;
}

// 1. With no config override, the stem comes from the running model.
static void case_stem_from_model(void) {
	active_model = &MODEL_R1;
	config_name[0] = '\0';
	CHECK(strcmp(firmware_update_stem(), "r1") == 0, "stem from model");
}

// 2. A config override supplies the stem when one is set.
static void case_stem_from_config(void) {
	snprintf(config_name, sizeof(config_name), "%s", "custom");
	CHECK(strcmp(firmware_update_stem(), "custom") == 0, "stem from config");
	config_name[0] = '\0';
}

// 3. An R1 image named for the R1 passes the check.
static void case_matching_r1(void) {
	active_model = &MODEL_R1;
	write_image("r1.upt", "boot loader junk HiBy R1 system-info.json more junk");
	CHECK(firmware_update_model_check(image_path("r1.upt")) == OTA_OK, "matching r1 accepted");
}

// 4. An R3 image renamed to r1.upt is refused before recovery is armed.
static void case_r3_image_renamed_r1(void) {
	active_model = &MODEL_R1;
	write_image("r1.upt", "boot loader junk HiBy R3 Pro II more junk");
	CHECK(firmware_update_model_check(image_path("r1.upt")) == OTA_MODEL_MISMATCH, "r3 image renamed r1 refused");
}

// 5. The file name must name this model too.
static void case_wrong_filename(void) {
	active_model = &MODEL_R1;
	write_image("r3proii.upt", "junk HiBy R1 junk");
	CHECK(firmware_update_model_check(image_path("r3proii.upt")) == OTA_MODEL_MISMATCH, "wrong filename refused");
}

// 6. The stock R3 image spells the model without spaces; that still matches.
static void case_matching_r3_compact(void) {
	active_model = &MODEL_R3;
	write_image("r3proii.upt", "junk HiBy R3PROII junk");
	CHECK(firmware_update_model_check(image_path("r3proii.upt")) == OTA_OK, "compact r3 marker accepted");
}

// 7. A Sonix R3 image spells the model with spaces; that matches as well.
static void case_matching_r3_spaced(void) {
	active_model = &MODEL_R3;
	write_image("r3proii.upt", "junk HiBy R3 Pro II junk");
	CHECK(firmware_update_model_check(image_path("r3proii.upt")) == OTA_OK, "spaced r3 marker accepted");
}

// 8. An R1 image renamed to r3proii.upt is refused.
static void case_r1_image_renamed_r3(void) {
	active_model = &MODEL_R3;
	write_image("r3proii.upt", "junk HiBy R1 junk");
	CHECK(firmware_update_model_check(image_path("r3proii.upt")) == OTA_MODEL_MISMATCH, "r1 image renamed r3 refused");
}

// 9. A missing file cannot pass the check.
static void case_missing_file(void) {
	active_model = &MODEL_R1;
	CHECK(firmware_update_model_check(image_path("absent.upt")) == OTA_MODEL_MISMATCH, "missing image refused");
}

int main(void) {
	if (!mkdtemp(dir)) {
		printf("FAIL cannot make a temporary directory\n");
		return 1;
	}
	case_stem_from_model();
	case_stem_from_config();
	case_matching_r1();
	case_r3_image_renamed_r1();
	case_wrong_filename();
	case_matching_r3_compact();
	case_matching_r3_spaced();
	case_r1_image_renamed_r3();
	case_missing_file();
	printf("firmware update model checks: all passed\n");
	return 0;
}
