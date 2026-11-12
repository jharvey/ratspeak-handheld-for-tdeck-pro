#!/usr/bin/env python3
"""Make offline map tiles for the T-Deck Pro (1-bit e-ink) and lay them out for the SD card.

Output: <out>/ratdeckpro/maps/<z>/<x>/<y>.rmt  -> copy the "ratdeckpro" folder to the card root.
Tile file: b"RMT1" + uint16 LE 256 + uint16 LE 256 + 8192 bytes (row-major, MSB first, 1 = black).

Sources (pick one):
  --mbtiles FILE      raster MBTiles (PNG/JPEG tiles), e.g. exported from QGIS or MOBAC.
  --url TEMPLATE      a tile server you are allowed to bulk-download from, e.g.
                      "https://tiles.example.com/{z}/{x}/{y}.png". Needs --user-agent.
                      Do NOT point this at tile.openstreetmap.org: its usage policy forbids
                      bulk downloading. Use your own server, a provider whose terms allow
                      offline use, or an MBTiles export.
  --test              synthetic grid tiles labelled with z/x/y (no Pillow needed) to try
                      the map screen without real map data.

  --overlay-url T     a second layer composited on top before the 1-bit step, e.g. Canada
                      (NRCan CBMT, Open Government Licence - Canada; credit "Contains
                      information licensed under the Open Government Licence - Canada"):
                      --url .../BaseMaps/CBMT_CBCT_GEOM_3857/MapServer/tile/{z}/{y}/{x}
                      --overlay-url .../BaseMaps/CBMT_TXT_3857/MapServer/tile/{z}/{y}/{x}
                      (host maps-cartes.services.geo.ca/server2_serveur2/rest/services)
                      with --threshold 200 (its roads are light grey).

Area: --bbox MINLON,MINLAT,MAXLON,MAXLAT  or  --center LAT,LON --radius-km R
Zoom: --zoom 12-16 (inclusive range) or a single level.
Start view: maps/home.txt = "lat lon zoom", used when the device has no GPS fix or saved
position (--home LAT,LON,ZOOM; else the first run's area centre at its lowest zoom).

Conversion (Pillow, `pip install pillow`): grey -> auto contrast -> Floyd-Steinberg dither.
--threshold N (0-255) instead gives clean black/white without dithering (good for line maps).
--invert flips black/white before saving.

Examples:
  python make_tiles.py --test --center 43.6532,-79.3832 --radius-km 3 --zoom 12-16 --out sdcard
  python make_tiles.py --mbtiles city.mbtiles --bbox -79.45,43.62,-79.33,43.70 --zoom 12-16 --out sdcard
"""
import argparse
import math
import os
import sqlite3
import struct
import sys
import time
import urllib.request

TILE = 256
HEADER = b"RMT1" + struct.pack("<HH", TILE, TILE)


def lonlat_to_tile(lon, lat, z):
    lat = max(min(lat, 85.0511), -85.0511)
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    r = math.radians(lat)
    y = int((1.0 - math.log(math.tan(r) + 1.0 / math.cos(r)) / math.pi) / 2.0 * n)
    return min(max(x, 0), n - 1), min(max(y, 0), n - 1)


def tile_range(bbox, z):
    minlon, minlat, maxlon, maxlat = bbox
    x0, y1 = lonlat_to_tile(minlon, minlat, z)
    x1, y0 = lonlat_to_tile(maxlon, maxlat, z)
    return range(min(x0, x1), max(x0, x1) + 1), range(min(y0, y1), max(y0, y1) + 1)


def parse_zoom(s):
    if "-" in s:
        a, b = s.split("-", 1)
        return list(range(int(a), int(b) + 1))
    return [int(s)]


def bbox_from_center(lat, lon, km):
    dlat = km / 111.32
    dlon = km / (111.32 * max(math.cos(math.radians(lat)), 0.01))
    return (lon - dlon, lat - dlat, lon + dlon, lat + dlat)


# ---- 1-bit packing ----------------------------------------------------------------------
def pack_bits(black_rows):
    """black_rows: 256 rows of 256 bools -> 8192 bytes, MSB first."""
    out = bytearray(TILE * TILE // 8)
    i = 0
    for row in black_rows:
        for bx in range(0, TILE, 8):
            b = 0
            for k in range(8):
                if row[bx + k]:
                    b |= 0x80 >> k
            out[i] = b
            i += 1
    return bytes(out)


def image_to_bits(data, threshold, invert, overlay=None):
    from io import BytesIO
    try:
        from PIL import Image, ImageOps
    except ImportError:
        sys.exit("Pillow is needed for real map tiles: pip install pillow")
    img = Image.open(BytesIO(data))
    if overlay:                              # e.g. a transparent label layer on top
        base = img.convert("RGBA")
        top = Image.open(BytesIO(overlay)).convert("RGBA")
        if top.size != base.size:
            top = top.resize(base.size)
        base.alpha_composite(top)
        img = base
    img = img.convert("L")
    if img.size != (TILE, TILE):
        img = img.resize((TILE, TILE))
    img = ImageOps.autocontrast(img, cutoff=1)
    if threshold is not None:
        bw = img.point(lambda v: 255 if v >= threshold else 0).convert("1", dither=Image.Dither.NONE)
    else:
        bw = img.convert("1")  # Floyd-Steinberg
    px = bw.load()
    rows = [[(px[x, y] == 0) != invert for x in range(TILE)] for y in range(TILE)]
    return pack_bits(rows)


# ---- synthetic test tiles ---------------------------------------------------------------
FONT = {  # 3x5 digits
    "0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111",
    "4": "101101111001001", "5": "111100111001111", "6": "111100111101111", "7": "111001001001001",
    "8": "111101111101111", "9": "111101111001111", "/": "001001010100100", "z": "000111001010111",
}


def test_tile(z, x, y):
    rows = [[False] * TILE for _ in range(TILE)]
    for i in range(TILE):
        for t in (0, 1):  # 2 px border
            rows[t][i] = rows[TILE - 1 - t][i] = rows[i][t] = rows[i][TILE - 1 - t] = True
    for g in range(32, TILE, 32):  # light grid
        for i in range(0, TILE, 2):
            rows[g][i] = rows[i][g] = True
    # a diagonal "road" that continues across tiles (world-pixel based)
    for i in range(TILE):
        wx, wy = x * TILE + i, y * TILE
        for j in range(TILE):
            if ((wx + wy + j) % 97) < 3:
                rows[j][i] = True
    label = "z%d/%d/%d" % (z, x, y)
    sx, sy, scale = 10, 10, 3
    for ch in label:
        glyph = FONT.get(ch)
        if glyph:
            for gy in range(5):
                for gx in range(3):
                    if glyph[gy * 3 + gx] == "1":
                        for dy in range(scale):
                            for dx in range(scale):
                                yy, xx = sy + gy * scale + dy, sx + gx * scale + dx
                                if xx < TILE and yy < TILE:
                                    rows[yy][xx] = True
        sx += 4 * scale
    return pack_bits(rows)


# ---- sources ----------------------------------------------------------------------------
class MBTiles:
    def __init__(self, path):
        self.db = sqlite3.connect(path)

    def get(self, z, x, y):
        tms_y = (1 << z) - 1 - y  # MBTiles rows are TMS (flipped)
        row = self.db.execute("SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?",
                              (z, x, tms_y)).fetchone()
        return row[0] if row else None


class UrlSource:
    def __init__(self, template, agent, delay):
        if "tile.openstreetmap.org" in template:
            sys.exit("Refusing tile.openstreetmap.org: its usage policy forbids bulk downloads.")
        self.template, self.agent, self.delay, self.last = template, agent, delay, 0.0

    def get(self, z, x, y):
        wait = self.delay - (time.time() - self.last)
        if wait > 0:
            time.sleep(wait)
        self.last = time.time()
        req = urllib.request.Request(self.template.format(z=z, x=x, y=y), headers={"User-Agent": self.agent})
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                return r.read()
        except Exception as e:  # missing tile or network error: skip it
            print("  skip %d/%d/%d: %s" % (z, x, y, e))
            return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--mbtiles")
    src.add_argument("--url")
    src.add_argument("--test", action="store_true")
    ap.add_argument("--overlay-url", help="second tile layer drawn on top (e.g. labels), same {z}/{x}/{y}")
    ap.add_argument("--bbox")
    ap.add_argument("--bboxes", help="file with one MINLON,MINLAT,MAXLON,MAXLAT per line (e.g. a road "
                                     "corridor); tiles of all boxes, each once")
    ap.add_argument("--center")
    ap.add_argument("--radius-km", type=float, default=2.0)
    ap.add_argument("--zoom", default="12-16")
    ap.add_argument("--out", default="sdcard")
    ap.add_argument("--threshold", type=int)
    ap.add_argument("--invert", action="store_true")
    ap.add_argument("--user-agent")
    ap.add_argument("--delay", type=float, default=1.0, help="seconds between URL downloads")
    ap.add_argument("--max-tiles", type=int, default=5000)
    ap.add_argument("--overwrite", action="store_true")
    ap.add_argument("--home", help="LAT,LON,ZOOM start view when the device has no position "
                    "(maps/home.txt; default: centre of this area at its lowest zoom, "
                    "written only if the card has no home.txt yet)")
    ap.add_argument("--home-only", action="store_true", help="only write home.txt, no tiles")
    a = ap.parse_args()

    boxes = None
    if a.bboxes:
        boxes = [tuple(float(v) for v in ln.split(",")) for ln in open(a.bboxes) if ln.strip() and ln[0] != "#"]
        bbox = (min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes), max(b[3] for b in boxes))
    elif a.bbox:
        bbox = tuple(float(v) for v in a.bbox.split(","))
    elif a.center:
        lat, lon = (float(v) for v in a.center.split(","))
        bbox = bbox_from_center(lat, lon, a.radius_km)
    else:
        sys.exit("give --bbox, --bboxes or --center")
    zooms = parse_zoom(a.zoom)
    root = os.path.join(a.out, "ratdeckpro", "maps")
    home = os.path.join(root, "home.txt")
    if a.home or not os.path.exists(home):
        if a.home:
            hlat, hlon, hz = a.home.split(",")
        else:
            hlat, hlon, hz = (bbox[1] + bbox[3]) / 2, (bbox[0] + bbox[2]) / 2, min(zooms)
        os.makedirs(root, exist_ok=True)
        with open(home, "w", newline="\n") as f:
            f.write("%.5f %.5f %d\n" % (float(hlat), float(hlon), int(hz)))
        print("start view (no GPS / saved position): %s" % open(home).read().strip())
    if a.home_only:
        return
    jobs = []
    for z in zooms:
        if boxes:
            seen = set()
            for b in boxes:
                xs, ys = tile_range(b, z)
                seen |= {(x, y) for x in xs for y in ys}
            jobs += [(z, x, y) for x, y in sorted(seen)]
        else:
            xs, ys = tile_range(bbox, z)
            jobs += [(z, x, y) for x in xs for y in ys]
    print("%d tiles over zoom %s (%.1f MB on the card)" % (len(jobs), a.zoom, len(jobs) * (8200 / 1e6)))
    if len(jobs) > a.max_tiles:
        sys.exit("more than --max-tiles %d; shrink the area or zoom range" % a.max_tiles)

    source = None
    if a.mbtiles:
        source = MBTiles(a.mbtiles)
    elif a.url:
        if not a.user_agent:
            sys.exit("--url needs --user-agent (identify your app to the tile server)")
        source = UrlSource(a.url, a.user_agent, a.delay)
    overlay = UrlSource(a.overlay_url, a.user_agent, a.delay) if a.overlay_url else None

    done = skipped = 0
    for z, x, y in jobs:
        path = os.path.join(root, str(z), str(x), "%d.rmt" % y)
        if os.path.exists(path) and not a.overwrite:
            skipped += 1
            continue
        if a.test:
            bits = test_tile(z, x, y)
        else:
            data = source.get(z, x, y)
            if not data:
                continue
            over = overlay.get(z, x, y) if overlay else None
            bits = image_to_bits(data, a.threshold, a.invert, over)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(HEADER + bits)
        done += 1
        if done % 100 == 0:
            print("  %d / %d" % (done, len(jobs)))
    print("wrote %d tiles (%d already there) under %s" % (done, skipped, root))
    print("Copy the folder %s to the SD card root." % os.path.join(a.out, "ratdeckpro"))


if __name__ == "__main__":
    main()
