// posix_fadvise
#define _GNU_SOURCE

#include "cardtest.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "src/system/device/system.h"

#define CHUNK_BYTES (64u * 1024u)
#define TEST_FILE ".local/cardtest.tmp"

static int64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// What chunk `index` of the file holds: bytes no two chunks share, so a card
// that returns one chunk in place of another is caught, and not all zeros,
// which says nothing when it comes back.
static void fill(unsigned char *chunk, uint32_t index) {
	uint32_t x = 0x9E3779B9u * (index + 1u);
	for (size_t i = 0; i < CHUNK_BYTES; i += 4) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		memcpy(chunk + i, &x, 4);
	}
}

static int kb_per_second(uint64_t bytes, int64_t us) {
	if (us <= 0) {
		us = 1;
	}
	return (int)(bytes * 1000u / (uint64_t)us);
}

void cardtest_run(cardtest_result_t *out) {
	out->status = CARDTEST_NO_CARD;
	out->write_kb_s = 0;
	out->read_kb_s = 0;

	const char *root = storage_sd_root();
	if (!root || !storage_card_usable() || !storage_sd_writable()) {
		return;
	}

	struct statvfs vfs;
	if (statvfs(root, &vfs) != 0) {
		return;
	}
	if ((uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize < 2ull * CARDTEST_BYTES) {
		out->status = CARDTEST_NO_ROOM;
		return;
	}

	char path[PATH_MAX];
	if (snprintf(path, sizeof(path), "%s/%s", root, TEST_FILE) >= (int)sizeof(path)) {
		return;
	}
	char folder[PATH_MAX];
	snprintf(folder, sizeof(folder), "%s/.local", root);
	mkdir(folder, 0755); // there already on a card the player has used

	unsigned char *chunk = malloc(CHUNK_BYTES);
	unsigned char *back = malloc(CHUNK_BYTES);
	if (!chunk || !back) {
		free(chunk);
		free(back);
		out->status = CARDTEST_IO_ERROR;
		return;
	}
	const uint32_t chunks = CARDTEST_BYTES / CHUNK_BYTES;
	out->status = CARDTEST_IO_ERROR;

	// ---- writing: timed up to the moment the card has it ----
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		goto done;
	}
	int64_t start = now_us();
	for (uint32_t i = 0; i < chunks; i++) {
		fill(chunk, i);
		size_t put = 0;
		while (put < CHUNK_BYTES) {
			ssize_t n = write(fd, chunk + put, CHUNK_BYTES - put);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				close(fd);
				goto done;
			}
			put += (size_t)n;
		}
	}
	if (fsync(fd) != 0) {
		close(fd);
		goto done;
	}
	out->write_kb_s = kb_per_second(CARDTEST_BYTES, now_us() - start);
	// The copy the kernel kept is let go, or the reading below would be timing
	// memory.
	posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	close(fd);

	// ---- reading it back, and comparing ----
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		goto done;
	}
	posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	bool same = true;
	start = now_us();
	for (uint32_t i = 0; i < chunks; i++) {
		size_t got = 0;
		while (got < CHUNK_BYTES) {
			ssize_t n = read(fd, back + got, CHUNK_BYTES - got);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) {
				close(fd);
				goto done;
			}
			got += (size_t)n;
		}
		fill(chunk, i);
		if (memcmp(chunk, back, CHUNK_BYTES) != 0) {
			same = false;
		}
	}
	out->read_kb_s = kb_per_second(CARDTEST_BYTES, now_us() - start);
	close(fd);
	out->status = same ? CARDTEST_OK : CARDTEST_MISMATCH;

done:
	unlink(path);
	free(chunk);
	free(back);
	printf("cardtest: status %d, write %d kB/s, read %d kB/s\n", (int)out->status, out->write_kb_s, out->read_kb_s);
}
