#!/usr/bin/env python3
"""Damages a FAT32 image in one chosen way, for tests/fatcheck_test.sh.

    fatcheck_images.py IMAGE lost          six clusters taken and owned by nothing
    fatcheck_images.py IMAGE tables        the second table differs from the first
    fatcheck_images.py IMAGE crosslink     BIG.BIN runs into OTHER.BIN
    fatcheck_images.py IMAGE cut           BIG.BIN's chain ends in free space
    fatcheck_images.py IMAGE short         BIG.BIN's chain ends before its size does

The images come from mkfs.fat and mtools; nothing here is a general FAT editor.
"""
import struct
import sys

EOC = 0x0FFFFFFF


class Image:
    def __init__(self, path):
        self.f = open(path, "r+b")
        boot = self.f.read(512)
        self.bps = struct.unpack_from("<H", boot, 11)[0]
        self.spc = boot[13]
        self.reserved = struct.unpack_from("<H", boot, 14)[0]
        self.nfats = boot[16]
        self.fatsz = struct.unpack_from("<I", boot, 36)[0]
        self.root = struct.unpack_from("<I", boot, 44)[0]
        total = struct.unpack_from("<I", boot, 32)[0]
        self.data = self.reserved + self.nfats * self.fatsz
        self.clusters = (total - self.data) // self.spc

    def fat_offset(self, copy, cluster):
        return (self.reserved + copy * self.fatsz) * self.bps + cluster * 4

    def get(self, cluster, copy=0):
        self.f.seek(self.fat_offset(copy, cluster))
        return struct.unpack("<I", self.f.read(4))[0] & 0x0FFFFFFF

    def put(self, cluster, value, copies=None):
        for copy in range(self.nfats) if copies is None else copies:
            self.f.seek(self.fat_offset(copy, cluster))
            self.f.write(struct.pack("<I", value))

    def free_clusters(self, wanted):
        found = []
        # From the far end, where a small test tree has put nothing.
        for c in range(self.clusters + 1, 1, -1):
            if self.get(c) == 0:
                found.append(c)
                if len(found) == wanted:
                    return found
        raise SystemExit("no free clusters to work with")

    def root_entry(self, short_name):
        """First cluster of a file in the root folder, by its 8.3 name."""
        c = self.root
        while c < 0x0FFFFFF8:
            self.f.seek((self.data + (c - 2) * self.spc) * self.bps)
            block = self.f.read(self.spc * self.bps)
            for i in range(0, len(block), 32):
                e = block[i:i + 32]
                if e[0] == 0:
                    break
                if e[11] & 0x0F == 0x0F or e[0] == 0xE5:
                    continue
                if e[0:11] == short_name:
                    hi, lo = struct.unpack_from("<H", e, 20)[0], struct.unpack_from("<H", e, 26)[0]
                    return hi << 16 | lo
            c = self.get(c)
        raise SystemExit("no %r in the root folder" % short_name)


def main():
    path, what = sys.argv[1], sys.argv[2]
    img = Image(path)
    big = lambda: img.root_entry(b"BIG     BIN")
    other = lambda: img.root_entry(b"OTHER   BIN")
    if what == "lost":
        a = img.free_clusters(6)
        for here, nxt in zip(a[:4], a[1:5]):  # a chain of five
            img.put(here, nxt)
        img.put(a[4], EOC)
        img.put(a[5], EOC)  # and one on its own
    elif what == "tables":
        c = img.free_clusters(1)[0]
        img.put(c, EOC, copies=[1])
    elif what == "crosslink":
        img.put(big(), other())
    elif what == "cut":
        img.put(img.get(big()), 0)
    elif what == "short":
        img.put(big(), EOC)
    else:
        raise SystemExit("unknown damage %r" % what)
    img.f.close()


main()
