#!/usr/bin/env python3
"""Builds a tiny stereo DSD64 .dsf and .dff for the output-stage regression.

4096 bytes per channel, filled with one known pattern, which is enough for the
DoP and PCM paths to be read, checked and seeked. The containers carry the
fields src/system/decode/dsd.c reads, in the order it reads them:

  .dsf  "DSD " chunk (28 bytes), then a 52-byte "fmt " chunk (version, format
        id, channel type, channel count, rate, bits, sample count per channel,
        block size), then "data". Block size equals the whole channel, so the
        stream is one block per channel, and the bits are least significant
        first, as a real .dsf has them.
  .dff  "FRM8"/"DSD ", then a PROP chunk of type "SND " holding FS (rate),
        CHNL (channel count and ids) and the byte-interleaved DSD data, most
        significant bit first.

Usage: make_dsd_fixtures.py <output-directory>
"""

import os
import sys

RATE = 2822400
CHANNELS = 2
BYTES_PER_CHANNEL = 4096
BLOCK = BYTES_PER_CHANNEL


def le16(value):
    return bytes((value & 0xFF, (value >> 8) & 0xFF))


def le32(value):
    return bytes((value & 0xFF, (value >> 8) & 0xFF,
                  (value >> 16) & 0xFF, (value >> 24) & 0xFF))


def le64(value):
    return le32(value & 0xFFFFFFFF) + le32(value >> 32)


def be16(value):
    return bytes(((value >> 8) & 0xFF, value & 0xFF))


def be32(value):
    return bytes(((value >> 24) & 0xFF, (value >> 16) & 0xFF,
                  (value >> 8) & 0xFF, value & 0xFF))


def be64(value):
    return be32(value >> 32) + be32(value & 0xFFFFFFFF)


def pattern(channel, index):
    """What dsd_regression_test.c expects, byte for byte."""
    return (channel * 31 + index * 7 + 17) & 0xFF


def channel_bytes(channel):
    return bytes(pattern(channel, i) for i in range(BYTES_PER_CHANNEL))


def write_dsf(path):
    # One block per channel, so the file is channel 0's block then channel 1's.
    audio = b"".join(channel_bytes(c) for c in range(CHANNELS))

    head = b"DSD " + le64(28) + le64(28 + 52 + 12 + len(audio)) + le64(0)
    assert len(head) == 28

    fmt = (b"fmt " + le64(52) +
           le32(1) +                        # format version
           le32(0) +                        # format id: DSD raw
           le32(2) +                        # channel type: stereo
           le32(CHANNELS) +
           le32(RATE) +
           le32(1) +                        # bits per sample: 1 = lsb first
           le64(BYTES_PER_CHANNEL * 8) +    # sample count, per channel
           le32(BLOCK) +
           le32(0))                         # reserved
    assert len(fmt) == 52

    data = b"data" + le64(12 + len(audio)) + audio

    with open(path, "wb") as f:
        f.write(head + fmt + data)


def write_dff(path):
    interleaved = bytearray()
    per_channel = [channel_bytes(c) for c in range(CHANNELS)]
    for i in range(BYTES_PER_CHANNEL):
        for c in range(CHANNELS):
            interleaved.append(per_channel[c][i])

    fs = b"FS  " + be64(4) + be32(RATE)
    chnl_body = be16(CHANNELS) + b"SLFT" + b"SRGT"
    chnl = b"CHNL" + be64(len(chnl_body)) + chnl_body
    dsd = b"DSD " + be64(len(interleaved)) + bytes(interleaved)

    prop_body = b"SND " + fs + chnl + dsd
    prop = b"PROP" + be64(len(prop_body)) + prop_body
    form = b"DSD "
    frm8 = b"FRM8" + be64(len(form) + len(prop)) + form + prop

    with open(path, "wb") as f:
        f.write(frm8)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_dsd_fixtures.py <output-directory>\n")
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    write_dsf(os.path.join(out, "stereo.dsf"))
    write_dff(os.path.join(out, "stereo.dff"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
