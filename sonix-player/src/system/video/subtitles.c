#include "subtitles.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// A two-hour film has between one and two thousand cues and a file of about a
// hundred kilobytes. These are only there to stop a file that is something
// else from being read into a player with 64 MB of memory.
#define MAX_FILE_BYTES (2 * 1024 * 1024)
#define MAX_CUES 20000
// What is shown of one cue. Longer than this does not fit on the panel in any
// case, and a cue that long is a damaged file more often than a speech.
#define MAX_CUE_BYTES 240

typedef struct {
	int64_t start_ms, end_ms;
	uint32_t text; // where its text starts in `texts`
} cue_t;

static cue_t *cues;
static int cue_count;
static char *texts; // every cue's text, one after the other, each ended by a NUL
static size_t texts_used;

void subtitles_unload(void) {
	free(cues);
	free(texts);
	cues = NULL;
	texts = NULL;
	cue_count = 0;
	texts_used = 0;
}

// ---------------------------------------------------------------------------
// finding the file
// ---------------------------------------------------------------------------

static bool readable(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	fclose(f);
	return true;
}

// Fills `out` with the subtitle file that goes with the film. See the header
// for the two places looked in.
static bool find_file(const char *film_path, char *out, size_t out_size) {
	const char *slash = strrchr(film_path, '/');
	const char *name = slash ? slash + 1 : film_path;
	const char *dot = strrchr(name, '.');
	size_t stem_len = dot ? (size_t)(dot - name) : strlen(name);
	size_t dir_len = slash ? (size_t)(slash - film_path) : 0;
	if (stem_len == 0 || stem_len > 255) {
		return false;
	}

	// The plain name first, in the two ways it gets written. The card is FAT,
	// which does not tell them apart; the simulator's folder does.
	static const char *const endings[] = {".srt", ".SRT"};
	for (size_t i = 0; i < sizeof(endings) / sizeof(endings[0]); i++) {
		snprintf(out, out_size, "%.*s%s%.*s%s", (int)dir_len, film_path, slash ? "/" : "", (int)stem_len, name, endings[i]);
		if (readable(out)) {
			return true;
		}
	}

	char dir[1024];
	snprintf(dir, sizeof(dir), "%.*s", (int)dir_len, film_path);
	DIR *d = opendir(slash ? (dir_len ? dir : "/") : ".");
	if (!d) {
		return false;
	}
	// The first in alphabetical order, so that the same one is picked each
	// time whatever order the folder hands its names out in.
	char best[256] = "";
	struct dirent *entry;
	while ((entry = readdir(d)) != NULL) {
		size_t len = strlen(entry->d_name);
		if (len < stem_len + 5 || len >= sizeof(best)) {
			continue;
		}
		if (strncasecmp(entry->d_name, name, stem_len) != 0 || entry->d_name[stem_len] != '.' ||
			strcasecmp(entry->d_name + len - 4, ".srt") != 0) {
			continue;
		}
		if (!best[0] || strcasecmp(entry->d_name, best) < 0) {
			snprintf(best, sizeof(best), "%s", entry->d_name);
		}
	}
	closedir(d);
	if (!best[0]) {
		return false;
	}
	snprintf(out, out_size, "%.*s%s%s", (int)dir_len, film_path, slash ? "/" : "", best);
	return readable(out);
}

// ---------------------------------------------------------------------------
// the text
// ---------------------------------------------------------------------------

static bool is_utf8(const unsigned char *s, size_t len) {
	size_t i = 0;
	while (i < len) {
		unsigned char c = s[i];
		size_t more;
		if (c < 0x80) {
			more = 0;
		} else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
			more = 1;
		} else if ((c & 0xF0) == 0xE0) {
			more = 2;
		} else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
			more = 3;
		} else {
			return false;
		}
		if (i + more >= len + (more == 0)) {
			return false; // cut off in the middle of a character
		}
		for (size_t k = 1; k <= more; k++) {
			if ((s[i + k] & 0xC0) != 0x80) {
				return false;
			}
		}
		i += more + 1;
	}
	return true;
}

// What Windows-1252 has between 0x80 and 0x9F, where Latin-1 has control
// codes nobody types: the curly quotes and the long dash that subtitle files
// are full of. Zero where the code page has nothing.
static const uint16_t cp1252_high[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

// A copy of `in` as UTF-8, read as Windows-1252. No character of it needs
// more than three bytes. The caller frees it.
static char *from_cp1252(const unsigned char *in, size_t len, size_t *out_len) {
	char *out = malloc(len * 3 + 1);
	if (!out) {
		return NULL;
	}
	size_t n = 0;
	for (size_t i = 0; i < len; i++) {
		unsigned code = in[i];
		if (code >= 0x80 && code <= 0x9F) {
			code = cp1252_high[code - 0x80];
			if (!code) {
				code = '?';
			}
		}
		if (code < 0x80) {
			out[n++] = (char)code;
		} else if (code < 0x800) {
			out[n++] = (char)(0xC0 | (code >> 6));
			out[n++] = (char)(0x80 | (code & 0x3F));
		} else {
			out[n++] = (char)(0xE0 | (code >> 12));
			out[n++] = (char)(0x80 | ((code >> 6) & 0x3F));
			out[n++] = (char)(0x80 | (code & 0x3F));
		}
	}
	out[n] = '\0';
	*out_len = n;
	return out;
}

// "00:01:02,345" at `s`, with a full stop accepted for the comma and the
// hours as long as they come. Returns what follows it, or NULL.
static const char *parse_time(const char *s, int64_t *ms_out) {
	unsigned h, m, sec, ms;
	int used = 0;
	if (sscanf(s, " %u:%u:%u%*1[,.]%u%n", &h, &m, &sec, &ms, &used) != 4 || used == 0) {
		return NULL;
	}
	if (m > 59 || sec > 59 || ms > 999 || h > 99) {
		return NULL;
	}
	*ms_out = ((int64_t)h * 3600 + m * 60 + sec) * 1000 + ms;
	return s + used;
}

// Whether the line holds "start --> end", and the two times if it does.
static bool parse_timing(const char *line, int64_t *start, int64_t *end) {
	const char *arrow = strstr(line, "-->");
	if (!arrow) {
		return false;
	}
	return parse_time(line, start) != NULL && parse_time(arrow + 3, end) != NULL && *end > *start;
}

// Appends one line of a cue to `out` without its markup: what stands between
// < and > when it looks like a tag, and between { and } when it starts with a
// backslash, which is how a position is given ({\an8}). A "<" that opens no
// tag, as in "2 < 3", stays.
static size_t add_clean(char *out, size_t used, size_t size, const char *line, size_t len) {
	for (size_t i = 0; i < len && used + 1 < size; i++) {
		char c = line[i];
		if (c == '<') {
			size_t k = i + 1;
			if (k < len && line[k] == '/') {
				k++;
			}
			if (k < len && isalpha((unsigned char)line[k])) {
				const char *close = memchr(line + k, '>', len - k);
				if (close) {
					i = (size_t)(close - line);
					continue;
				}
			}
		} else if (c == '{' && i + 1 < len && line[i + 1] == '\\') {
			const char *close = memchr(line + i, '}', len - i);
			if (close) {
				i = (size_t)(close - line);
				continue;
			}
		}
		out[used++] = c;
	}
	return used;
}

// Does not leave half a character at the end of a text that was cut short.
static size_t trim_partial_utf8(const char *s, size_t len) {
	size_t back = 0;
	while (back < 4 && back < len && ((unsigned char)s[len - 1 - back] & 0xC0) == 0x80) {
		back++;
	}
	if (back < len) {
		unsigned char lead = (unsigned char)s[len - 1 - back];
		size_t want = lead < 0x80 ? 0 : (lead & 0xE0) == 0xC0 ? 1 : (lead & 0xF0) == 0xE0 ? 2 : 3;
		if (lead >= 0x80 && back < want) {
			return len - 1 - back;
		}
	}
	return len;
}

static int by_start(const void *a, const void *b) {
	const cue_t *x = a, *y = b;
	return x->start_ms < y->start_ms ? -1 : x->start_ms > y->start_ms;
}

// The file is a run of blocks with an empty line between them: a number, a
// line of times, and the lines of text. The number is not needed and is not
// always there, so a block starts wherever a line of times is found.
static bool parse(char *data, size_t len) {
	int cue_room = 512;
	cues = malloc((size_t)cue_room * sizeof(*cues));
	// No text is longer cleaned than it was raw, and each cue adds one NUL at
	// the cost of at least its line of times.
	texts = malloc(len + 1);
	if (!cues || !texts) {
		subtitles_unload();
		return false;
	}

	char *end = data + len;
	char *line = data;
	bool in_cue = false;
	char text[MAX_CUE_BYTES];
	size_t text_len = 0;
	int64_t start = 0, stop = 0;

	while (line <= end) {
		char *nl = line < end ? memchr(line, '\n', (size_t)(end - line)) : NULL;
		char *line_end = nl ? nl : end;
		size_t n = (size_t)(line_end - line);
		if (n && line[n - 1] == '\r') {
			n--;
		}
		bool at_end = line >= end;

		char saved = line[n]; // data has one byte to spare after `end`
		line[n] = '\0';
		int64_t s, e;
		bool timing = !at_end && parse_timing(line, &s, &e);
		line[n] = saved;

		bool blank = n == 0;
		if (in_cue && (blank || timing || at_end)) {
			// A line of times straight after text means the empty line between
			// two blocks is missing, and the last line taken was the next
			// block's number.
			if (timing && text_len) {
				char *last = NULL;
				for (size_t i = text_len; i > 0 && !last; i--) {
					if (text[i - 1] == '\n') {
						last = text + i - 1;
					}
				}
				const char *tail = last ? last + 1 : text;
				size_t tail_len = text_len - (size_t)(tail - text);
				bool digits = tail_len > 0;
				for (size_t i = 0; i < tail_len; i++) {
					digits = digits && isdigit((unsigned char)tail[i]);
				}
				if (digits) {
					text_len = last ? (size_t)(last - text) : 0;
				}
			}
			text_len = trim_partial_utf8(text, text_len);
			if (text_len && cue_count < MAX_CUES) {
				if (cue_count == cue_room) {
					cue_room *= 2;
					cue_t *grown = realloc(cues, (size_t)cue_room * sizeof(*cues));
					if (!grown) {
						break; // what was read so far is still good
					}
					cues = grown;
				}
				cues[cue_count].start_ms = start;
				cues[cue_count].end_ms = stop;
				cues[cue_count].text = (uint32_t)texts_used;
				memcpy(texts + texts_used, text, text_len);
				texts[texts_used + text_len] = '\0';
				texts_used += text_len + 1;
				cue_count++;
			}
			in_cue = false;
		}
		if (timing) {
			in_cue = true;
			start = s;
			stop = e;
			text_len = 0;
		} else if (in_cue && !blank) {
			if (text_len && text_len + 1 < sizeof(text)) {
				text[text_len++] = '\n';
			}
			text_len = add_clean(text, text_len, sizeof(text), line, n);
		}

		if (at_end || !nl) {
			if (at_end) {
				break;
			}
			line = end; // one more turn, to close a cue the file ends in
			continue;
		}
		line = nl + 1;
	}

	if (cue_count == 0) {
		subtitles_unload();
		return false;
	}
	// Nearly always in order already; a file put together from two is not.
	qsort(cues, (size_t)cue_count, sizeof(*cues), by_start);
	return true;
}

bool subtitles_load(const char *film_path) {
	subtitles_unload();
	if (!film_path || !film_path[0]) {
		return false;
	}
	char path[1400];
	if (!find_file(film_path, path, sizeof(path))) {
		return false;
	}

	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	// One byte more than is read, so that a line can be ended in place.
	char *data = malloc(MAX_FILE_BYTES + 2);
	if (!data) {
		fclose(f);
		return false;
	}
	size_t len = fread(data, 1, MAX_FILE_BYTES + 1, f);
	fclose(f);
	if (len == 0 || len > MAX_FILE_BYTES) {
		fprintf(stderr, "subtitles: %s is %s; not used\n", path, len ? "too large to be subtitles" : "empty");
		free(data);
		return false;
	}

	char *body = data;
	if (len >= 3 && memcmp(body, "\xEF\xBB\xBF", 3) == 0) {
		body += 3; // the mark Windows editors put in front of UTF-8
		len -= 3;
	}
	char *converted = NULL;
	const char *encoding = "UTF-8";
	if (!is_utf8((const unsigned char *)body, len)) {
		size_t converted_len = 0;
		converted = from_cp1252((const unsigned char *)body, len, &converted_len);
		if (!converted) {
			free(data);
			return false;
		}
		body = converted;
		len = converted_len;
		encoding = "Windows-1252";
	}
	body[len] = '\0';

	bool ok = parse(body, len);
	free(converted);
	free(data);
	if (ok) {
		printf("subtitles: %d cues from %s (%s)\n", cue_count, path, encoding);
	} else {
		fprintf(stderr, "subtitles: nothing to show in %s\n", path);
	}
	return ok;
}

int subtitles_cue_at(int64_t position_ms) {
	if (cue_count == 0) {
		return -1;
	}
	// The last cue that has started.
	int low = 0, high = cue_count - 1, found = -1;
	while (low <= high) {
		int mid = (low + high) / 2;
		if (cues[mid].start_ms <= position_ms) {
			found = mid;
			low = mid + 1;
		} else {
			high = mid - 1;
		}
	}
	// It may be over while an earlier, longer one still runs: two people
	// speaking at once are written as two cues. A few back is as far as that
	// ever goes.
	for (int i = found; i >= 0 && i > found - 4; i--) {
		if (position_ms < cues[i].end_ms) {
			return i;
		}
	}
	return -1;
}

const char *subtitles_text(int cue) {
	if (cue < 0 || cue >= cue_count) {
		return "";
	}
	return texts + cues[cue].text;
}
