#!/usr/bin/env python3
"""Render a 1-bit world overview map (zoom 0-7) from Natural Earth data, as .rmt tiles.

Natural Earth is public domain (naturalearthdata.com), so no tile-server terms apply.
Drawn for the e-ink panel: coastlines and lakes 1 px, country borders 2 px, state /
province borders 1 px dotted (where Natural Earth shows them at that zoom), names with a
white halo: countries (capitals), states / provinces from zoom 4, big cities from zoom 4.

Input: a folder with these GeoJSON files (github.com/nvkelso/natural-earth-vector, geojson/):
  ne_10m_coastline, ne_10m_admin_0_boundary_lines_land, ne_10m_admin_1_states_provinces_lines,
  ne_50m_admin_0_countries, ne_50m_admin_1_states_provinces, ne_10m_lakes,
  ne_10m_populated_places_simple

    pip install pillow
    python render_world.py --ne ne --zoom 0-7 --out world
    python push_tiles.py --port COM9 --src world

Tiles: z0-6 = 5,461, z7 = 16,384 (all written, ocean tiles too, so the map never shows
"no tiles" at these zooms). Larger zooms come from make_tiles.py (e.g. USGS topo).
"""
import argparse
import json
import math
import os
import sys
from collections import defaultdict

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_tiles import HEADER  # noqa: E402

T = 256
INVERT = bytes(255 - i for i in range(256))


def load(folder, name):
    with open(os.path.join(folder, name + ".geojson"), encoding="utf-8") as f:
        return json.load(f)["features"]


def lines_of(geom):
    t, c = geom["type"], geom["coordinates"]
    if t == "LineString":
        return [c]
    if t == "MultiLineString":
        return c
    if t == "Polygon":
        return c
    if t == "MultiPolygon":
        return [ring for poly in c for ring in poly]
    return []


def proj(lon, lat, z):
    lat = max(min(lat, 85.0511), -85.0511)
    s = T * (1 << z)
    x = (lon + 180.0) / 360.0 * s
    r = math.radians(lat)
    y = (1.0 - math.log(math.tan(r) + 1.0 / math.cos(r)) / math.pi) / 2.0 * s
    return x, y


def font(size, bold=True):
    for name in (("arialbd.ttf", "DejaVuSans-Bold.ttf") if bold else ("arial.ttf", "DejaVuSans.ttf")):
        for d in ("", os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts")):
            try:
                return ImageFont.truetype(os.path.join(d, name) if d else name, size)
            except OSError:
                pass
    return ImageFont.load_default()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ne", required=True, help="folder with the Natural Earth GeoJSON files")
    ap.add_argument("--zoom", default="0-7")
    ap.add_argument("--out", default="world")
    ap.add_argument("--home", help="LAT,LON,ZOOM start view written to maps/home.txt")
    a = ap.parse_args()
    z0, z1 = (int(v) for v in (a.zoom.split("-") + [a.zoom])[:2])

    layers = [  # (features, style, include(props, z))
        (load(a.ne, "ne_10m_coastline"), "coast", lambda p, z: (p.get("min_zoom") or 0) <= z + 1),
        (load(a.ne, "ne_10m_lakes"), "lake", lambda p, z: (p.get("scalerank") or 0) <= z - 1),
        (load(a.ne, "ne_10m_admin_0_boundary_lines_land"), "country", lambda p, z: True),
        (load(a.ne, "ne_10m_admin_1_states_provinces_lines"), "state",
         lambda p, z: z >= 3 and (p.get("MIN_ZOOM") or 99) <= z + 1.5),
    ]
    countries = load(a.ne, "ne_50m_admin_0_countries")
    states = load(a.ne, "ne_50m_admin_1_states_provinces")
    places = load(a.ne, "ne_10m_populated_places_simple")

    root = os.path.join(a.out, "ratdeckpro", "maps")
    total = 0
    for z in range(z0, z1 + 1):
        n = 1 << z
        segs = defaultdict(list)          # (tx, ty) -> [(style, x0, y0, x1, y1)] in world px
        for feats, style, inc in layers:
            for f in feats:
                if not f.get("geometry") or not inc(f["properties"], z):
                    continue
                for line in lines_of(f["geometry"]):
                    pts = [proj(p[0], p[1], z) for p in line]
                    for (xa, ya), (xb, yb) in zip(pts, pts[1:]):
                        if abs(xa - xb) > T * n / 2:       # dateline jump
                            continue
                        tx0, tx1 = sorted((int(xa // T), int(xb // T)))
                        ty0, ty1 = sorted((int(ya // T), int(yb // T)))
                        for tx in range(max(tx0, 0), min(tx1, n - 1) + 1):
                            for ty in range(max(ty0, 0), min(ty1, n - 1) + 1):
                                segs[(tx, ty)].append((style, xa, ya, xb, yb))

        # Labels: (priority, world x, y, text, font, dot). Greedy placement, no overlaps.
        cand = []
        fc = font(14 if z >= 4 else 12)
        fs = font(11)
        fp = font(11, bold=False)
        for f in countries:
            p = f["properties"]
            if p.get("MIN_LABEL", 99) <= z + 0.5 and z >= 2:
                name = p.get("NAME") if z >= 4 else (p.get("ABBREV") or p.get("NAME"))
                x, y = proj(p["LABEL_X"], p["LABEL_Y"], z)
                cand.append((p.get("LABELRANK", 9), x, y, name.upper() if z >= 3 else name, fc, False))
        if z >= 4:
            for f in states:
                p = f["properties"]
                if (p.get("min_label") or 99) <= z and p.get("latitude") is not None:
                    x, y = proj(p["longitude"], p["latitude"], z)
                    label = p["name"] if z >= 5 else (p.get("postal") or p["name"])
                    cand.append((10 + (p.get("labelrank") or 9), x, y, label, fs, False))
            for f in places:
                p = f["properties"]
                if (p.get("min_zoom") or 99) <= z - 0.5:
                    x, y = proj(p["longitude"], p["latitude"], z)
                    cand.append((20 + (p.get("scalerank") or 9), x, y, p["name"], fp, True))
        cand.sort(key=lambda c: c[0])
        placed, labels = [], defaultdict(list)
        probe = ImageDraw.Draw(Image.new("1", (1, 1)))
        for pr, x, y, text, fnt, dot in cand:
            l, t_, r, b = probe.textbbox((0, 0), text, font=fnt, stroke_width=2)
            w, h = r - l, b - t_
            lx, ly = (x + 4, y - h / 2) if dot else (x - w / 2, y - h / 2)
            box = (lx - 1, ly - 1, lx + w + 1, ly + h + 1)
            if any(not (box[2] < q[0] or box[0] > q[2] or box[3] < q[1] or box[1] > q[3]) for q in placed):
                continue
            placed.append(box)
            for tx in range(max(int(box[0] // T), 0), min(int(box[2] // T), n - 1) + 1):
                for ty in range(max(int(box[1] // T), 0), min(int(box[3] // T), n - 1) + 1):
                    labels[(tx, ty)].append((x, y, lx - l, ly - t_, text, fnt, dot))

        count = 0
        for tx in range(n):
            for ty in range(n):
                im = Image.new("1", (T, T), 1)
                d = ImageDraw.Draw(im)
                ox, oy = tx * T, ty * T
                for style, xa, ya, xb, yb in segs.get((tx, ty), ()):
                    a_, b_ = (xa - ox, ya - oy), (xb - ox, yb - oy)
                    if style == "country":
                        d.line([a_, b_], fill=0, width=2 if z >= 3 else 1)
                    elif style == "state":
                        # dotted: draw every other 3 px stretch along the segment
                        L = math.hypot(b_[0] - a_[0], b_[1] - a_[1])
                        steps = max(int(L // 3), 1)
                        for k in range(0, steps, 2):
                            t0, t1 = k / steps, min((k + 1) / steps, 1)
                            d.line([(a_[0] + (b_[0] - a_[0]) * t0, a_[1] + (b_[1] - a_[1]) * t0),
                                    (a_[0] + (b_[0] - a_[0]) * t1, a_[1] + (b_[1] - a_[1]) * t1)], fill=0)
                    else:
                        d.line([a_, b_], fill=0, width=1)
                for x, y, lx, ly, text, fnt, dot in labels.get((tx, ty), ()):
                    if dot:
                        d.ellipse([x - ox - 2, y - oy - 2, x - ox + 2, y - oy + 2], fill=0)
                    d.text((lx - ox, ly - oy), text, font=fnt, fill=0, stroke_width=2, stroke_fill=1)
                # PIL "1" packs MSB first with 1 = white; .rmt wants 1 = black.
                bits = im.tobytes().translate(INVERT)
                path = os.path.join(root, str(z), str(tx), "%d.rmt" % ty)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(HEADER + bits)
                count += 1
        total += count
        print("z%d: %d tiles, %d labels" % (z, count, len(placed)), flush=True)
    if a.home:
        hlat, hlon, hz = a.home.split(",")
        with open(os.path.join(root, "home.txt"), "w", newline="\n") as f:
            f.write("%.5f %.5f %d\n" % (float(hlat), float(hlon), int(hz)))
    print("wrote %d tiles under %s" % (total, root))


if __name__ == "__main__":
    main()
