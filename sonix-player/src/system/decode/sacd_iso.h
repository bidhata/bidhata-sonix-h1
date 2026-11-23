#ifndef SACD_ISO_H
#define SACD_ISO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// SACD: the ScarletBook layout inside a ripped .iso.
//
// An SACD carries up to six channels of DSD in one of three shapes. Plain DSD
// is stored in fixed 3-in-14 or 3-in-16 sector patterns; DST (Direct Stream
// Transfer) is DSD compressed losslessly, one frame at a time, and needs the
// decoder in dst_decoder.h. The TOC, the track list, the text and the DST
// access list all live in the first sectors of the image and are described in
// terms of 2048-byte sectors (LSN * 2048).
//
// Every area in the master TOC is parsed and exposed; the player normally
// picks the two-channel one (sacd_iso_stereo_area()), and the multichannel
// area is a second, separate TOC with its own audio. A track is presented as
// `frames * 4704` bytes per channel -- 588 samples a frame at 64 bits a
// sample -- read out of the interleaved frame buffer the container hands
// over.

#define SACD_MAX_TRACKS 255
#define SACD_MAX_AREAS 2

typedef struct {
	int number;		 /* 1-based */
	uint32_t start_lsn;	 /* absolute LSN of the track's first sector */
	uint32_t length_lsn;	 /* sectors in the track */
	uint32_t start_frame;	 /* absolute 75 fps frame, from SACDTRL2 */
	uint32_t frames;	 /* duration in 75 fps frames, from SACDTRL2 */
	char title[256];
	char performer[256];
} sacd_track_t;

typedef struct {
	int channels;
	int frame_format;	  /* 0 DST, 2 DSD 3-in-14, 3 DSD 3-in-16 */
	uint32_t track_start_lsn; /* area Track Area start */
	uint32_t track_end_lsn;	  /* area Track Area end */
	int track_count;
	sacd_track_t tracks[SACD_MAX_TRACKS];
} sacd_area_t;

typedef struct sacd_iso sacd_iso_t;

// Opens the image, or a virtual track path (see sacd_virtual_path()); NULL
// when it is not a readable ScarletBook image. Never fails on a truncated or
// random file other than by returning NULL.
sacd_iso_t *sacd_iso_open(const char *path);
void sacd_iso_close(sacd_iso_t *s);

int sacd_iso_area_count(const sacd_iso_t *s);
const sacd_area_t *sacd_iso_area(const sacd_iso_t *s, int area);
int sacd_iso_stereo_area(const sacd_iso_t *s); /* -1 when none */
const char *sacd_iso_album_title(const sacd_iso_t *s);
const char *sacd_iso_album_artist(const sacd_iso_t *s);
const char *sacd_iso_genre(const sacd_iso_t *s);
int sacd_iso_year(const sacd_iso_t *s);

// Makes a track of an area the one read and seek work on. Track is 1-based.
bool sacd_iso_select(sacd_iso_t *s, int area, int track);

int sacd_iso_channels(const sacd_iso_t *s);
int sacd_iso_multiple(const sacd_iso_t *s);	   /* always 64 */
uint32_t sacd_iso_rate(const sacd_iso_t *s);	   /* 2822400 */
uint64_t sacd_iso_track_bytes(const sacd_iso_t *s); /* per channel */

// Fills chan[c] with up to `bytes` per channel from the selected track.
// Returns how many bytes per channel were produced; 0 at the end of the
// track and when nothing is selected.
uint64_t sacd_iso_read(sacd_iso_t *s, unsigned char *const *chan,
		       uint64_t bytes);
bool sacd_iso_seek(sacd_iso_t *s, uint64_t byte);

// True when the file name ends in ".iso" (case-insensitive), ignoring a
// virtual track suffix.
bool sacd_is_iso(const char *path);

// The virtual path this player uses for one track of an image, 1-based:
//
//     /mnt/sd/Music/Album/disc.iso?sacd=3
//
// Everything downstream carries paths, so a track of an image has to be
// expressible as one.
void sacd_virtual_path(const char *iso_path, int track_number, char *out,
		       size_t size);

// Splits a virtual path. Returns the 1-based track number and copies the
// image path into `iso_out`; returns 0 for an ordinary path (still copied).
int sacd_split_path(const char *path, char *iso_out, size_t size);

// Orders two paths that are both tracks of the same image, by track number.
//
// It exists because the alternative is alphabetical, and alphabetically track
// ten comes before track two. Returns false when the two are not tracks of one
// image; `out` is only written when it returns true.
bool sacd_order_tracks(const char *a, const char *b, int *out);

#endif /* SACD_ISO_H */
