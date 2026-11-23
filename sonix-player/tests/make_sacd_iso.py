#!/usr/bin/env python3
"""Builds small synthetic SACD ISOs for the reader's tests.

The layouts follow the ScarletBook offset note in
.superpowers/sdd/2026-11-16-sacd-iso/task-2-format-note.md: master TOC at
LSN 510, area TOC at 600, track lists at 601/602, track text at 603, access
list at 607, audio at 700. Everything is big-endian and every sector is 2048
bytes.

Usage: make_sacd_iso.py <output-directory>
"""

import os
import sys

SECTOR = 2048
BYTES_PER_FRAME = 4704
AUDIO_LSN = 700


def be16(value):
    return bytes(((value >> 8) & 0xFF, value & 0xFF))


def be24(value):
    return bytes(((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF))


def be32(value):
    return bytes(((value >> 24) & 0xFF, (value >> 16) & 0xFF,
                  (value >> 8) & 0xFF, value & 0xFF))


class Image:
    def __init__(self, sectors):
        self.data = bytearray(sectors * SECTOR)

    def put(self, lsn, offset, blob):
        start = lsn * SECTOR + offset
        self.data[start:start + len(blob)] = blob

    def put8(self, lsn, offset, value):
        self.data[lsn * SECTOR + offset] = value & 0xFF

    def put16(self, lsn, offset, value):
        self.put(lsn, offset, be16(value))

    def put24(self, lsn, offset, value):
        self.put(lsn, offset, be24(value))

    def put32(self, lsn, offset, value):
        self.put(lsn, offset, be32(value))

    def write(self, path):
        with open(path, "wb") as f:
            f.write(self.data)


def timecode(frames):
    """75 fps frame number -> (minutes, seconds, frames)."""
    return frames // 4500, (frames // 75) % 60, frames % 75


def put_timecode(image, lsn, offset, frames, flags=0):
    m, s, f = timecode(frames)
    image.put(lsn, offset, bytes((m, s, f, flags)))


def frame_bytes(track, rel_frame):
    """One interleaved stereo frame of the test pattern.

    `track` is 1-based and `rel_frame` is relative to the track's first frame,
    which is what the expected track-relative byte index uses.
    """
    buf = bytearray(BYTES_PER_FRAME * 2)
    base = rel_frame * BYTES_PER_FRAME
    for k in range(len(buf)):
        channel = k & 1
        index = base + (k >> 1)
        buf[k] = (track * 17 + channel * 31 + index * 7) & 0xFF
    return bytes(buf)


# {offset, length} reads for each frame position in a 3-frame block, copied
# from the fixed layouts in libsacd's sacd_dsd_reader.c.
LAYOUT_3_IN_14 = {
    "sector_mul": 14,
    0: {"addition": 0, "reads": [(32, 2016), (32, 2016), (32, 2016),
                                (32, 2016), (32, 1344)]},
    1: {"addition": 4, "reads": [(32 + 1344, 672), (32, 2016), (32, 2016),
                                (32, 2016), (32, 2016), (32, 672)]},
    2: {"addition": 9, "reads": [(32 + 672, 1344), (32, 2016), (32, 2016),
                                (32, 2016), (32, 2016)]},
}

LAYOUT_3_IN_16 = {
    "sector_mul": 16,
    0: {"addition": 0, "reads": [(284, 1764), (284, 1764), (284, 1764),
                                (284, 1764), (284, 1764), (284, 588)]},
    1: {"addition": 5, "reads": [(284 + 588, 1176), (284, 1764), (284, 1764),
                                (284, 1764), (284, 1764), (284, 1176)]},
    2: {"addition": 10, "reads": [(284 + 1176, 588), (284, 1764), (284, 1764),
                                 (284, 1764), (284, 1764), (284, 1764)]},
}


def write_master(image):
    lsn = 510
    image.put(lsn, 0, b"SACDMTOC")
    image.put8(lsn, 8, 1)    # version 1.20
    image.put8(lsn, 9, 20)
    image.put8(lsn, 40, 1)   # general genre table
    image.put16(lsn, 42, 5)  # Classical
    image.put32(lsn, 64, 600)
    image.put16(lsn, 84, 40)
    image.put16(lsn, 120, 2024)
    image.put8(lsn, 122, 1)
    image.put8(lsn, 123, 1)
    image.put8(lsn, 128, 1)  # one master text area
    image.put(lsn, 136, b"en")
    image.put8(lsn, 138, 2)  # ISO-8859-1

    # Master text sector: title at position 16, artist at 18; strings at 64/96.
    text = 511
    image.put(text, 0, b"SACDText")
    image.put16(text, 16, 64)
    image.put16(text, 18, 96)
    image.put(text, 64, b"SACD Test Album\0")
    image.put(text, 96, b"Test Artist\0")

    image.put(519, 0, b"SACD_Man")


def text_item(entries):
    """count, three reserved bytes, then {type, 0x20, string} per entry."""
    out = bytearray((len(entries), 0, 0, 0))
    for kind, value in entries:
        out += bytes((kind, 0x20))
        out += value.encode("latin-1") + b"\0"
        while len(out) % 4:
            out += b"\0"
    return bytes(out)


def build_iso(path, frame_format, tracks, total_frames, track_end,
              total_sectors, access, fill_audio):
    image = Image(total_sectors)
    write_master(image)

    toc = 600
    image.put(toc, 0, b"TWOCHTOC")
    image.put8(toc, 8, 1)
    image.put8(toc, 9, 20)
    image.put16(toc, 10, 40)
    image.put8(toc, 20, 4)  # 64 * 44.1 kHz
    image.put8(toc, 21, frame_format)
    image.put8(toc, 32, 2)  # channels
    m, s, f = timecode(total_frames)
    image.put(toc, 64, bytes((m, s, f)))
    image.put8(toc, 69, len(tracks))
    image.put32(toc, 72, AUDIO_LSN)
    image.put32(toc, 76, track_end)
    image.put8(toc, 80, 1)  # text channel count
    image.put(toc, 88, b"en")
    image.put8(toc, 90, 2)      # ISO-8859-1
    image.put16(toc, 128, 3)    # track text at LSN 603
    image.put16(toc, 132, 7)    # access list at LSN 607

    # SACDTRL1: start LSN and length in sectors.
    image.put(601, 0, b"SACDTRL1")
    for n, track in enumerate(tracks):
        image.put32(601, 8 + 4 * n, track["start_lsn"])
        image.put32(601, 1028 + 4 * n, track["length_lsn"])

    # SACDTRL2: start and duration as 75 fps timecodes.
    image.put(602, 0, b"SACDTRL2")
    for n, track in enumerate(tracks):
        put_timecode(image, 602, 8 + 4 * n, track["start_frame"])
        put_timecode(image, 602, 1028 + 4 * n, track["frames"])

    # Track text: pointer table at 8 + 2n, item bodies from block offset 4096
    # (LSN 605), so the table's first 4096 bytes stay untouched.
    image.put(603, 0, b"SACDTTxt")
    position = 4096
    for n, track in enumerate(tracks):
        image.put16(603, 8 + 2 * n, position)
        body = text_item(((0x01, track["title"]), (0x02, track["performer"])))
        image.put(603, position, body)
        position += len(body)

    # Access list: two entries, flags then a 24-bit LSN, step 10.
    image.put(607, 0, b"SACD_ACC")
    image.put16(607, 8, len(access))
    image.put8(607, 10, 10)
    for n, lsn in enumerate(access):
        image.put16(607, 16 + 5 * n, 0)
        image.put24(607, 16 + 5 * n + 2, lsn)

    fill_audio(image)
    image.write(path)


def fill_fixed(image, layout, tracks):
    for track_number, track in enumerate(tracks, 1):
        for rel in range(track["frames"]):
            absolute = track["start_frame"] + rel
            block, slot = divmod(absolute, 3)
            pattern = layout[slot]
            sector = (AUDIO_LSN + block * layout["sector_mul"] +
                      pattern["addition"])
            frame = frame_bytes(track_number, rel)
            position = 0
            for offset, length in pattern["reads"]:
                image.put(sector, offset, frame[position:position + length])
                position += length
                sector += 1


def make_dsd14(path):
    tracks = [
        {"start_lsn": 700, "length_lsn": 47, "start_frame": 0, "frames": 10,
         "title": "Track 1", "performer": "Artist 1"},
        {"start_lsn": 746, "length_lsn": 24, "start_frame": 10, "frames": 5,
         "title": "Track 2", "performer": "Artist 2"},
    ]
    build_iso(path, 2, tracks, 15, 769, 770, [700, 746],
              lambda image: fill_fixed(image, LAYOUT_3_IN_14, tracks))


def make_dsd16(path):
    tracks = [
        {"start_lsn": 700, "length_lsn": 48, "start_frame": 0, "frames": 9,
         "title": "Track 1", "performer": "Artist 1"},
    ]
    build_iso(path, 3, tracks, 9, 747, 748, [700, 742],
              lambda image: fill_fixed(image, LAYOUT_3_IN_16, tracks))


def make_dst(path):
    tracks = [
        {"start_lsn": 700, "length_lsn": 75, "start_frame": 0, "frames": 15,
         "title": "Track 1", "performer": "Artist 1"},
    ]

    def fill_audio(image):
        lengths = (2041, 2041, 2041, 2041, 1245)
        for frame_index in range(15):
            payload = b"\0" + frame_bytes(1, frame_index)
            position = 0
            for part, length in enumerate(lengths):
                lsn = AUDIO_LSN + frame_index * 5 + part
                if part == 0:
                    image.put8(lsn, 0, 0x25)  # DST, 1 packet, 1 frame info
                    image.put16(lsn, 1, 0x97F9)  # start, audio, 2041
                    m, s, f = timecode(frame_index)
                    image.put(lsn, 3, bytes((m, s, f, 0x14)))  # 5 sectors
                    data_offset = 7
                else:
                    image.put8(lsn, 0, 0x21)  # DST, 1 packet, no frame info
                    image.put16(lsn, 1, (2 << 11) | length)  # audio, not start
                    data_offset = 3
                image.put(lsn, data_offset, payload[position:position + length])
                position += length

    build_iso(path, 0, tracks, 15, 774, 775, [700, 750], fill_audio)


def make_dst_shared(path):
    """Two frames, the second starting inside the first one's last sector.

    Frame 0 takes 5 sectors; frame 1 starts 1245 bytes into the fifth sector
    and takes 6 sectors in all. The shared sector has two packets and one
    frame info (for frame 1), so a reader that copies from byte 0 instead of
    skipping to the target's frame_start gets the previous tail, and one that
    trusts frame 0's span truncates frame 1.
    """
    tracks = [
        {"start_lsn": 700, "length_lsn": 10, "start_frame": 0, "frames": 2,
         "title": "Track 1", "performer": "Artist 1"},
    ]

    def fill_audio(image):
        frame0 = b"\0" + frame_bytes(1, 0)
        frame1 = b"\0" + frame_bytes(1, 1)

        image.put8(700, 0, 0x25)  # DST, 1 packet, 1 frame info
        image.put16(700, 1, 0x97F9)  # start, audio, 2041
        image.put(700, 3, bytes((0, 0, 0, 0x14)))  # frame 0 spans 5 sectors
        image.put(700, 7, frame0[0:2041])
        position = 2041
        for part in range(1, 4):
            lsn = 700 + part
            image.put8(lsn, 0, 0x21)
            image.put16(lsn, 1, (2 << 11) | 2041)
            image.put(lsn, 3, frame0[position:position + 2041])
            position += 2041
        assert position == 8164

        # Shared sector: frame 0's tail (1245) then frame 1's start (794).
        # Data begins after two packet infos and one frame info: offset 9.
        lsn = 704
        image.put8(lsn, 0, 0x45)  # DST, 2 packets, 1 frame info
        image.put16(lsn, 1, (0 << 15) | (2 << 11) | 1245)
        image.put16(lsn, 3, (1 << 15) | (2 << 11) | 794)
        image.put(lsn, 5, bytes((0, 0, 1, 0x18)))  # frame 1, 6 sectors
        image.put(lsn, 9, frame0[8164:9409])
        image.put(lsn, 1254, frame1[0:794])

        position = 794
        for part, length in enumerate((2041, 2041, 2041, 2041, 451)):
            lsn = 705 + part
            image.put8(lsn, 0, 0x21)
            image.put16(lsn, 1, (2 << 11) | length)
            image.put(lsn, 3, frame1[position:position + length])
            position += length
        assert position == len(frame1)

    build_iso(path, 0, tracks, 2, 709, 710, [700, 704], fill_audio)


def make_bad(path):
    Image(600).write(path)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_sacd_iso.py <output-directory>\n")
        return 2
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    make_dsd14(os.path.join(out, "dsd14.iso"))
    make_dsd16(os.path.join(out, "dsd16.iso"))
    make_dst(os.path.join(out, "dst.iso"))
    make_dst_shared(os.path.join(out, "dst_shared.iso"))
    make_bad(os.path.join(out, "bad.iso"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
