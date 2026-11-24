#!/usr/bin/env python3
"""Damages an exFAT image in one chosen way, for tests/exfatcheck_test.sh.

    exfatcheck_images.py IMAGE lost       six clusters marked taken that no file owns
    exfatcheck_images.py IMAGE unmarked   the first cluster of big.bin marked free
    exfatcheck_images.py IMAGE cut        frag-a.bin's chain ends in nothing
    exfatcheck_images.py IMAGE crosslink  frag-a.bin's chain runs into frag-b.bin
    exfatcheck_images.py IMAGE info       says whether frag-a.bin is in pieces

The images come from mkfs.exfat and the kernel's driver; this is not a general
exFAT editor.
"""
import struct
import sys


class Image:
    def __init__(self, path):
        self.f = open(path, "r+b")
        boot = self.f.read(512)
        assert boot[3:11] == b"EXFAT   "
        self.bps = 1 << boot[108]
        self.cluster = self.bps << boot[109]
        self.fat = struct.unpack_from("<I", boot, 80)[0] * self.bps
        self.heap = struct.unpack_from("<I", boot, 88)[0] * self.bps
        self.clusters = struct.unpack_from("<I", boot, 92)[0]
        self.root = struct.unpack_from("<I", boot, 96)[0]
        self.files = {}
        self.bitmap_first = None
        self.read_root()

    def cluster_offset(self, c):
        return self.heap + (c - 2) * self.cluster

    def fat_get(self, c):
        self.f.seek(self.fat + c * 4)
        return struct.unpack("<I", self.f.read(4))[0]

    def fat_put(self, c, value):
        self.f.seek(self.fat + c * 4)
        self.f.write(struct.pack("<I", value))

    def read_root(self):
        data = b""
        c = self.root
        while 2 <= c < 0xFFFFFFF7:
            self.f.seek(self.cluster_offset(c))
            data += self.f.read(self.cluster)
            c = self.fat_get(c)
        i = 0
        while i + 32 <= len(data):
            e = data[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0x81:
                self.bitmap_first = struct.unpack_from("<I", e, 20)[0]
            if e[0] == 0x85:
                count = e[1]
                stream = data[i + 32:i + 64]
                name = b"".join(data[i + 64 + 32 * k + 2:i + 96 + 32 * k] for k in range(count - 1))
                name = name.decode("utf-16-le")[:stream[3]]
                self.files[name] = dict(one_piece=bool(stream[1] & 2), first=struct.unpack_from("<I", stream, 20)[0],
                                        size=struct.unpack_from("<Q", stream, 24)[0])
                i += 32 * count
            i += 32

    def bit(self, c, value=None):
        # The bitmap of a test image is small and lies in one piece.
        offset = self.cluster_offset(self.bitmap_first) + (c - 2) // 8
        self.f.seek(offset)
        byte = self.f.read(1)[0]
        mask = 1 << ((c - 2) % 8)
        if value is None:
            return bool(byte & mask)
        self.f.seek(offset)
        self.f.write(bytes([byte | mask if value else byte & ~mask]))


def main():
    img = Image(sys.argv[1])
    what = sys.argv[2]
    if what == "info":
        print("pieces" if not img.files["frag-a.bin"]["one_piece"] else "one-piece")
    elif what == "lost":
        free = [c for c in range(img.clusters + 1, 1, -1) if not img.bit(c)][:6]
        assert len(free) == 6
        for c in free:
            img.bit(c, True)
    elif what == "unmarked":
        img.bit(img.files["big.bin"]["first"], False)
    elif what == "cut":
        assert not img.files["frag-a.bin"]["one_piece"]
        img.fat_put(img.files["frag-a.bin"]["first"], 0)
    elif what == "crosslink":
        assert not img.files["frag-a.bin"]["one_piece"]
        img.fat_put(img.files["frag-a.bin"]["first"], img.files["frag-b.bin"]["first"])
    else:
        raise SystemExit("unknown damage %r" % what)
    img.f.close()


main()
