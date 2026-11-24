#ifndef SUBTITLES_H
#define SUBTITLES_H

#include <stdbool.h>
#include <stdint.h>

// Subtitles for a film, from a SubRip file kept beside it.
//
// "Film.mkv" takes "Film.srt", in the same folder. Failing that, the first
// file there whose name starts with the film's and ends in .srt, which is how
// a language is usually marked: "Film.en.srt". Subtitles carried inside the
// film itself are not read: the decoder is built without the parts for them.
//
// The text is handed out as UTF-8 with the lines of a cue separated by '\n'
// and SubRip's markup (<i>, <font ...>, {\an8}) taken out. A file that is not
// UTF-8 is taken for Windows-1252, the usual other case, and converted.
//
// Everything here is the interface thread's: a file is read when a film
// opens, and looked into on the page's tick.

// Reads the subtitles that go with `film_path`, dropping any read before.
// False when there are none, which is the ordinary case and not an error.
bool subtitles_load(const char *film_path);

// Forgets them.
void subtitles_unload(void);

// The cue to show at `position_ms`: its number from 0, or -1 for none. Cheap
// enough to ask on every tick.
int subtitles_cue_at(int64_t position_ms);

// A cue's text. Stays valid until subtitles_load() or subtitles_unload().
const char *subtitles_text(int cue);

#endif /* SUBTITLES_H */
