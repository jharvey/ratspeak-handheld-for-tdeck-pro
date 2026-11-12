#!/usr/bin/env python3
"""Pack loose map tiles (.rmt) into compressed pack files (.rmp) for the SD card.

    python make_packs.py --out M:/ratspeak-maps/packed  M:/ratspeak-maps/mapsd  M:/ratspeak-maps/world
    python make_packs.py --out D:/ --zooms 10-14 M:/ratspeak-maps/mapsd       (straight onto a card)

Each source is a folder that holds ratdeckpro/maps/<z>/<x>/<y>.rmt (or maps/..., or <z>/...);
when two sources have the same tile the first one named wins (put detailed maps before the
world base map). Output: <out>/ratdeckpro/maps/packs/<z>/<bx>_<by>.rmp, one pack per 64x64
block of tiles. Re-running rebuilds only packs older than their tiles.

Why: a loose 8 KB tile fills a whole 8 KB cluster on the FAT32 card. Coded with the
context-model coder (rmcodec.py, the default) a USGS topo tile is ~1.2 KB (6.5x smaller;
deflate gets ~2.3 KB); world base-map tiles shrink far more. The unit decodes a tile in a few
ms and keeps 160 decoded tiles in PSRAM, so panning around stays at cache speed. Coding runs
on every CPU (~0.4 s per tile per CPU).

Pack format (little-endian):
    0   "RMP2" (context coder) or "RMP1" (--codec deflate)
    4   u8 z, u8 block shift (6), u16 0
    8   u32 bx, u32 by                      block = tiles (bx<<6 .. bx<<6|63, by<<6 ..)
    16  RMP2 only: 1024 x u16 starting probabilities for the coder (trained on this pack)
    then 4096 x (u32 offset, u32 length)    index by (y & 63) * 64 + (x & 63); length 0 = no tile
    then the tiles: coded (RMP2: rmcodec; RMP1: raw deflate) or, when length == 8192, the
    8192 bitmap bytes as they are.
The unit still reads loose .rmt tiles where a pack has no tile, so packs and loose tiles mix.
"""
import argparse
import multiprocessing
import os
import random
import struct
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rmcodec  # noqa: E402

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
    """{(z, x, y): (path, mtime)}, the first source winning."""
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


def write_pack(path, z, bx, by, members, codec):
    tiles = []
    for (x, y), src in sorted(members.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        bits = bitmap(src)
        if bits is None:
            print("  skipped (not a tile): " + src, flush=True)
            continue
        tiles.append((x, y, bits))
    if not tiles:
        return 0, 0
    prior = b""
    if codec == "ctx":
        sample = [t[2] for t in tiles]
        if len(sample) > 128:
            sample = random.Random(z * 7919 + bx * 31 + by).sample(sample, 128)
        probs = rmcodec.train(sample)
        prior = struct.pack("<%dH" % rmcodec.NCTX, *probs)
    index = bytearray(INDEX)
    blobs = []
    off = HEADER + len(prior) + INDEX
    raw = 0
    for i, (x, y, bits) in enumerate(tiles):
        if codec == "ctx":
            blob = rmcodec.encode(bits, probs)
            if i == 0 and rmcodec.decode(blob, probs) != bits:
                raise RuntimeError("coder self-check failed on " + path)
        else:
            c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
            blob = c.compress(bits) + c.flush()
        if len(blob) >= TILE:
            blob = bits                       # incompressible: stored as is
        struct.pack_into("<II", index, ((y & (SIDE - 1)) * SIDE + (x & (SIDE - 1))) * 8, off, len(blob))
        blobs.append(blob)
        off += len(blob)
        raw += TILE
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write((b"RMP2" if codec == "ctx" else b"RMP1") + struct.pack("<BBHII", z, SHIFT, 0, bx, by))
        f.write(prior)
        f.write(index)
        for b in blobs:
            f.write(b)
    os.replace(tmp, path)
    return raw, off


def build(job):
    path, z, bx, by, members, codec = job
    t0 = time.time()
    raw, packed = write_pack(path, z, bx, by, members, codec)
    print("z%d %d_%d: %d tiles, %.1f -> %.2f MB (%.0f s)" % (z, bx, by, len(members), raw / 1e6,
          packed / 1e6, time.time() - t0), flush=True)
    return raw, packed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="folder that gets ratdeckpro/maps/packs/...")
    ap.add_argument("--zooms", help="e.g. 0-14 (default: every zoom found)")
    ap.add_argument("--all", action="store_true", help="rebuild packs that are newer than their tiles too")
    ap.add_argument("--codec", choices=("ctx", "deflate"), default="ctx",
                    help="ctx: context coder, ~2x smaller than deflate (default); deflate: RMP1")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="packs coded at once")
    ap.add_argument("sources", nargs="+", help="tile folders, the preferred one first")
    a = ap.parse_args()
    t0 = time.time()
    tiles = scan(a.sources, zoom_range(a.zooms))
    blocks = {}
    for (z, x, y), (p, mt) in tiles.items():
        blocks.setdefault((z, x >> SHIFT, y >> SHIFT), {})[(x, y)] = (p, mt)
    outroot = os.path.join(a.out, "ratdeckpro", "maps", "packs")
    jobs = []
    kept = 0
    for (z, bx, by) in sorted(blocks):
        members = blocks[(z, bx, by)]
        path = os.path.join(outroot, str(z), "%d_%d.rmp" % (bx, by))
        newest = max(mt for _, mt in members.values())
        if not a.all and os.path.exists(path) and os.path.getmtime(path) > newest:
            kept += 1
            continue
        jobs.append((path, z, bx, by, {k: p for k, (p, _) in members.items()}, a.codec))
    jobs.sort(key=lambda j: -len(j[4]))          # big packs first: the CPUs finish together
    if a.jobs > 1 and len(jobs) > 1:
        with multiprocessing.Pool(a.jobs) as pool:
            results = pool.map(build, jobs, chunksize=1)
    else:
        results = [build(j) for j in jobs]
    total_raw = sum(r for r, _ in results)
    total_packed = sum(p for _, p in results)
    print("%d packs built, %d up to date; %d tiles; raw %.0f MB, packed %.1f MB (%.1fx) in %.0f s"
          % (len(jobs), kept, len(tiles), total_raw / 1e6, total_packed / 1e6,
             total_raw / max(1, total_packed), time.time() - t0))


if __name__ == "__main__":
    main()
