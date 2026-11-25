// SD-card update checks, on the host, against the firmware.c the player
// builds. HOST_BUILD keeps firmware_update_start() a no-op, so the only
// dependencies to stub are config, sysinfo and the card mount point.
//
// Built and run by `make test-firmware-update`. Not part of host or target.

#include "src/system/device/firmware.h"
#include "src/system/device/sysinfo.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const sysinfo_model_t MODEL_R1 = {
	.name = "HiBy R1",
	.update_stem = "r1",
	.compat_id = "hiby_r1_x1600_auto_rf_auto",
	.serial_prefix = "R1",
};

// Its images carry no ota_compatibility.in, and that is what is expected of
// them: see compat_id in sysinfo.h.
static const sysinfo_model_t MODEL_R3 = {
	.name = "HiBy R3 Pro II",
	.update_stem = "r3proii",
	.compat_id = NULL,
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

// A .upt as small as the check will accept: a volume descriptor at sector 16,
// a root directory at 17, OTA_V0 at 18 and, when `compat` is not NULL, the
// member that names the hardware at 19. Enough of ISO9660 to exercise the walk
// the player does, rather than a text file with the name written in it.
#define ISO_SECTORS 20
#define SECTOR 2048

static void put32(unsigned char *p, uint32_t v) {
	p[0] = (unsigned char)(v & 0xff);
	p[1] = (unsigned char)((v >> 8) & 0xff);
	p[2] = (unsigned char)((v >> 16) & 0xff);
	p[3] = (unsigned char)((v >> 24) & 0xff);
	// the big-endian copy ISO9660 keeps beside every little-endian one
	p[4] = p[3];
	p[5] = p[2];
	p[6] = p[1];
	p[7] = p[0];
}

// One directory record. Returns the length written, which is padded to an even
// number as ISO9660 requires.
static size_t put_record(unsigned char *p, uint32_t extent, uint32_t len, bool is_dir, const char *name,
						 size_t name_len) {
	size_t total = 33 + name_len;
	if (total % 2) {
		total++;
	}
	memset(p, 0, total);
	p[0] = (unsigned char)total;
	put32(p + 2, extent);
	put32(p + 10, len);
	p[25] = is_dir ? 0x02 : 0x00;
	p[28] = 1; // volume sequence number, both byte orders
	p[31] = 1;
	p[32] = (unsigned char)name_len;
	memcpy(p + 33, name, name_len);
	return total;
}

static void write_iso(const char *name, const char *compat) {
	static unsigned char img[ISO_SECTORS * SECTOR];
	memset(img, 0, sizeof(img));

	unsigned char *pvd = img + 16 * SECTOR;
	pvd[0] = 1; // primary volume descriptor
	memcpy(pvd + 1, "CD001", 5);
	pvd[6] = 1;
	pvd[128] = SECTOR & 0xff; // logical block size, little endian
	pvd[129] = (SECTOR >> 8) & 0xff;
	put_record(pvd + 156, 17, SECTOR, true, "\0", 1); // the root, named "."

	unsigned char *root = img + 17 * SECTOR;
	size_t o = 0;
	o += put_record(root + o, 17, SECTOR, true, "\0", 1);
	o += put_record(root + o, 17, SECTOR, true, "\1", 1);
	put_record(root + o, 18, SECTOR, true, "OTA_V0", 6);

	unsigned char *ota = img + 18 * SECTOR;
	o = 0;
	o += put_record(ota + o, 18, SECTOR, true, "\0", 1);
	o += put_record(ota + o, 17, SECTOR, true, "\1", 1);
	if (compat) {
		char line[128];
		int n = snprintf(line, sizeof(line), "compatibility=%s\n", compat);
		put_record(ota + o, 19, (uint32_t)n, false, "OTA_COMP.IN;1", 13);
		memcpy(img + 19 * SECTOR, line, (size_t)n);
	} else {
		// No such member: an image that names no hardware, as the R3 Pro II's do.
		put_record(ota + o, 19, 1, false, "OTA_V0.OK;1", 11);
		img[19 * SECTOR] = 'y';
	}

	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		printf("FAIL cannot write %s\n", path);
		exit(1);
	}
	fwrite(img, 1, sizeof(img), f);
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

// 3. An R1 image named for the R1, declaring the R1, passes.
static void case_matching_r1(void) {
	active_model = &MODEL_R1;
	write_iso("r1.upt", "hiby_r1_x1600_auto_rf_auto");
	CHECK(firmware_update_model_check(image_path("r1.upt")) == OTA_OK, "matching r1 accepted");
}

// 4. An R3 image renamed to r1.upt is refused: it declares no hardware where
//    every R1 image declares the R1. This is the case that would brick.
static void case_r3_image_renamed_r1(void) {
	active_model = &MODEL_R1;
	write_iso("r1.upt", NULL);
	CHECK(firmware_update_model_check(image_path("r1.upt")) == OTA_MODEL_MISMATCH, "r3 image renamed r1 refused");
}

// 5. The file name must name this model too, whatever the image declares.
static void case_wrong_filename(void) {
	active_model = &MODEL_R1;
	write_iso("r3proii.upt", "hiby_r1_x1600_auto_rf_auto");
	CHECK(firmware_update_model_check(image_path("r3proii.upt")) == OTA_MODEL_MISMATCH, "wrong filename refused");
}

// 6. An R3 Pro II image declares no hardware, and that is what its player
//    expects of it.
static void case_matching_r3(void) {
	active_model = &MODEL_R3;
	write_iso("r3proii.upt", NULL);
	CHECK(firmware_update_model_check(image_path("r3proii.upt")) == OTA_OK, "r3 image without an id accepted");
}

// 7. An R1 image renamed to r3proii.upt is refused: it declares an id where an
//    R3 Pro II image declares none. The other way the player could be bricked.
static void case_r1_image_renamed_r3(void) {
	active_model = &MODEL_R3;
	write_iso("r3proii.upt", "hiby_r1_x1600_auto_rf_auto");
	CHECK(firmware_update_model_check(image_path("r3proii.upt")) == OTA_MODEL_MISMATCH, "r1 image renamed r3 refused");
}

// 8. Some third player's id is not this one's either.
static void case_unknown_id(void) {
	active_model = &MODEL_R1;
	write_iso("r1.upt", "hiby_r4_whatever");
	CHECK(firmware_update_model_check(image_path("r1.upt")) == OTA_MODEL_MISMATCH, "another id refused");
}

// 9. A file that is not an ISO image at all is refused, rather than read as
//    declaring nothing -- which on the R3 Pro II would otherwise pass.
static void case_not_an_image(void) {
	active_model = &MODEL_R3;
	write_image("r3proii.upt", "this is not an iso9660 image, it is a sentence");
	CHECK(firmware_update_model_check(image_path("r3proii.upt")) == OTA_MODEL_MISMATCH, "a non-image refused");
}

// 9b. The same for an image cut short before its directory.
static void case_truncated_image(void) {
	active_model = &MODEL_R1;
	write_image("r1.upt", "CD001 but nothing after it");
	CHECK(firmware_update_model_check(image_path("r1.upt")) == OTA_MODEL_MISMATCH, "a truncated image refused");
}

// 9c. A missing file cannot pass the check.
static void case_missing_file(void) {
	active_model = &MODEL_R1;
	CHECK(firmware_update_model_check(image_path("absent.upt")) == OTA_MODEL_MISMATCH, "missing image refused");
}

// 10. The manual path hashes the image with the same SHA-256 the OTA path uses.
static void case_sha256_known_vector(void) {
	char hex[SHA256_HEX_LEN];
	write_image("abc.bin", "abc");
	CHECK(firmware_file_sha256(image_path("abc.bin"), hex), "sha256 reads the file");
	CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "sha256 of abc matches the published vector");
}

// 11. A file that cannot be read yields no digest.
static void case_sha256_missing(void) {
	char hex[SHA256_HEX_LEN];
	CHECK(!firmware_file_sha256(image_path("absent.bin"), hex), "sha256 of a missing file fails");
}

// 12. The single entry point both update paths call accepts a matching image.
static void case_prepare_ok(void) {
	active_model = &MODEL_R1;
	write_iso("r1.upt", "hiby_r1_x1600_auto_rf_auto");
	CHECK(firmware_update_prepare(image_path("r1.upt")) == OTA_OK, "prepare accepts a matching image");
}

// 13. That entry point refuses another model's image.
static void case_prepare_mismatch(void) {
	active_model = &MODEL_R1;
	write_iso("r1.upt", NULL);
	CHECK(firmware_update_prepare(image_path("r1.upt")) == OTA_MODEL_MISMATCH, "prepare refuses another model");
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
	case_matching_r3();
	case_r1_image_renamed_r3();
	case_unknown_id();
	case_not_an_image();
	case_truncated_image();
	case_missing_file();
	case_sha256_known_vector();
	case_sha256_missing();
	case_prepare_ok();
	case_prepare_mismatch();
	printf("firmware update checks: all passed\n");
	return 0;
}
