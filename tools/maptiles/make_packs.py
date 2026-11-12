#!/usr/bin/env python3
"""Pack loose map tiles (.rmt) into compressed pack files (.rmp) for the SD card.

    python make_packs.py --out M:/ratspeak-maps/packed  M:/ratspeak-maps/mapsd  M:/ratspeak-maps/world
    python make_packs.py --out D:/ --zooms 10-14 M:/ratspeak-maps/mapsd       (straight onto a card)

Each source is a folder that holds ratdeckpro/maps/<z>/<x>/<y>.rmt (or maps/..., or <z>/...);
when two sources have the same tile the first one named wins (put detailed maps before the
world base map). Output: <out>/ratdeckpro/maps/packs/<z>/<bx>_<by>.rmp, one pack per 64x64
block of tiles.

Why: a loose 8 KB tile fills a whole 8 KB cluster on the FAT32 card; deflated, a USGS tile is
~2-3.5 KB and a world tile ~0.1-0.4 KB, so the same card holds 2.5-4x more map (20-60x for the
world base map). The unit inflates a tile with the inflater in the ESP32-S3's ROM (~1 ms) and
keeps ~160 inflated tiles in PSRAM, so panning around stays at cache speed.

Pack format (little-endian):
    0   "RMP1"
    4   u8 z, u8 block shift (6), u16 0
    8   u32 bx, u32 by                      block = tiles (bx<<6 .. bx<<6|63, by<<6 ..)
    16  4096 x (u32 offset, u32 length)     index by (y & 63) * 64 + (x & 63); length 0 = no tile
    then the tiles: raw deflate of the 8192 bitmap bytes (no header), or the 8192 bytes
    stored as they are when length == 8192.
The unit still reads loose .rmt tiles where a pack has no tile, so packs and loose tiles mix.
"""
import argparse
import os
import struct
import sys
import time
import zlib

SHIFT = 6
SIDE = 1 << SHIFT
HEADER = 16
INDEX = SIDE * SIDE * 8
TILE = 8192


def maps_dir(src):
    for sub in ("ratdeckpro/maps", "maps", ""):
        d = os.path.join(src, sub) if sub else src
        if os.path.isdir(d) and any(n.isdigit() for n in os.listdir(d)):
            return d
    sys.exit("no tiles under " + src)


def zoom_range(text):
    if not text:
        return None
    lo, _, hi = text.partition("-")
    return set(range(int(lo), int(hi or lo) + 1))


def scan(srcs, zooms):
    """{(z, x, y): path}, the first source winning."""
    tiles = {}
    for src in srcs:
        root = maps_dir(src)
        n = 0
        for zn in os.listdir(root):
            if not zn.isdigit() or (zooms and int(zn) not in zooms):
                continue
            z = int(zn)
            for xe in os.scandir(os.path.join(root, zn)):
                if not xe.is_dir() or not xe.name.isdigit():
                    continue
                x = int(xe.name)
                for ye in os.scandir(xe.path):
                    if ye.name.endswith(".rmt") and ye.name[:-4].isdigit():
                        key = (z, x, int(ye.name[:-4]))
                        if key not in tiles:
                            tiles[key] = (ye.path, ye.stat().st_mtime)
                            n += 1
        print("%s: %d tiles" % (root, n), flush=True)
    return tiles


def bitmap(path):
    d = open(path, "rb").read()
    if len(d) == TILE + 8 and d[:4] == b"RMT1":
        return d[8:]
    if len(d) == TILE:
        return d
    return None


def write_pack(path, z, bx, by, members):
    index = bytearray(INDEX)
    blobs = []
    off = HEADER + INDEX
    raw = packed = 0
    for (x, y), src in sorted(members.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        bits = bitmap(src)
        if bits is None:
            print("  skipped (not a tile): " + src, flush=True)
            continue
        c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
        blob = c.compress(bits) + c.flush()
        if len(blob) >= TILE:
            blob = bits                       # incompressible: stored as is
        struct.pack_into("<II", index, ((y & (SIDE - 1)) * SIDE + (x & (SIDE - 1))) * 8, off, len(blob))
        blobs.append(blob)
        off += len(blob)
        raw += TILE
        packed += len(blob)
    if not blobs:
        return 0, 0
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(b"RMP1" + struct.pack("<BBHII", z, SHIFT, 0, bx, by))
        f.write(index)
        for b in blobs:
            f.write(b)
    os.replace(tmp, path)
    return raw, packed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="folder that gets ratdeckpro/maps/packs/...")
    ap.add_argument("--zooms", help="e.g. 0-14 (default: every zoom found)")
    ap.add_argument("--all", action="store_true", help="rebuild packs that are newer than their tiles too")
    ap.add_argument("sources", nargs="+", help="tile folders, the preferred one first")
    a = ap.parse_args()
    t0 = time.time()
    tiles = scan(a.sources, zoom_range(a.zooms))
    blocks = {}
    for (z, x, y), (p, mt) in tiles.items():
        blocks.setdefault((z, x >> SHIFT, y >> SHIFT), {})[(x, y)] = (p, mt)
    outroot = os.path.join(a.out, "ratdeckpro", "maps", "packs")
    total_raw = total_packed = built = kept = 0
    for (z, bx, by) in sorted(blocks):
        members = blocks[(z, bx, by)]
        path = os.path.join(outroot, str(z), "%d_%d.rmp" % (bx, by))
        newest = max(mt for _, mt in members.values())
        if not a.all and os.path.exists(path) and os.path.getmtime(path) > newest:
            kept += 1
            continue
        raw, packed = write_pack(path, z, bx, by, {k: p for k, (p, _) in members.items()})
        total_raw += raw
        total_packed += packed
        built += 1
        print("z%d %d_%d: %d tiles, %.1f -> %.1f MB" % (z, bx, by, len(members), raw / 1e6,
              (packed + HEADER + INDEX) / 1e6), flush=True)
    clusters = sum(1 for _ in tiles) * TILE
    print("%d packs built, %d up to date; %d tiles; loose on a card %.0f MB, packed %.0f MB (%.1fx) in %.0f s"
          % (built, kept, len(tiles), clusters / 1e6, (total_packed + built * (HEADER + INDEX)) / 1e6,
             total_raw / max(1, total_packed + built * (HEADER + INDEX)), time.time() - t0))


if __name__ == "__main__":
    main()
