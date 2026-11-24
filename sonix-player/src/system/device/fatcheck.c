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
		return failed(card, FATCHECK_UNSUPPORTED, "neither FAT32 nor exFAT");
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
// exFAT
//
// The same question asked of a different book. exFAT does not say which
// clusters are taken in its allocation table: it keeps a bitmap for that, a
// bit per cluster, and the table only strings together the clusters of a file
// that is in pieces. A file in one piece is marked as such in its folder
// entry and has no chain at all. So a cluster is lost here when its bit is
// set and nothing in the tree reaches it, and freeing it is clearing the bit.
// ---------------------------------------------------------------------------

#define X_ENTRY_END 0x00
#define X_ENTRY_IN_USE 0x80 // the top bit of the type: clear on an entry that was deleted
#define X_ENTRY_BITMAP 0x81
#define X_ENTRY_UPCASE 0x82
#define X_ENTRY_FILE 0x85
#define X_ENTRY_STREAM 0xC0
#define X_STREAM_ONE_PIECE 0x02 // "no chain": the clusters follow one another
#define X_ATTR_FOLDER 0x10

#define X_ENTRY_LAST 0xFFFFFFF8u
#define X_ENTRY_BAD 0xFFFFFFF7u

// Folders are read this much at a time. A cluster can be megabytes here.
#define X_BLOCK_BYTES 65536u
// And the largest cluster taken on: the bitmap is read in whole clusters.
#define X_MAX_CLUSTER_BYTES (4u * 1024 * 1024)

typedef struct {
	uint32_t first;
	uint64_t bytes;
	bool one_piece;
} x_extent_t;

typedef struct {
	card_t *card;
	x_extent_t *folders; // found and not yet read
	uint32_t folder_count, folder_room;
	uint32_t *bitmap_clusters; // where the bitmap lies, in order
	uint32_t bitmap_cluster_count;
	uint64_t bitmap_bytes;
	bool bitmap_seen;
} xcard_t;

static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint32_t x_fat_entry(const card_t *card, uint32_t c) { return le32(card->fat + (size_t)c * 4); }

// A walk along the clusters of a file or folder, whichever way they are kept.
typedef struct {
	card_t *card;
	uint32_t next;	// the cluster to hand out; 0 when there are no more
	uint32_t left;	// one piece: how many are still to come
	bool one_piece;
} x_walk_t;

static void x_walk_begin(x_walk_t *walk, card_t *card, const x_extent_t *extent) {
	walk->card = card;
	walk->one_piece = extent->one_piece;
	walk->next = extent->first;
	uint64_t clusters = (extent->bytes + card->cluster_bytes - 1) / card->cluster_bytes;
	// More clusters than the card has is caught at the first one that is not
	// there, so the count only has to fit.
	walk->left = clusters > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)clusters;
	if (walk->one_piece && walk->left == 0) {
		walk->next = 0;
	}
}

// The next cluster of the walk, taken for the tree. 1 with `*c`, 0 at the
// end, -1 when the chain is broken.
static int x_walk_next(x_walk_t *walk, uint32_t *c) {
	card_t *card = walk->card;
	if (walk->next == 0) {
		return 0;
	}
	uint32_t here = walk->next;
	if (!valid_cluster(card, here)) {
		damaged(card, "a file points at cluster %u, which the card does not have", (unsigned)here);
		return -1;
	}
	if (is_owned(card, here)) {
		damaged(card, "cluster %u belongs to two files", (unsigned)here);
		return -1;
	}
	set_owned(card, here);

	if (walk->one_piece) {
		walk->left--;
		walk->next = walk->left ? here + 1 : 0;
	} else {
		uint32_t entry = x_fat_entry(card, here);
		if (entry >= X_ENTRY_LAST) {
			walk->next = 0;
		} else if (entry == X_ENTRY_BAD || !valid_cluster(card, entry)) {
			damaged(card, "the chain is broken at cluster %u", (unsigned)here);
			return -1;
		} else {
			walk->next = entry;
		}
	}
	*c = here;
	return 1;
}

// Takes every cluster of a file. With a chain, the chain has to be as long as
// the file says it is.
static bool x_take_file(card_t *card, const x_extent_t *extent) {
	x_walk_t walk;
	x_walk_begin(&walk, card, extent);
	uint64_t taken = 0;
	uint32_t c;
	int more;
	while ((more = x_walk_next(&walk, &c)) > 0) {
		taken++;
	}
	if (more < 0) {
		return false;
	}
	uint64_t needed = (extent->bytes + card->cluster_bytes - 1) / card->cluster_bytes;
	if (taken < needed) {
		return damaged(card, "a file of %llu bytes has only %llu clusters", (unsigned long long)extent->bytes, (unsigned long long)taken);
	}
	return true;
}

static bool x_remember_folder(xcard_t *x, const x_extent_t *extent) {
	if (x->folder_count == x->folder_room) {
		uint32_t room = x->folder_room ? x->folder_room * 2 : 256;
		x_extent_t *grown = realloc(x->folders, (size_t)room * sizeof(*grown));
		if (!grown) {
			return failed(x->card, FATCHECK_NO_MEMORY, "no memory for the list of folders");
		}
		x->folders = grown;
		x->folder_room = room;
	}
	x->folders[x->folder_count++] = *extent;
	return true;
}

// The bitmap's own clusters, kept in order: they are where the repair writes.
static bool x_take_bitmap(xcard_t *x, uint32_t first, uint64_t bytes) {
	card_t *card = x->card;
	if (x->bitmap_seen) {
		// A second bitmap goes with a second table, which is refused before
		// this is reached; one more here is a root folder gone wrong.
		return damaged(card, "the root folder names two bitmaps");
	}
	x->bitmap_seen = true;
	x->bitmap_bytes = bytes;
	if (bytes < ((uint64_t)card->clusters + 7) / 8) {
		return damaged(card, "the bitmap is too small for the card");
	}
	uint64_t count = (bytes + card->cluster_bytes - 1) / card->cluster_bytes;
	if (count == 0 || count > card->clusters) {
		return damaged(card, "the bitmap has no sensible size");
	}
	x->bitmap_clusters = malloc((size_t)count * sizeof(uint32_t));
	if (!x->bitmap_clusters) {
		return failed(card, FATCHECK_NO_MEMORY, "no memory for the bitmap's place");
	}
	x_extent_t extent = {first, bytes, false};
	x_walk_t walk;
	x_walk_begin(&walk, card, &extent);
	uint32_t c;
	int more = 0;
	while (x->bitmap_cluster_count < count && (more = x_walk_next(&walk, &c)) > 0) {
		x->bitmap_clusters[x->bitmap_cluster_count++] = c;
	}
	if (more < 0) {
		return false;
	}
	if (x->bitmap_cluster_count < count) {
		return damaged(card, "the bitmap's chain is shorter than the bitmap");
	}
	// What the chain holds beyond the bitmap's size is still the bitmap's.
	while ((more = x_walk_next(&walk, &c)) > 0) {
	}
	return more == 0;
}

// Reads one folder. Its entries come in sets: a file entry, then a stream
// entry that says where the contents are, then the name. Only the first two
// matter here. `root` is where the bitmap and the case table are named.
static bool x_read_folder(xcard_t *x, const x_extent_t *folder, bool root) {
	card_t *card = x->card;
	uint32_t block = card->cluster_bytes < X_BLOCK_BYTES ? card->cluster_bytes : X_BLOCK_BYTES;
	x_walk_t walk;
	x_walk_begin(&walk, card, folder);

	bool end = false;
	bool have_file = false; // a file entry is waiting for its stream entry
	bool is_folder = false;
	uint32_t c;
	int more;
	while ((more = x_walk_next(&walk, &c)) > 0) {
		// After the closing entry the rest is still the folder's, with
		// nothing in it to read.
		for (uint32_t at = 0; !end && at < card->cluster_bytes; at += block) {
			off_t offset = card->data_start + (off_t)(c - 2) * card->cluster_bytes + at;
			if (!read_at(card->fd, card->cluster, block, offset)) {
				return failed(card, FATCHECK_IO, "a folder cannot be read");
			}
			for (uint32_t i = 0; !end && i + ENTRY_BYTES <= block; i += ENTRY_BYTES) {
				const uint8_t *e = card->cluster + i;
				uint8_t type = e[0];
				if (type == X_ENTRY_END) {
					end = true;
				} else if (!(type & X_ENTRY_IN_USE)) {
					have_file = false; // deleted, the whole set of it
				} else if (type == X_ENTRY_FILE) {
					if (have_file) {
						return damaged(card, "a file entry has no stream entry after it");
					}
					have_file = true;
					is_folder = le16(e + 4) & X_ATTR_FOLDER;
				} else if (type == X_ENTRY_STREAM && have_file) {
					have_file = false;
					x_extent_t extent = {le32(e + 20), le64(e + 24), e[1] & X_STREAM_ONE_PIECE};
					if (extent.first == 0 && extent.bytes != 0) {
						return damaged(card, "a file of %llu bytes has no cluster", (unsigned long long)extent.bytes);
					}
					if (is_folder) {
						card->out->folders++;
						// A folder always has a cluster, if only for its
						// closing entry.
						if (extent.first == 0) {
							return damaged(card, "a folder has no cluster");
						}
						if (!x_remember_folder(x, &extent)) {
							return false;
						}
					} else {
						card->out->files++;
						if (extent.first != 0 && !x_take_file(card, &extent)) {
							return false;
						}
					}
				} else if (root && type == X_ENTRY_BITMAP) {
					if (!x_take_bitmap(x, le32(e + 20), le64(e + 24))) {
						return false;
					}
				} else if (root && type == X_ENTRY_UPCASE) {
					x_extent_t extent = {le32(e + 20), le64(e + 24), false};
					if (!x_take_file(card, &extent)) {
						return false;
					}
				}
				// Anything else is a name, the card's label, or a kind of
				// entry that owns no clusters.
			}
		}
	}
	if (more < 0) {
		return false;
	}
	if (have_file) {
		return damaged(card, "a folder ends in the middle of an entry");
	}
	return true;
}

// The bitmap, read whole into memory. The caller frees it.
static uint8_t *x_read_bitmap(xcard_t *x) {
	card_t *card = x->card;
	uint8_t *bitmap = malloc((size_t)x->bitmap_cluster_count * card->cluster_bytes);
	if (!bitmap) {
		failed(card, FATCHECK_NO_MEMORY, "no memory for the bitmap");
		return NULL;
	}
	for (uint32_t k = 0; k < x->bitmap_cluster_count; k++) {
		off_t offset = card->data_start + (off_t)(x->bitmap_clusters[k] - 2) * card->cluster_bytes;
		if (!read_at(card->fd, bitmap + (size_t)k * card->cluster_bytes, card->cluster_bytes, offset)) {
			free(bitmap);
			failed(card, FATCHECK_IO, "the bitmap cannot be read");
			return NULL;
		}
	}
	return bitmap;
}

// The first sector of an exFAT volume. Fills in the same fields the FAT32
// reader does, as far as they have a meaning here.
static bool x_read_layout(card_t *card, const uint8_t *boot) {
	uint64_t volume_sectors = le64(boot + 72);
	uint32_t fat_sector = le32(boot + 80), fat_sectors = le32(boot + 84);
	uint32_t heap_sector = le32(boot + 88);
	uint32_t clusters = le32(boot + 92);
	card->root_cluster = le32(boot + 96);
	uint8_t sector_shift = boot[108], cluster_shift = boot[109], fats = boot[110];

	if (sector_shift < 9 || sector_shift > 12 || cluster_shift > 25 - sector_shift) {
		return damaged(card, "the first sector does not describe an exFAT volume");
	}
	// Two tables and two bitmaps is the transaction-safe variant, made for
	// other devices than this one and never met on a card.
	if (fats != 1) {
		return failed(card, FATCHECK_UNSUPPORTED, "an exFAT volume with two tables");
	}
	card->sector_bytes = 1u << sector_shift;
	uint64_t cluster_bytes = (uint64_t)card->sector_bytes << cluster_shift;
	if (cluster_bytes > X_MAX_CLUSTER_BYTES) {
		return failed(card, FATCHECK_UNSUPPORTED, "clusters larger than this reads");
	}
	card->cluster_bytes = (uint32_t)cluster_bytes;
	card->fat_count = 1;
	card->fat_sectors = fat_sectors;

	uint64_t heap_end = (uint64_t)heap_sector + ((uint64_t)clusters << cluster_shift);
	if (clusters == 0 || clusters > 0xFFFFFFF5u || fat_sector < 24 || heap_sector < fat_sector + fat_sectors || heap_end > volume_sectors || (uint64_t)fat_sectors * card->sector_bytes / 4 < (uint64_t)clusters + 2) {
		return damaged(card, "the first sector's figures do not add up");
	}
	card->clusters = clusters;
	card->fat_start = (off_t)fat_sector * card->sector_bytes;
	card->data_start = (off_t)heap_sector * card->sector_bytes;

	off_t device_bytes = lseek(card->fd, 0, SEEK_END);
	if (device_bytes < 0 || (uint64_t)device_bytes < volume_sectors * card->sector_bytes) {
		return damaged(card, "the volume is larger than the card");
	}
	if (!valid_cluster(card, card->root_cluster)) {
		return damaged(card, "the root folder is nowhere");
	}
	return true;
}

// The whole check of an exFAT card, and the repair when it was asked for and
// is allowed. Leaves the status and the counts in card->out.
static bool x_check(card_t *card, const uint8_t *boot, bool repair, long expect_files, long expect_folders) {
	fatcheck_result_t *out = card->out;
	xcard_t x = {.card = card};
	uint8_t *bitmap = NULL;

	bool ok = x_read_layout(card, boot) && open_table(card);
	if (ok) {
		card->owned = calloc(((size_t)card->clusters + 2 + 7) / 8, 1);
		card->cluster = malloc(card->cluster_bytes < X_BLOCK_BYTES ? card->cluster_bytes : X_BLOCK_BYTES);
		if (!card->owned || !card->cluster) {
			ok = failed(card, FATCHECK_NO_MEMORY, "no memory for the map of clusters");
		}
	}

	// The root folder has no entry of its own to say how long it is: it is
	// as long as its chain. The size here only has to be more than that.
	if (ok) {
		x_extent_t root = {card->root_cluster, (uint64_t)card->clusters * card->cluster_bytes, false};
		ok = x_read_folder(&x, &root, true);
	}
	while (ok && x.folder_count > 0) {
		x_extent_t folder = x.folders[--x.folder_count];
		ok = x_read_folder(&x, &folder, false);
	}
	if (ok && !x.bitmap_seen) {
		ok = damaged(card, "the root folder names no bitmap");
	}
	if (ok) {
		bitmap = x_read_bitmap(&x);
		ok = bitmap != NULL;
	}

	// The two books against each other. A cluster a file uses that the bitmap
	// calls free is the dangerous way round: the next file written would be
	// given it. That is damage, and nothing is freed beside it.
	uint32_t lost = 0;
	if (ok) {
		for (uint32_t c = 2; ok && c < card->clusters + 2; c++) {
			bool taken = bitmap[(c - 2) >> 3] & (1u << ((c - 2) & 7));
			bool owned = is_owned(card, c);
			if (owned && !taken) {
				ok = damaged(card, "cluster %u holds a file and is marked free", (unsigned)c);
			} else if (taken && !owned && x_fat_entry(card, c) != X_ENTRY_BAD) {
				lost++;
			}
		}
	}

	if (ok) {
		out->lost_clusters = lost;
		out->lost_bytes = (uint64_t)lost * card->cluster_bytes;
		out->status = lost ? FATCHECK_LOST : FATCHECK_CLEAN;

		bool counted = expect_files >= 0 && expect_folders >= 0;
		if (lost && repair && counted && ((uint32_t)expect_files != out->files || (uint32_t)expect_folders != out->folders)) {
			out->status = FATCHECK_MISMATCH;
			snprintf(out->problem, sizeof(out->problem), "read %u files in %u folders where the card showed %ld in %ld", (unsigned)out->files, (unsigned)out->folders, expect_files, expect_folders);
		} else if (lost && repair) {
			// The bits are cleared in the copy, and each cluster of the
			// bitmap that changed is written back whole, as it was read.
			for (uint32_t k = 0; ok && k < x.bitmap_cluster_count; k++) {
				bool changed = false;
				uint64_t first_bit = (uint64_t)k * card->cluster_bytes * 8;
				for (uint64_t bit = first_bit; bit < first_bit + (uint64_t)card->cluster_bytes * 8 && bit < card->clusters; bit++) {
					uint32_t c = (uint32_t)bit + 2;
					bool taken = bitmap[bit >> 3] & (1u << (bit & 7));
					if (taken && !is_owned(card, c) && x_fat_entry(card, c) != X_ENTRY_BAD) {
						bitmap[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
						changed = true;
					}
				}
				if (changed) {
					off_t offset = card->data_start + (off_t)(x.bitmap_clusters[k] - 2) * card->cluster_bytes;
					if (!write_at(card->fd, bitmap + (size_t)k * card->cluster_bytes, card->cluster_bytes, offset)) {
						ok = failed(card, FATCHECK_IO, "writing the bitmap failed");
					}
				}
			}
			if (ok && fsync(card->fd) != 0) {
				ok = failed(card, FATCHECK_IO, "writing the bitmap failed");
			}
			if (ok) {
				out->repaired = true;
			}
		}
	}

	free(bitmap);
	free(x.bitmap_clusters);
	free(x.folders);
	return ok;
}

// ---------------------------------------------------------------------------

// The whole check of a FAT32 card, and the repair when it was asked for and
// is allowed.
static bool fat32_check(card_t *card, bool repair, long expect_files, long expect_folders) {
	fatcheck_result_t *out = card->out;
	bool ok = read_layout(card) && open_table(card);
	if (ok) {
		card->owned = calloc(((size_t)card->clusters + 2 + 7) / 8, 1);
		card->cluster = malloc(card->cluster_bytes);
		if (!card->owned || !card->cluster) {
			ok = failed(card, FATCHECK_NO_MEMORY, "no memory for the map of clusters");
		}
	}
	// The status is only a result once the whole tree has been read without
	// finding fault: until then it is whatever stopped it.
	ok = ok && tables_agree(card) && read_tree(card);
	if (!ok) {
		return false;
	}

	uint32_t lost = 0, free_clusters = 0;
	find_lost(card, &lost, &free_clusters);
	out->lost_clusters = lost;
	out->lost_bytes = (uint64_t)lost * card->cluster_bytes;
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
			close_table(card);
			ok = free_lost(card, free_clusters + lost);
			if (ok) {
				out->repaired = true;
			}
		}
	}
	return ok;
}

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

	// exFAT names itself at the start of its first sector; everything else
	// is for the FAT32 reader to take or to turn away.
	uint8_t boot[512];
	bool ok;
	if (!read_at(card.fd, boot, sizeof(boot), 0)) {
		ok = failed(&card, FATCHECK_IO, "the first sector cannot be read");
	} else if (boot[510] == 0x55 && boot[511] == 0xAA && memcmp(boot + 3, "EXFAT   ", 8) == 0) {
		ok = x_check(&card, boot, repair, expect_files, expect_folders);
	} else {
		ok = fat32_check(&card, repair, expect_files, expect_folders);
	}

	close_table(&card);
	free(card.owned);
	free(card.cluster);
	free(card.folders_todo);
	close(card.fd);
	// A card that was read through and found damaged is a result too.
	return ok || out->status == FATCHECK_DAMAGED || out->status == FATCHECK_UNSUPPORTED;
}
