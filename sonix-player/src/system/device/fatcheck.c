#include "fatcheck.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

// What an entry of the allocation table can say besides "the next cluster is
// this one". Only the low 28 bits of an entry count; the top four are kept as
// they are found.
#define ENTRY_MASK 0x0FFFFFFFu
#define ENTRY_FREE 0u
#define ENTRY_BAD 0x0FFFFFF7u
#define ENTRY_LAST 0x0FFFFFF8u // this and above: the chain ends here

// A folder entry.
#define ENTRY_BYTES 32
#define ATTR_VOLUME 0x08
#define ATTR_FOLDER 0x10
#define ATTR_LONG_NAME 0x0F // all four low bits: a piece of a long name, not an entry of its own
#define ATTR_LONG_MASK 0x3F

// The largest cluster this reads: far above anything a card is formatted
// with, and a bound on the one buffer that is as large as a cluster.
#define MAX_CLUSTER_BYTES (1024 * 1024)
// A table is written back in pieces of this many entries.
#define CHUNK_ENTRIES 16384

typedef struct {
	int fd;
	uint32_t sector_bytes, cluster_bytes;
	uint32_t reserved_sectors, fat_count, fat_sectors;
	uint32_t root_cluster, info_sector, backup_sector;
	uint32_t clusters; // how many the card has; they are numbered from 2
	off_t fat_start;   // the first copy of the table, in bytes from the start
	off_t data_start;  // cluster 2

	const uint8_t *fat; // the first copy, as it is on the card
	void *map;			// what to unmap or free to let go of it
	size_t map_bytes;
	bool map_is_heap;

	uint8_t *owned;	  // one bit per cluster: something in the tree reaches it
	uint8_t *cluster; // one cluster of a folder

	uint32_t *folders_todo; // folders found and not yet read, by first cluster
	uint32_t todo_count, todo_room;

	fatcheck_result_t *out;
} card_t;

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static void put_le32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static bool valid_cluster(const card_t *card, uint32_t c) { return c >= 2 && c - 2 < card->clusters; }
static uint32_t fat_entry(const card_t *card, uint32_t c) { return le32(card->fat + (size_t)c * 4) & ENTRY_MASK; }
static bool is_owned(const card_t *card, uint32_t c) { return card->owned[c >> 3] & (1u << (c & 7)); }
static void set_owned(card_t *card, uint32_t c) { card->owned[c >> 3] |= (uint8_t)(1u << (c & 7)); }

// Something is wrong that this does not repair. The first thing found is the
// one that is kept: what comes after is usually its consequence.
static bool damaged(card_t *card, const char *format, ...) {
	if (card->out->status != FATCHECK_DAMAGED) {
		card->out->status = FATCHECK_DAMAGED;
		va_list args;
		va_start(args, format);
		vsnprintf(card->out->problem, sizeof(card->out->problem), format, args);
		va_end(args);
	}
	return false;
}

static bool failed(card_t *card, fatcheck_status_t status, const char *what) {
	card->out->status = status;
	snprintf(card->out->problem, sizeof(card->out->problem), "%s", what);
	return false;
}

// All of `size` bytes at `offset`, or nothing.
static bool read_at(int fd, void *buf, size_t size, off_t offset) {
	uint8_t *p = buf;
	while (size > 0) {
		ssize_t got = pread(fd, p, size, offset);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			return false;
		}
		p += got;
		size -= (size_t)got;
		offset += got;
	}
	return true;
}

static bool write_at(int fd, const void *buf, size_t size, off_t offset) {
	const uint8_t *p = buf;
	while (size > 0) {
		ssize_t put = pwrite(fd, p, size, offset);
		if (put < 0 && errno == EINTR) {
			continue;
		}
		if (put <= 0) {
			return false;
		}
		p += put;
		size -= (size_t)put;
		offset += put;
	}
	return true;
}

// ---------------------------------------------------------------------------
// the first sector
// ---------------------------------------------------------------------------

static bool read_layout(card_t *card) {
	uint8_t boot[512];
	if (!read_at(card->fd, boot, sizeof(boot), 0)) {
		return failed(card, FATCHECK_IO, "the first sector cannot be read");
	}
	// FAT32 says so itself at offset 82. exFAT and the older FATs are told
	// apart by the same bytes the storage code looks at.
	if (boot[510] != 0x55 || boot[511] != 0xAA || memcmp(boot + 82, "FAT32", 5) != 0) {
		return failed(card, FATCHECK_UNSUPPORTED, "not FAT32");
	}

	card->sector_bytes = le16(boot + 11);
	uint32_t cluster_sectors = boot[13];
	card->reserved_sectors = le16(boot + 14);
	card->fat_count = boot[16];
	uint32_t total_sectors = le32(boot + 32);
	card->fat_sectors = le32(boot + 36);
	card->root_cluster = le32(boot + 44) & ENTRY_MASK;
	card->info_sector = le16(boot + 48);
	card->backup_sector = le16(boot + 50);

	bool sector_ok = card->sector_bytes == 512 || card->sector_bytes == 1024 || card->sector_bytes == 2048 || card->sector_bytes == 4096;
	bool cluster_ok = cluster_sectors != 0 && (cluster_sectors & (cluster_sectors - 1)) == 0;
	// A FAT32 volume has no fixed root folder and no 16-bit counts; one that
	// claims either is not what it says it is.
	if (!sector_ok || !cluster_ok || card->reserved_sectors == 0 || card->fat_count < 1 || card->fat_count > 2 || card->fat_sectors == 0 || le16(boot + 17) != 0 || le16(boot + 19) != 0 || le16(boot + 22) != 0 || total_sectors == 0) {
		return damaged(card, "the first sector does not describe a FAT32 volume");
	}
	uint64_t cluster_bytes = (uint64_t)card->sector_bytes * cluster_sectors;
	if (cluster_bytes > MAX_CLUSTER_BYTES) {
		return failed(card, FATCHECK_UNSUPPORTED, "clusters larger than this reads");
	}
	card->cluster_bytes = (uint32_t)cluster_bytes;

	uint64_t data_sector = (uint64_t)card->reserved_sectors + (uint64_t)card->fat_count * card->fat_sectors;
	if (data_sector >= total_sectors) {
		return damaged(card, "the tables are larger than the volume");
	}
	uint64_t clusters = (total_sectors - data_sector) / cluster_sectors;
	// Fewer than this and the volume is FAT16 by definition, whatever it is
	// labelled; more than the numbering allows cannot be.
	if (clusters < 65525) {
		return failed(card, FATCHECK_UNSUPPORTED, "too few clusters to be FAT32");
	}
	if (clusters > 0x0FFFFFF5u || (uint64_t)card->fat_sectors * card->sector_bytes / 4 < clusters + 2) {
		return damaged(card, "the table is too small for the volume");
	}
	card->clusters = (uint32_t)clusters;
	card->fat_start = (off_t)card->reserved_sectors * card->sector_bytes;
	card->data_start = (off_t)(data_sector * card->sector_bytes);

	// The card must really be as large as the volume on it says.
	off_t device_bytes = lseek(card->fd, 0, SEEK_END);
	if (device_bytes < 0 || (uint64_t)device_bytes < (uint64_t)total_sectors * card->sector_bytes) {
		return damaged(card, "the volume is larger than the card");
	}
	if (!valid_cluster(card, card->root_cluster)) {
		return damaged(card, "the root folder is nowhere");
	}
	return true;
}

// The first copy of the table, readable as one piece of memory. Mapped, so
// that it costs page cache and not the player's own memory: on a large card
// it is tens of megabytes. Read into memory where mapping is refused and it
// is small enough.
static bool open_table(card_t *card) {
	size_t table_bytes = ((size_t)card->clusters + 2) * 4;
	long page = sysconf(_SC_PAGESIZE);
	if (page <= 0) {
		page = 4096;
	}
	off_t base = card->fat_start - card->fat_start % page;
	size_t lead = (size_t)(card->fat_start - base);

	void *map = mmap(NULL, lead + table_bytes, PROT_READ, MAP_SHARED, card->fd, base);
	if (map != MAP_FAILED) {
		card->map = map;
		card->map_bytes = lead + table_bytes;
		card->fat = (const uint8_t *)map + lead;
		return true;
	}
	if (table_bytes > 16u * 1024 * 1024) {
		return failed(card, FATCHECK_NO_MEMORY, "the table cannot be mapped");
	}
	uint8_t *copy = malloc(table_bytes);
	if (!copy) {
		return failed(card, FATCHECK_NO_MEMORY, "no memory for the table");
	}
	if (!read_at(card->fd, copy, table_bytes, card->fat_start)) {
		free(copy);
		return failed(card, FATCHECK_IO, "the table cannot be read");
	}
	card->map = copy;
	card->map_is_heap = true;
	card->fat = copy;
	return true;
}

static void close_table(card_t *card) {
	if (!card->map) {
		return;
	}
	if (card->map_is_heap) {
		free(card->map);
	} else {
		munmap(card->map, card->map_bytes);
	}
	card->map = NULL;
	card->fat = NULL;
}

// The second copy must say what the first does. They are written together,
// so two that differ mean a write was cut short, and there is no telling
// from here which of them is the whole one.
//
// From cluster 2 on: the two entries before it are not clusters, and one of
// them carries flags that only the first copy is sure to have.
static bool tables_agree(card_t *card) {
	if (card->fat_count < 2) {
		return true;
	}
	uint8_t *chunk = malloc(CHUNK_ENTRIES * 4);
	if (!chunk) {
		return failed(card, FATCHECK_NO_MEMORY, "no memory to compare the tables");
	}
	off_t second = card->fat_start + (off_t)card->fat_sectors * card->sector_bytes;
	bool ok = true;
	for (uint32_t first = 2; ok && first < card->clusters + 2; first += CHUNK_ENTRIES) {
		uint32_t count = card->clusters + 2 - first;
		if (count > CHUNK_ENTRIES) {
			count = CHUNK_ENTRIES;
		}
		if (!read_at(card->fd, chunk, (size_t)count * 4, second + (off_t)first * 4)) {
			ok = failed(card, FATCHECK_IO, "the second table cannot be read");
		} else if (memcmp(chunk, card->fat + (size_t)first * 4, (size_t)count * 4) != 0) {
			ok = damaged(card, "the two copies of the table differ near cluster %u", (unsigned)first);
		}
	}
	free(chunk);
	return ok;
}

// ---------------------------------------------------------------------------
// the tree
// ---------------------------------------------------------------------------

// Takes cluster `c` for the chain being followed and says what follows it:
// the next cluster, or 0 at the end of the chain. False when the chain is
// broken there.
static bool take_cluster(card_t *card, uint32_t c, uint32_t *next) {
	if (!valid_cluster(card, c)) {
		return damaged(card, "a chain points at cluster %u, which the card does not have", (unsigned)c);
	}
	// Reached before: by another file, or by this chain coming round again.
	if (is_owned(card, c)) {
		return damaged(card, "cluster %u belongs to two chains", (unsigned)c);
	}
	set_owned(card, c);
	uint32_t entry = fat_entry(card, c);
	if (entry >= ENTRY_LAST) {
		*next = 0;
		return true;
	}
	if (entry == ENTRY_FREE) {
		return damaged(card, "a chain runs into free space at cluster %u", (unsigned)c);
	}
	if (entry == ENTRY_BAD) {
		return damaged(card, "a chain runs into a bad cluster at %u", (unsigned)c);
	}
	*next = entry;
	return true;
}

// Follows a file's chain from `first` and says how many clusters it has.
static bool follow_file(card_t *card, uint32_t first, uint32_t *length) {
	uint32_t n = 0;
	for (uint32_t c = first; c != 0;) {
		uint32_t next = 0;
		if (!take_cluster(card, c, &next)) {
			return false;
		}
		n++;
		c = next;
	}
	*length = n;
	return true;
}

static bool remember_folder(card_t *card, uint32_t first) {
	if (card->todo_count == card->todo_room) {
		uint32_t room = card->todo_room ? card->todo_room * 2 : 256;
		uint32_t *grown = realloc(card->folders_todo, (size_t)room * sizeof(*grown));
		if (!grown) {
			return failed(card, FATCHECK_NO_MEMORY, "no memory for the list of folders");
		}
		card->folders_todo = grown;
		card->todo_room = room;
	}
	card->folders_todo[card->todo_count++] = first;
	return true;
}

// One entry of a folder. `*end` is set at the entry that closes the folder.
static bool read_entry(card_t *card, const uint8_t *e, bool *end) {
	if (e[0] == 0x00) {
		*end = true; // nothing has ever been written from here on
		return true;
	}
	if (e[0] == 0xE5) {
		return true; // deleted
	}
	uint8_t attr = e[11];
	if ((attr & ATTR_LONG_MASK) == ATTR_LONG_NAME || (attr & ATTR_VOLUME)) {
		return true; // part of the next entry's long name, or the card's label
	}
	// "." and "..": the folder itself and its parent, which own nothing new.
	if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) {
		return true;
	}

	uint32_t first = (le16(e + 20) << 16 | le16(e + 26)) & ENTRY_MASK;
	uint32_t size = le32(e + 28);

	if (attr & ATTR_FOLDER) {
		card->out->folders++;
		if (first == 0) {
			return damaged(card, "a folder has no cluster");
		}
		return remember_folder(card, first);
	}

	card->out->files++;
	if (first == 0) {
		// An empty file has no cluster. One with a size and no cluster has
		// lost its contents.
		return size == 0 ? true : damaged(card, "a file of %u bytes has no cluster", (unsigned)size);
	}
	uint32_t length;
	if (!follow_file(card, first, &length)) {
		return false;
	}
	// More clusters than the size needs is what an interrupted write leaves:
	// the space is the file's, and stays so. Fewer means the end of the file
	// is not on the card.
	uint64_t needed = ((uint64_t)size + card->cluster_bytes - 1) / card->cluster_bytes;
	if (length < needed) {
		return damaged(card, "a file of %u bytes has only %u clusters", (unsigned)size, (unsigned)length);
	}
	return true;
}

// Reads the folder that starts at `first`: takes its clusters and looks at
// every entry in them.
static bool read_folder(card_t *card, uint32_t first) {
	bool end = false;
	for (uint32_t c = first; c != 0;) {
		uint32_t next = 0;
		if (!take_cluster(card, c, &next)) {
			return false;
		}
		// After the closing entry the rest of the chain is still the folder's,
		// but there is nothing in it to read.
		if (!end) {
			off_t at = card->data_start + (off_t)(c - 2) * card->cluster_bytes;
			if (!read_at(card->fd, card->cluster, card->cluster_bytes, at)) {
				return failed(card, FATCHECK_IO, "a folder cannot be read");
			}
			for (uint32_t i = 0; !end && i + ENTRY_BYTES <= card->cluster_bytes; i += ENTRY_BYTES) {
				if (!read_entry(card, card->cluster + i, &end)) {
					return false;
				}
			}
		}
		c = next;
	}
	return true;
}

// The whole tree, from the root down. Not by a function calling itself: a
// folder found is put on a list and read in its turn, so that how deep the
// folders go costs no stack.
static bool read_tree(card_t *card) {
	if (!remember_folder(card, card->root_cluster)) {
		return false;
	}
	while (card->todo_count > 0) {
		uint32_t first = card->folders_todo[--card->todo_count];
		if (!read_folder(card, first)) {
			return false;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// lost clusters
// ---------------------------------------------------------------------------

// Turns the map of owned clusters into the map of lost ones -- taken
// according to the table, reached by nothing -- and counts them. A cluster
// marked bad is neither: it is set aside, not lost.
static void find_lost(card_t *card, uint32_t *lost, uint32_t *free_clusters) {
	uint32_t n_lost = 0, n_free = 0;
	for (uint32_t c = 2; c < card->clusters + 2; c++) {
		uint32_t entry = fat_entry(card, c);
		bool is_lost = entry != ENTRY_FREE && entry != ENTRY_BAD && !is_owned(card, c);
		if (is_lost) {
			card->owned[c >> 3] |= (uint8_t)(1u << (c & 7));
			n_lost++;
		} else {
			card->owned[c >> 3] &= (uint8_t)~(1u << (c & 7));
			n_free += entry == ENTRY_FREE;
		}
	}
	*lost = n_lost;
	*free_clusters = n_free;
}

// Frees, in one copy of the table, every cluster the map marks. Each piece
// of the table is read from the card, changed and written back only if
// something in it changed.
static bool free_in_table(card_t *card, off_t table, uint8_t *chunk) {
	for (uint32_t first = 2; first < card->clusters + 2; first += CHUNK_ENTRIES) {
		uint32_t count = card->clusters + 2 - first;
		if (count > CHUNK_ENTRIES) {
			count = CHUNK_ENTRIES;
		}
		off_t at = table + (off_t)first * 4;
		if (!read_at(card->fd, chunk, (size_t)count * 4, at)) {
			return false;
		}
		bool changed = false;
		for (uint32_t i = 0; i < count; i++) {
			uint32_t c = first + i;
			if (!(card->owned[c >> 3] & (1u << (c & 7)))) {
				continue;
			}
			uint32_t entry = le32(chunk + (size_t)i * 4);
			if ((entry & ENTRY_MASK) != ENTRY_FREE) {
				put_le32(chunk + (size_t)i * 4, entry & ~ENTRY_MASK);
				changed = true;
			}
		}
		if (changed && !write_at(card->fd, chunk, (size_t)count * 4, at)) {
			return false;
		}
	}
	return true;
}

// The volume keeps a count of its free clusters in a sector of its own, as a
// hint for whoever mounts it. Brought up to date in one copy of that sector,
// when it is where the first sector says and is what it should be.
static void write_free_count(card_t *card, uint32_t sector, uint32_t free_clusters, uint8_t *buf) {
	if (sector == 0 || sector >= card->reserved_sectors) {
		return;
	}
	off_t at = (off_t)sector * card->sector_bytes;
	if (!read_at(card->fd, buf, card->sector_bytes, at)) {
		return;
	}
	if (le32(buf) != 0x41615252u || le32(buf + 484) != 0x61417272u || buf[510] != 0x55 || buf[511] != 0xAA) {
		return;
	}
	put_le32(buf + 488, free_clusters);
	// A hint that is wrong only costs the next mount a count of its own, so a
	// write that fails here does not undo the repair.
	(void)write_at(card->fd, buf, card->sector_bytes, at);
}

static bool free_lost(card_t *card, uint32_t free_after) {
	uint8_t *chunk = malloc(CHUNK_ENTRIES * 4 > card->sector_bytes ? CHUNK_ENTRIES * 4 : card->sector_bytes);
	if (!chunk) {
		return failed(card, FATCHECK_NO_MEMORY, "no memory to write the table");
	}
	bool ok = true;
	for (uint32_t copy = 0; ok && copy < card->fat_count; copy++) {
		off_t table = card->fat_start + (off_t)copy * card->fat_sectors * card->sector_bytes;
		ok = free_in_table(card, table, chunk);
	}
	if (ok) {
		write_free_count(card, card->info_sector, free_after, chunk);
		if (card->backup_sector != 0) {
			write_free_count(card, card->backup_sector + card->info_sector, free_after, chunk);
		}
	}
	free(chunk);
	if (!ok || fsync(card->fd) != 0) {
		return failed(card, FATCHECK_IO, "writing the table failed");
	}
	return true;
}

// ---------------------------------------------------------------------------

bool fatcheck_run(const char *device, bool repair, long expect_files, long expect_folders, fatcheck_result_t *out) {
	memset(out, 0, sizeof(*out));
	out->status = FATCHECK_IO;
	card_t card = {.fd = -1, .out = out};

	// A card node is opened for this alone: the kernel refuses that while the
	// card is mounted, which is the one state this must never run in. A
	// mounted filesystem has changes in memory that the card does not hold
	// yet, and clusters that look lost from here may be a file half written.
	struct stat st;
	int flags = (repair ? O_RDWR : O_RDONLY) | O_CLOEXEC;
	if (stat(device, &st) == 0 && S_ISBLK(st.st_mode)) {
		flags |= O_EXCL;
	}
	card.fd = open(device, flags);
	if (card.fd < 0) {
		snprintf(out->problem, sizeof(out->problem), "cannot open the card: %s", strerror(errno));
		return false;
	}

	bool ok = read_layout(&card) && open_table(&card);
	if (ok) {
		card.owned = calloc(((size_t)card.clusters + 2 + 7) / 8, 1);
		card.cluster = malloc(card.cluster_bytes);
		if (!card.owned || !card.cluster) {
			ok = failed(&card, FATCHECK_NO_MEMORY, "no memory for the map of clusters");
		}
	}
	// The status is only a result once the whole tree has been read without
	// finding fault: until then it is whatever stopped it.
	ok = ok && tables_agree(&card) && read_tree(&card);

	if (ok) {
		uint32_t lost = 0, free_clusters = 0;
		find_lost(&card, &lost, &free_clusters);
		out->lost_clusters = lost;
		out->lost_bytes = (uint64_t)lost * card.cluster_bytes;
		out->status = lost ? FATCHECK_LOST : FATCHECK_CLEAN;

		if (lost && repair) {
			bool counted = expect_files >= 0 && expect_folders >= 0;
			if (counted && ((uint32_t)expect_files != out->files || (uint32_t)expect_folders != out->folders)) {
				out->status = FATCHECK_MISMATCH;
				snprintf(out->problem, sizeof(out->problem), "read %u files in %u folders where the card showed %ld in %ld", (unsigned)out->files, (unsigned)out->folders, expect_files, expect_folders);
			} else {
				// The map is all that is needed from here on. The table is let
				// go of before it is written, so that nothing here reads its
				// own half-finished work.
				close_table(&card);
				ok = free_lost(&card, free_clusters + lost);
				if (ok) {
					out->repaired = true;
				}
			}
		}
	}

	close_table(&card);
	free(card.owned);
	free(card.cluster);
	free(card.folders_todo);
	close(card.fd);
	// A card that was read through and found damaged is a result too.
	return ok || out->status == FATCHECK_DAMAGED || out->status == FATCHECK_UNSUPPORTED;
}
