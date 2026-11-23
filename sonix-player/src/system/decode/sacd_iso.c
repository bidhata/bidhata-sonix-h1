#include "sacd_iso.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dst_decoder.h"

// The ScarletBook side of an SACD: the master TOC at LSN 510, one area TOC per
// sound area, the track lists behind it, and the audio itself. All multi-byte
// on-disc fields are big-endian; everything is addressed in 2048-byte sectors.
//
// The audio is handed to the caller one frame at a time. A 75 fps frame is 588
// 44.1 kHz samples, and at DSD64 each sample is 64 bits, so one frame is 4704
// bytes per channel (9408 interleaved for stereo).

#define SECTOR_SIZE 2048
#define BYTES_PER_FRAME 4704
#define FRAME_RATE 75
#define SACD_RATE 2822400u
#define MAX_CHANNELS 6
#define MASTER_LSN 510
#define MASTER_SECTORS 10
#define MAX_TOC_SECTORS 96
#define MAX_ACC_ENTRIES 6550
#define MAX_FRAME_SECTORS 255
#define TEXT_TABLE_SIZE 4096 // the track-text pointer table owns this much

typedef struct {
	int offset;
	int length;
} dsd_read_t;

typedef struct {
	int sector_count;
	int sector_mul;
	int sector_addition;
	dsd_read_t reads[6];
} dsd_layout_t;

// The fixed layouts of plain (uncompressed) DSD. Three frames share a block of
// 14 or 16 sectors, each frame taking a different slice of every sector. The
// offsets already include the sector header (32 / 284 bytes), because an ISO
// has no outer header of its own. Each row sums to 9408 bytes, one stereo
// frame. Matches the tables in libsacd's sacd_dsd_reader.c.
static const dsd_layout_t layout_3in14[3] = {
	{5, 14, 0,
	 {{32, 2016}, {32, 2016}, {32, 2016}, {32, 2016}, {32, 1344}, {0, 0}}},
	{6, 14, 4,
	 {{32 + 1344, 672}, {32, 2016}, {32, 2016}, {32, 2016}, {32, 2016},
	  {32, 672}}},
	{5, 14, 9,
	 {{32 + 672, 1344}, {32, 2016}, {32, 2016}, {32, 2016}, {32, 2016},
	  {0, 0}}},
};

static const dsd_layout_t layout_3in16[3] = {
	{6, 16, 0,
	 {{284, 1764}, {284, 1764}, {284, 1764}, {284, 1764}, {284, 1764},
	  {284, 588}}},
	{6, 16, 5,
	 {{284 + 588, 1176}, {284, 1764}, {284, 1764}, {284, 1764}, {284, 1764},
	  {284, 1176}}},
	{6, 16, 10,
	 {{284 + 1176, 588}, {284, 1764}, {284, 1764}, {284, 1764}, {284, 1764},
	  {284, 1764}}},
};

typedef struct {
	int count;
	int step;
	uint32_t *lsn;
} sacd_acc_t;

struct sacd_iso {
	int fd;
	uint64_t file_sectors;

	int area_count;
	sacd_area_t areas[SACD_MAX_AREAS];
	sacd_acc_t acc[SACD_MAX_AREAS];

	char title[256];
	char artist[256];
	char genre[64];
	int year;
	int charset;

	// Read position: an absolute area frame, a byte inside it, and the frame
	// decoded for it.
	int sel_area;
	int sel_track;
	int frame_channels;
	uint32_t frame_index;
	uint32_t frame_pos;
	uint64_t track_bytes;
	bool frame_loaded;

	unsigned char *sector;
	unsigned char *frame;
	unsigned char *raw;
	size_t raw_cap;
	dst_decoder_t *dst;
	int dst_channels;

	// Where the next sequential DST frame starts, when the current frame's
	// last sector said so.
	bool next_valid;
	uint32_t next_frame;
	uint32_t next_lsn;
	int next_sectors;
};

// ---------------------------------------------------------------------------
// bytes and text
// ---------------------------------------------------------------------------

static uint16_t be16(const unsigned char *p) {
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be24(const unsigned char *p) {
	return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static uint32_t be32(const unsigned char *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

// Charset 1 is ASCII, 2 is ISO-8859-1, which needs expanding to UTF-8 above
// 0x7f. Anything else is copied with the control bytes dropped.
static void convert_text(char *out, size_t size, const unsigned char *src,
			 size_t len, int charset) {
	size_t o = 0;

	if (size == 0) {
		return;
	}
	for (size_t i = 0; i < len && src[i] != '\0'; i++) {
		unsigned char c = src[i];
		if (charset == 2 && c >= 0x80) {
			if (o + 2 >= size) {
				break;
			}
			out[o++] = (char)(0xc0 | (c >> 6));
			out[o++] = (char)(0x80 | (c & 0x3f));
		} else if (charset != 1 && charset != 2 &&
			   (c < 0x20 || c == 0x7f)) {
			continue;
		} else {
			if (o + 1 >= size) {
				break;
			}
			out[o++] = (char)c;
		}
	}
	out[o] = '\0';
}

static void copy_bounded(char *out, size_t size, const char *in) {
	if (size == 0) {
		return;
	}
	if (!in) {
		out[0] = '\0';
		return;
	}
	size_t len = strlen(in);
	if (len >= size) {
		len = size - 1;
	}
	memcpy(out, in, len);
	out[len] = '\0';
}

// Album genres as the standard table numbers them; index 5 is Classical.
static const char *const genre_general[] = {
	"Reserved",
	"Not defined",
	"Adult Contemporary",
	"Alternative Rock",
	"Children's Music",
	"Classical",
	"Contemporary Christian",
	"Country",
	"Dance",
	"Easy Listening",
	"Erotic",
	"Folk",
	"Gospel",
	"Hip Hop",
	"Jazz",
	"Latin",
	"Musical",
	"New Age",
	"Opera",
	"Operetta",
	"Pop Music",
	"Rap",
	"Reggae",
	"Rock Music",
	"Rhythm & Blues",
	"Sound Effects",
	"Sound Track",
	"Spoken Word",
	"World Music",
	"Blues",
};

#define GENRE_COUNT (int)(sizeof(genre_general) / sizeof(genre_general[0]))

static const char *genre_name(int table, int index) {
	if (table == 2) { // the Japanese table; not carried here
		return "";
	}
	if (index < 0 || index >= GENRE_COUNT) {
		return "";
	}
	return genre_general[index];
}

// ---------------------------------------------------------------------------
// sectors
// ---------------------------------------------------------------------------

static bool read_at(int fd, uint64_t off, void *buf, size_t len) {
	size_t done = 0;

	while (done < len) {
		ssize_t n = pread(fd, (char *)buf + done, len - done,
				  (off_t)(off + done));
		if (n <= 0) {
			return false;
		}
		done += (size_t)n;
	}
	return true;
}

static bool read_sector(const sacd_iso_t *s, uint32_t lsn,
			unsigned char *buf) {
	if (s->fd < 0 || (uint64_t)lsn >= s->file_sectors) {
		return false;
	}
	return read_at(s->fd, (uint64_t)lsn * SECTOR_SIZE, buf, SECTOR_SIZE);
}

// ---------------------------------------------------------------------------
// track text
// ---------------------------------------------------------------------------

// One track's item: a count, three reserved bytes, then count entries of
// {type, 0x20, NUL-terminated text} each padded to four bytes.
static void parse_text_item(const unsigned char *p, size_t len, int charset,
			    sacd_track_t *track) {
	const unsigned char *end;

	if (len < 4) {
		return;
	}
	int count = p[0];
	const unsigned char *q = p + 4;
	end = p + len;

	for (int i = 0; i < count; i++) {
		if (q + 2 > end) {
			break;
		}
		unsigned char type = q[0];
		q += 2; // type, then the 0x20 padding byte

		const unsigned char *str = q;
		while (q < end && *q != '\0') {
			q++;
		}
		size_t slen = (size_t)(q - str);
		if (q < end) {
			q++;
		}
		size_t used = (size_t)(q - p);
		size_t pad = (4 - (used & 3)) & 3;
		q += pad;
		if (q > end) {
			q = end;
		}

		if (type == 0x01) {
			convert_text(track->title, sizeof(track->title), str, slen,
				     charset);
		} else if (type == 0x02) {
			convert_text(track->performer, sizeof(track->performer),
				     str, slen, charset);
		}
	}
}

// ---------------------------------------------------------------------------
// area TOC
// ---------------------------------------------------------------------------

static bool parse_area(sacd_iso_t *s, uint32_t start, uint32_t toc_size) {
	if (toc_size < 1 || toc_size > MAX_TOC_SECTORS) {
		return false;
	}
	if ((uint64_t)start + toc_size > s->file_sectors) {
		return false;
	}

	size_t blob_len = (size_t)toc_size * SECTOR_SIZE;
	unsigned char *blob = malloc(blob_len);
	if (!blob) {
		return false;
	}
	if (!read_at(s->fd, (uint64_t)start * SECTOR_SIZE, blob, blob_len)) {
		free(blob);
		return false;
	}

	bool two_channel = memcmp(blob, "TWOCHTOC", 8) == 0;
	if (!two_channel && memcmp(blob, "MULCHTOC", 8) != 0) {
		free(blob);
		return false;
	}

	// The declared length counts sector 0 and may not exceed what was read.
	unsigned declared = be16(blob + 10);
	if (declared == 0 || declared > toc_size) {
		declared = toc_size;
	}
	size_t used = (size_t)declared * SECTOR_SIZE;

	int channels = blob[32];
	int format = blob[21] & 0x0f;
	if (channels < 1 || channels > MAX_CHANNELS || two_channel != (channels == 2)) {
		free(blob);
		return false;
	}
	if (format != 0 && format != 2 && format != 3) {
		free(blob);
		return false;
	}

	int track_count = blob[69];
	if (track_count < 1 || track_count > SACD_MAX_TRACKS) {
		free(blob);
		return false;
	}
	if (8 + 2 * track_count > (int)SECTOR_SIZE) {
		free(blob);
		return false;
	}

	uint32_t track_start = be32(blob + 72);
	uint32_t track_end = be32(blob + 76);
	if (track_end < track_start || track_start >= s->file_sectors ||
	    track_end >= s->file_sectors) {
		free(blob);
		return false;
	}
	uint64_t span = (uint64_t)track_end - track_start + 1;

	// The block positions of the track text and the access list are sector
	// offsets from the first TOC sector. Bounds-check them against the blob.
	unsigned text_off = be16(blob + 128);
	unsigned acc_off = be16(blob + 132);
	// The area's own description and copyright (offsets 144/146) are byte
	// offsets from the TOC start. Neither string is exposed by this reader,
	// so they are read but not consumed.
	unsigned desc_off = be16(blob + 144);
	unsigned copyright_off = be16(blob + 146);
	(void)desc_off;
	(void)copyright_off;

	// The area's own charset and language live in its text channel 0.
	int text_charset = blob[80] > 0 ? blob[90] : s->charset;
	if (text_charset == 0) {
		text_charset = s->charset;
	}

	// Track lists are found by magic rather than by a fixed offset.
	const unsigned char *trl1 = NULL;
	const unsigned char *trl2 = NULL;
	for (unsigned i = 1; i < declared; i++) {
		const unsigned char *p = blob + (size_t)i * SECTOR_SIZE;
		if (!trl1 && memcmp(p, "SACDTRL1", 8) == 0) {
			trl1 = p;
		} else if (!trl2 && memcmp(p, "SACDTRL2", 8) == 0) {
			trl2 = p;
		}
	}
	if (!trl1 || !trl2) {
		free(blob);
		return false;
	}

	// Total playtime is the natural ceiling for frame numbers.
	uint32_t total_frames = (uint32_t)blob[64] * 60 * FRAME_RATE +
				(uint32_t)blob[65] * FRAME_RATE + blob[66];
	uint64_t frame_bound = total_frames;
	if (frame_bound == 0 || frame_bound > span) {
		frame_bound = span; // one frame can never be shorter than a sector
	}

	sacd_area_t *area = &s->areas[s->area_count];
	memset(area, 0, sizeof(*area));
	area->channels = channels;
	area->frame_format = format;
	area->track_start_lsn = track_start;
	area->track_end_lsn = track_end;
	area->track_count = track_count;

	for (int n = 0; n < track_count; n++) {
		sacd_track_t *t = &area->tracks[n];
		const unsigned char *l1 = trl1 + 8 + 4 * (size_t)n;
		const unsigned char *d2 = trl2 + 1028 + 4 * (size_t)n;
		uint32_t frames;

		t->number = n + 1;
		t->start_lsn = be32(l1);
		t->length_lsn = be32(trl1 + 1028 + 4 * (size_t)n);
		t->start_frame = (uint32_t)trl2[8 + 4 * (size_t)n] * 60 * FRAME_RATE +
				 (uint32_t)trl2[9 + 4 * (size_t)n] * FRAME_RATE +
				 trl2[10 + 4 * (size_t)n];
		frames = (uint32_t)d2[0] * 60 * FRAME_RATE +
			 (uint32_t)d2[1] * FRAME_RATE + d2[2];

		// Keep a malformed list from pointing outside the area: a frame
		// cannot start past the area and cannot run past its end.
		if (t->start_lsn < track_start || t->start_lsn > track_end) {
			t->start_lsn = track_start;
		}
		if (t->length_lsn > (uint32_t)(track_end - t->start_lsn + 1)) {
			t->length_lsn = track_end - t->start_lsn + 1;
		}
		if (t->start_frame > frame_bound) {
			t->start_frame = (uint32_t)frame_bound;
			frames = 0;
		} else if (frames > frame_bound - t->start_frame) {
			frames = (uint32_t)(frame_bound - t->start_frame);
		}
		t->frames = frames;
	}

	// Track titles and performers.
	if (text_off >= 1 && text_off < declared) {
		const unsigned char *block = blob + (size_t)text_off * SECTOR_SIZE;
		size_t block_len = used - (size_t)text_off * SECTOR_SIZE;
		if (block_len >= 8 && memcmp(block, "SACDTTxt", 8) == 0) {
			for (int n = 0; n < track_count; n++) {
				size_t p = 8 + 2 * (size_t)n;
				if (p + 2 > block_len) {
					break;
				}
				unsigned pos = be16(block + p);
				// The pointer table owns the first 4096 bytes.
				if (pos < TEXT_TABLE_SIZE || pos >= block_len) {
					continue;
				}
				parse_text_item(block + pos, block_len - pos,
						text_charset, &area->tracks[n]);
			}
		}
	}

	// Access list, only present (and only needed) for DST.
	sacd_acc_t acc = {0};
	if (acc_off >= 1 && acc_off < declared) {
		const unsigned char *block = blob + (size_t)acc_off * SECTOR_SIZE;
		size_t block_len = used - (size_t)acc_off * SECTOR_SIZE;
		if (block_len >= 16 && memcmp(block, "SACD_ACC", 8) == 0) {
			int count = be16(block + 8);
			int step = block[10];
			if (count > MAX_ACC_ENTRIES) {
				count = MAX_ACC_ENTRIES;
			}
			if ((size_t)count * 5 + 16 > block_len) {
				count = (int)((block_len - 16) / 5);
			}
			if (step < 1) {
				step = 1;
			}
			if (count > 0) {
				acc.lsn = malloc((size_t)count * sizeof(uint32_t));
				if (!acc.lsn) {
					free(blob);
					return false;
				}
				for (int i = 0; i < count; i++) {
					// Flags are the first two bytes; the LSN is the
					// last three.
					acc.lsn[i] = be24(block + 16 + 5 * (size_t)i + 2);
				}
				acc.count = count;
				acc.step = step;
			}
		}
	}

	s->acc[s->area_count] = acc;
	s->area_count++;
	free(blob);
	return true;
}

// ---------------------------------------------------------------------------
// open and close
// ---------------------------------------------------------------------------

sacd_iso_t *sacd_iso_open(const char *path) {
	if (!path) {
		return NULL;
	}

	// A virtual track path carries the image's name before the suffix.
	char base[1024];
	sacd_split_path(path, base, sizeof(base));
	const char *real = base[0] ? base : path;

	int fd = open(real, O_RDONLY);
	if (fd < 0) {
		return NULL;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return NULL;
	}
	uint64_t size = (uint64_t)st.st_size;
	if (size < (uint64_t)(MASTER_LSN + MASTER_SECTORS) * SECTOR_SIZE) {
		close(fd);
		return NULL;
	}

	sacd_iso_t *s = calloc(1, sizeof(*s));
	if (!s) {
		close(fd);
		return NULL;
	}
	s->fd = fd;
	s->file_sectors = size / SECTOR_SIZE;
	s->sel_area = -1;
	s->sel_track = -1;

	unsigned char *master = malloc(MASTER_SECTORS * SECTOR_SIZE);
	s->sector = malloc(SECTOR_SIZE);
	if (!master || !s->sector) {
		free(master);
		sacd_iso_close(s);
		return NULL;
	}
	if (!read_at(fd, (uint64_t)MASTER_LSN * SECTOR_SIZE, master,
		     MASTER_SECTORS * SECTOR_SIZE) ||
	    memcmp(master, "SACDMTOC", 8) != 0) {
		free(master);
		sacd_iso_close(s);
		return NULL;
	}

	s->charset = master[138];
	s->year = be16(master + 120);
	int genre_table = master[40];
	int genre_index = be16(master + 42);
	copy_bounded(s->genre, sizeof(s->genre),
		     genre_name(genre_table, genre_index));

	// The album title and artist live in the first master text sector, at
	// byte offsets 16 (title) and 18 (artist) into its pointer table.
	unsigned char text[SECTOR_SIZE];
	if (read_at(fd, (uint64_t)(MASTER_LSN + 1) * SECTOR_SIZE, text,
		    sizeof(text)) &&
	    memcmp(text, "SACDText", 8) == 0) {
		unsigned pos = be16(text + 16);
		if (pos < SECTOR_SIZE) {
			convert_text(s->title, sizeof(s->title), text + pos,
				     SECTOR_SIZE - pos, s->charset);
		}
		pos = be16(text + 18);
		if (pos < SECTOR_SIZE) {
			convert_text(s->artist, sizeof(s->artist), text + pos,
				     SECTOR_SIZE - pos, s->charset);
		}
	}

	// Area 1 is the two-channel TOC, area 2 the multichannel one. Each has a
	// second TOC to fall back on when the first cannot be read.
	uint32_t starts[SACD_MAX_AREAS][2];
	unsigned sizes[SACD_MAX_AREAS];
	starts[0][0] = be32(master + 64);
	starts[0][1] = be32(master + 68);
	sizes[0] = be16(master + 84);
	starts[1][0] = be32(master + 72);
	starts[1][1] = be32(master + 76);
	sizes[1] = be16(master + 86);
	free(master);

	for (int i = 0; i < SACD_MAX_AREAS && i < 2; i++) {
		if (s->area_count >= SACD_MAX_AREAS) {
			break;
		}
		if (sizes[i] == 0 ||
		    (starts[i][0] == 0 && starts[i][1] == 0)) {
			continue;
		}
		if (starts[i][0] != 0 && parse_area(s, starts[i][0], sizes[i])) {
			continue;
		}
		if (starts[i][1] != 0) {
			parse_area(s, starts[i][1], sizes[i]);
		}
	}

	if (s->area_count == 0) {
		sacd_iso_close(s);
		return NULL;
	}
	return s;
}

void sacd_iso_close(sacd_iso_t *s) {
	if (!s) {
		return;
	}
	for (int i = 0; i < SACD_MAX_AREAS; i++) {
		free(s->acc[i].lsn);
	}
	dst_decoder_close(s->dst);
	free(s->sector);
	free(s->frame);
	free(s->raw);
	if (s->fd >= 0) {
		close(s->fd);
	}
	free(s);
}

// ---------------------------------------------------------------------------
// metadata
// ---------------------------------------------------------------------------

int sacd_iso_area_count(const sacd_iso_t *s) {
	return s ? s->area_count : 0;
}

const sacd_area_t *sacd_iso_area(const sacd_iso_t *s, int area) {
	if (!s || area < 0 || area >= s->area_count) {
		return NULL;
	}
	return &s->areas[area];
}

int sacd_iso_stereo_area(const sacd_iso_t *s) {
	if (!s) {
		return -1;
	}
	for (int i = 0; i < s->area_count; i++) {
		if (s->areas[i].channels == 2) {
			return i;
		}
	}
	return -1;
}

const char *sacd_iso_album_title(const sacd_iso_t *s) {
	return s ? s->title : "";
}

const char *sacd_iso_album_artist(const sacd_iso_t *s) {
	return s ? s->artist : "";
}

const char *sacd_iso_genre(const sacd_iso_t *s) {
	return s ? s->genre : "";
}

int sacd_iso_year(const sacd_iso_t *s) {
	return s ? s->year : 0;
}

// ---------------------------------------------------------------------------
// DST frames
// ---------------------------------------------------------------------------

// Scans sector by sector for the sector that starts `frame`: its first audio
// packet marked frame_start, whose frame info timecode is the frame wanted.
static bool scan_frame(sacd_iso_t *s, uint32_t from, uint32_t to,
		       uint32_t frame, uint32_t *lsn, int *sectors) {
	if (to < from || from >= s->file_sectors) {
		return false;
	}
	if ((uint64_t)to >= s->file_sectors) {
		to = (uint32_t)(s->file_sectors - 1);
	}
	for (uint32_t cur = from; cur <= to; cur++) {
		if (!read_sector(s, cur, s->sector)) {
			return false;
		}
		unsigned b = s->sector[0];
		int frame_infos = (b >> 2) & 7;
		int packets = (b >> 5) & 7;
		size_t packet_base = 1 + 2 * (size_t)packets;
		if (packet_base + 4 * (size_t)frame_infos > SECTOR_SIZE) {
			continue;
		}
		bool starts = false;
		for (int i = 0; i < packets; i++) {
			unsigned w = ((unsigned)s->sector[1 + 2 * (size_t)i] << 8) |
				     s->sector[2 + 2 * (size_t)i];
			if ((w >> 15) && ((w >> 11) & 7) == 2) {
				starts = true;
			}
		}
		if (!starts) {
			continue;
		}
		for (int j = 0; j < frame_infos; j++) {
			const unsigned char *f = s->sector + packet_base + 4 * (size_t)j;
			uint32_t tc = (uint32_t)f[0] * 60 * FRAME_RATE +
				      (uint32_t)f[1] * FRAME_RATE + f[2];
			if (tc == frame) {
				*lsn = cur;
				*sectors = (f[3] >> 2) & 0x1f;
				return true;
			}
		}
		if (cur == to) {
			break;
		}
	}
	return false;
}

static bool find_frame(sacd_iso_t *s, uint32_t frame, uint32_t *lsn,
		       int *sectors) {
	sacd_area_t *area = &s->areas[s->sel_area];
	sacd_acc_t *acc = &s->acc[s->sel_area];

	if (acc->count > 0) {
		uint32_t step = acc->step > 0 ? (uint32_t)acc->step : 1;
		int idx = (int)(frame / step);
		if (idx >= acc->count) {
			idx = acc->count - 1;
		}
		uint32_t from = acc->lsn[idx];
		uint32_t to = idx + 1 < acc->count ? acc->lsn[idx + 1]
						   : area->track_end_lsn;
		if (from < area->track_start_lsn) {
			from = area->track_start_lsn;
		}
		if (from > area->track_end_lsn) {
			from = area->track_start_lsn;
		}
		if (to > area->track_end_lsn) {
			to = area->track_end_lsn;
		}
		if (to >= from && scan_frame(s, from, to, frame, lsn, sectors)) {
			return true;
		}
	}
	// No access list, or the narrowed range did not hold it: the whole area.
	return scan_frame(s, area->track_start_lsn, area->track_end_lsn, frame,
			  lsn, sectors);
}

// Copies one frame out of the sectors that hold it. `lsn` may be a sector
// shared with the previous frame, so packets before the matching frame_start
// are skipped. Returns false when the frame does not start at that LSN.
static bool assemble_dst(sacd_iso_t *s, uint32_t frame, uint32_t lsn,
			 int hint_sectors, size_t frame_bytes) {
	int sectors = hint_sectors > 0 ? hint_sectors : 1;
	bool started = false;
	bool have_span = false;
	bool next_here = false;
	size_t total = 0;
	uint32_t cur = lsn;
	int used = 0;

	while (used < sectors) {
		if (!read_sector(s, cur, s->sector)) {
			break;
		}
		unsigned b = s->sector[0];
		int frame_infos = (b >> 2) & 7;
		int packets = (b >> 5) & 7;
		size_t packet_base = 1 + 2 * (size_t)packets;
		size_t off = packet_base + 4 * (size_t)frame_infos;
		if (off > SECTOR_SIZE) {
			break;
		}
		size_t data = off;
		int info_index = 0; // frame infos are indexed per sector

		for (int i = 0; i < packets; i++) {
			size_t pi = 1 + 2 * (size_t)i;
			if (pi + 2 > SECTOR_SIZE) {
				break;
			}
			unsigned w = ((unsigned)s->sector[pi] << 8) |
				     s->sector[pi + 1];
			int starts = (int)(w >> 15);
			int type = (int)((w >> 11) & 7);
			size_t len = w & 0x7ff;

			if (starts && type == 2) {
				const unsigned char *f = NULL;
				uint32_t tc = 0;
				int span = 0;

				if (info_index < frame_infos) {
					f = s->sector + packet_base +
					    4 * (size_t)info_index;
					tc = (uint32_t)f[0] * 60 * FRAME_RATE +
					     (uint32_t)f[1] * FRAME_RATE + f[2];
					span = (f[3] >> 2) & 0x1f;
					info_index++;
				}
				if (!started) {
					// Only this frame's own start begins the
					// payload; another frame's start is skipped.
					if (!f || tc != frame) {
						data += len;
						if (data > SECTOR_SIZE) {
							data = SECTOR_SIZE;
						}
						continue;
					}
					started = true;
					if (!have_span) {
						sectors = span > 0 ? span : 1;
						have_span = true;
					}
				} else {
					// The target is under way and another frame
					// starts: remember where, and stop. The next
					// read skips to it by its timecode.
					s->next_valid = true;
					s->next_frame = f ? tc : frame + 1;
					s->next_lsn = cur;
					s->next_sectors = span;
					next_here = true;
					break;
				}
			}

			if (started && type == 2 && total < s->raw_cap) {
				size_t n = len;
				if (n > SECTOR_SIZE - data) {
					n = SECTOR_SIZE - data;
				}
				if (n > s->raw_cap - total) {
					n = s->raw_cap - total;
				}
				memcpy(s->raw + total, s->sector + data, n);
				total += n;
			}
			data += len;
			if (data > SECTOR_SIZE) {
				data = SECTOR_SIZE;
			}
		}

		used++;
		cur++;
		if (next_here) {
			break;
		}
	}

	if (!started) {
		return false;
	}
	if (!next_here && used >= sectors) {
		// No later frame_start in range: the next frame starts at the next
		// sector. Its own frame info will give its true span.
		s->next_valid = true;
		s->next_frame = frame + 1;
		s->next_lsn = cur;
		s->next_sectors = 0;
	}
	if (total == 0 || !s->dst) {
		return true;
	}
	int out_len = 0;
	if (dst_decoder_decode(s->dst, s->raw, (int)total, s->frame, &out_len) != 0 ||
	    out_len != (int)frame_bytes) {
		memset(s->frame, 0, frame_bytes);
	}
	return true;
}

// Locates and assembles one DST frame. A malformed frame becomes silence.
static void dst_frame(sacd_iso_t *s, uint32_t frame, size_t frame_bytes) {
	uint32_t lsn;
	int sectors = 0;
	bool have = false;

	if (s->next_valid && s->next_frame == frame) {
		lsn = s->next_lsn;
		sectors = s->next_sectors;
		have = true;
	}
	s->next_valid = false;

	if (have && assemble_dst(s, frame, lsn, sectors, frame_bytes)) {
		return;
	}
	if (find_frame(s, frame, &lsn, &sectors)) {
		assemble_dst(s, frame, lsn, sectors, frame_bytes);
	}
}

// ---------------------------------------------------------------------------
// reading
// ---------------------------------------------------------------------------

static void fill_frame(sacd_iso_t *s, uint32_t frame) {
	sacd_area_t *area = &s->areas[s->sel_area];
	size_t frame_bytes = (size_t)BYTES_PER_FRAME * (size_t)area->channels;

	memset(s->frame, 0, frame_bytes);

	if (area->frame_format == 0) {
		dst_frame(s, frame, frame_bytes);
		return;
	}

	const dsd_layout_t *table =
		area->frame_format == 2 ? layout_3in14 : layout_3in16;
	uint32_t block = frame / 3;
	uint32_t slot = frame % 3;
	const dsd_layout_t *l = &table[slot];
	uint32_t first = area->track_start_lsn +
			 block * (uint32_t)l->sector_mul +
			 (uint32_t)l->sector_addition;
	size_t done = 0;

	for (int i = 0; i < 6 && l->reads[i].length > 0; i++) {
		if (!read_sector(s, first + (uint32_t)i, s->sector)) {
			break;
		}
		size_t n = (size_t)l->reads[i].length;
		if (n > frame_bytes - done) {
			n = frame_bytes - done;
		}
		memcpy(s->frame + done, s->sector + l->reads[i].offset, n);
		done += n;
		if (done >= frame_bytes) {
			break;
		}
	}
}

bool sacd_iso_select(sacd_iso_t *s, int area, int track) {
	if (!s || area < 0 || area >= s->area_count) {
		return false;
	}
	sacd_area_t *a = &s->areas[area];
	if (track < 1 || track > a->track_count) {
		return false;
	}
	// v1 is stereo-only: the fixed layouts describe two-channel interleaving
	// and the output stage is two channels, so a multichannel area is refused
	// rather than handed to it.
	if (a->channels != 2) {
		return false;
	}

	if (!s->frame) {
		s->frame = malloc((size_t)BYTES_PER_FRAME * MAX_CHANNELS);
		if (!s->frame) {
			return false;
		}
	}

	if (a->frame_format == 0) {
		if (!s->raw) {
			s->raw = malloc((size_t)MAX_FRAME_SECTORS * SECTOR_SIZE);
			if (!s->raw) {
				return false;
			}
			s->raw_cap = (size_t)MAX_FRAME_SECTORS * SECTOR_SIZE;
		}
		if (s->dst && s->dst_channels != a->channels) {
			dst_decoder_close(s->dst);
			s->dst = NULL;
		}
		if (!s->dst) {
			if (dst_decoder_init(&s->dst, a->channels,
					     (int)SACD_RATE) != 0) {
				s->dst = NULL;
				return false;
			}
			s->dst_channels = a->channels;
		}
	}

	sacd_track_t *t = &a->tracks[track - 1];
	s->sel_area = area;
	s->sel_track = track;
	s->frame_channels = a->channels;
	s->frame_index = t->start_frame;
	s->frame_pos = 0;
	s->frame_loaded = false;
	s->track_bytes = (uint64_t)t->frames * BYTES_PER_FRAME;
	s->next_valid = false;
	return true;
}

int sacd_iso_channels(const sacd_iso_t *s) {
	if (!s || s->sel_area < 0) {
		return 0;
	}
	return s->frame_channels;
}

int sacd_iso_multiple(const sacd_iso_t *s) {
	(void)s;
	return 64;
}

uint32_t sacd_iso_rate(const sacd_iso_t *s) {
	(void)s;
	return SACD_RATE;
}

uint64_t sacd_iso_track_bytes(const sacd_iso_t *s) {
	if (!s || s->sel_area < 0) {
		return 0;
	}
	return s->track_bytes;
}

uint64_t sacd_iso_read(sacd_iso_t *s, unsigned char *const *chan,
		       uint64_t bytes) {
	if (!s || !chan || s->sel_area < 0 || s->track_bytes == 0) {
		return 0;
	}
	sacd_track_t *t = &s->areas[s->sel_area].tracks[s->sel_track - 1];
	uint64_t end_frame = (uint64_t)t->start_frame + t->frames;
	size_t channels = (size_t)s->frame_channels;
	uint64_t done = 0;

	while (done < bytes && s->frame_index < end_frame) {
		if (!s->frame_loaded) {
			fill_frame(s, s->frame_index);
			s->frame_loaded = true;
		}
		uint64_t n = BYTES_PER_FRAME - s->frame_pos;
		if (n > bytes - done) {
			n = bytes - done;
		}
		for (size_t c = 0; c < channels; c++) {
			unsigned char *out = chan[c];
			if (!out) {
				continue;
			}
			for (uint64_t k = 0; k < n; k++) {
				out[done + k] =
					s->frame[(s->frame_pos + k) * channels + c];
			}
		}
		done += n;
		s->frame_pos += (uint32_t)n;
		if (s->frame_pos == BYTES_PER_FRAME) {
			s->frame_pos = 0;
			s->frame_index++;
			s->frame_loaded = false;
		}
	}
	return done;
}

bool sacd_iso_seek(sacd_iso_t *s, uint64_t byte) {
	if (!s || s->sel_area < 0) {
		return false;
	}
	sacd_track_t *t = &s->areas[s->sel_area].tracks[s->sel_track - 1];

	if (byte > s->track_bytes) {
		byte = s->track_bytes;
	}
	s->frame_index = t->start_frame + (uint32_t)(byte / BYTES_PER_FRAME);
	s->frame_pos = (uint32_t)(byte % BYTES_PER_FRAME);
	s->frame_loaded = false;
	s->next_valid = false;
	return true;
}

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------

bool sacd_is_iso(const char *path) {
	if (!path) {
		return false;
	}
	char base[1024];
	sacd_split_path(path, base, sizeof(base));
	const char *name = base[0] ? base : path;
	size_t len = strlen(name);
	return len > 4 && strcasecmp(name + len - 4, ".iso") == 0;
}

void sacd_virtual_path(const char *iso_path, int track_number, char *out,
		       size_t size) {
	if (!out || size == 0) {
		return;
	}
	if (!iso_path || track_number < 1) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%s?sacd=%d", iso_path, track_number);
}

int sacd_split_path(const char *path, char *iso_out, size_t size) {
	if (iso_out && size > 0) {
		iso_out[0] = '\0';
	}
	if (!path) {
		return 0;
	}

	// A real file name may contain '?', so only a final "?sacd=<digits>"
	// counts. This is deliberately not CUE's "?track=".
	const char *mark = strrchr(path, '?');
	int number = 0;
	if (mark && strncmp(mark, "?sacd=", 6) == 0 &&
	    isdigit((unsigned char)mark[6])) {
		char *end = NULL;
		long value = strtol(mark + 6, &end, 10);
		if (end && *end == '\0' && value >= 1 && value <= SACD_MAX_TRACKS) {
			number = (int)value;
		}
	}

	if (iso_out && size > 0) {
		if (number > 0) {
			size_t len = (size_t)(mark - path);
			if (len >= size) {
				len = size - 1;
			}
			memcpy(iso_out, path, len);
			iso_out[len] = '\0';
		} else {
			copy_bounded(iso_out, size, path);
		}
	}
	return number;
}

bool sacd_order_tracks(const char *a, const char *b, int *out) {
	if (!a || !b || !out) {
		return false;
	}
	char iso_a[1024], iso_b[1024];
	int ta = sacd_split_path(a, iso_a, sizeof(iso_a));
	int tb = sacd_split_path(b, iso_b, sizeof(iso_b));
	if (ta <= 0 || tb <= 0 || strcmp(iso_a, iso_b) != 0) {
		return false;
	}
	*out = ta - tb;
	return true;
}
