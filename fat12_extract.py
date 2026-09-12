#!/usr/bin/env python3
"""Extract the contents of a FAT12 floppy image.

    python3 fat12_extract.py disk1.img outdir

Prints a listing and writes every file out under `outdir`, preserving
subdirectories. Written for the 1.44 MB images produced by dosemu.c, but works
on any plain FAT12 image with a standard BPB in its boot sector.
"""
import os
import struct
import sys


def extract(img_path, outdir):
    d = open(img_path, 'rb').read()

    bps      = struct.unpack_from('<H', d, 11)[0]   # bytes per sector
    spc      = d[13]                                # sectors per cluster
    reserved = struct.unpack_from('<H', d, 14)[0]
    nfats    = d[16]
    rootents = struct.unpack_from('<H', d, 17)[0]
    spf      = struct.unpack_from('<H', d, 22)[0]   # sectors per FAT

    fat_start  = reserved * bps
    root_start = (reserved + nfats * spf) * bps
    data_start = root_start + rootents * 32
    fat = d[fat_start:fat_start + spf * bps]

    def fat_next(cluster):
        off = cluster + (cluster >> 1)              # cluster * 1.5
        val = fat[off] | (fat[off + 1] << 8)
        return (val >> 4) if (cluster & 1) else (val & 0xFFF)

    def chain(cluster):
        out = []
        while 2 <= cluster < 0xFF8 and len(out) < 5000:
            out.append(cluster)
            cluster = fat_next(cluster)
        return out

    def read_clusters(clusters):
        buf = b''
        for c in clusters:
            off = data_start + (c - 2) * spc * bps
            buf += d[off:off + spc * bps]
        return buf

    listing = []

    def walk(entries, path):
        for i in range(0, len(entries), 32):
            e = entries[i:i + 32]
            if len(e) < 32 or e[0] == 0:
                break
            if e[0] == 0xE5:                        # deleted
                continue
            attr = e[11]
            if attr == 0x0F:                        # VFAT long-name fragment
                continue

            name = e[0:8].decode('cp437').rstrip()
            ext = e[8:11].decode('cp437').rstrip()
            fn = name + ('.' + ext if ext else '')
            cluster = struct.unpack_from('<H', e, 26)[0]
            size = struct.unpack_from('<I', e, 28)[0]
            date = struct.unpack_from('<H', e, 24)[0]
            time = struct.unpack_from('<H', e, 22)[0]
            stamp = "%04d-%02d-%02d %02d:%02d" % (
                1980 + (date >> 9), (date >> 5) & 15, date & 31,
                time >> 11, (time >> 5) & 63)

            if attr & 0x08:                         # volume label
                listing.append(("VOL ", path + fn, 0, stamp))
            elif attr & 0x10:                       # directory
                if fn in ('.', '..'):
                    continue
                listing.append(("DIR ", path + fn + "/", 0, stamp))
                os.makedirs(os.path.join(outdir, path + fn), exist_ok=True)
                walk(read_clusters(chain(cluster)), path + fn + "/")
            else:
                listing.append(("FILE", path + fn, size, stamp))
                data = read_clusters(chain(cluster))[:size] if cluster else b''
                dest = os.path.join(outdir, path + fn)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                open(dest, 'wb').write(data)

    os.makedirs(outdir, exist_ok=True)
    walk(d[root_start:data_start], "")
    return listing


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit("usage: fat12_extract.py <image.img> <outdir>")
    for kind, name, size, stamp in extract(sys.argv[1], sys.argv[2]):
        print("%s %-42s %9d  %s" % (kind, name, size, stamp))
