#ifndef DST_DECODER_H
#define DST_DECODER_H

#include <stdint.h>

// DST (Direct Stream Transfer) decoding: the lossless compression SACD uses
// for its DSD stream.
//
// The decoder is vendored from FFmpeg's libdst (LGPL-2.1-or-later; the
// copyright and licence header is in dst_decoder.c). It is the single-frame
// core only -- no container: an .iso reader hands it one DST frame at a time
// and receives the decoded DSD bytes.
//
// At DSD64 a frame carries 37632 one-bit samples per channel, which the
// decoder writes as 4704 interleaved DSD bytes per channel (8 samples per
// byte); the caller's buffer must hold 4704 * channels bytes. Both the sample
// count and the byte count scale with the DSD rate.

typedef struct dst_decoder dst_decoder_t;

// Channels is the SACD channel count (1..6) and sample_rate the DSD rate
// itself (2822400 for DSD64, 5644800 for DSD128). Returns 0, or -1 when the
// channel count is unsupported or memory is short.
int dst_decoder_init(dst_decoder_t **decoder, int channel_count,
		     int sample_rate);

// Frees the decoder. Safe with NULL. Returns 0.
int dst_decoder_close(dst_decoder_t *decoder);

// Decodes one DST frame. `dst_data`/`frame_size` is the frame's payload as it
// sits in the ISO, and `dsd_output` must have room for 4704 * channels bytes.
// Returns 0 on success, with *dsd_output_len the number of interleaved DSD
// bytes produced (4704 * channels for a valid frame); negative on bad data.
int dst_decoder_decode(dst_decoder_t *decoder, const uint8_t *dst_data,
		       int frame_size, uint8_t *dsd_output,
		       int *dsd_output_len);

#endif /* DST_DECODER_H */
