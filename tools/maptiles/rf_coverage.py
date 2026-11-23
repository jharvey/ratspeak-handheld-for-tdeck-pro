#!/usr/bin/env python3
"""Radio coverage overlay for the Atlas: where a site has line of sight, from the USGS 3DEP
elevation already on the NAS (M:/ratspeak-maps/dem/1, 1 arc-second, about 30 m).

    python rf_coverage.py --site 35.28330,-86.37000 --height 10 --radius 15 \
        --zooms 10-14 --out <folder>          (then copy <folder>/maps to the SD card's ratdeckpro/)

For each direction (one ray per ~30 m of the circle at --radius) the terrain is followed out
from the site; a point is covered when the line from the antenna (--height above ground) to
a receiver --rx-height above ground there clears every bit of terrain in between, with the
earth curving at 4/3 of its radius (normal radio refraction). Not a full propagation model
(no Longley-Rice / ITM, no Fresnel zone, no foliage): LoRa often reaches a bit past line of
sight by diffraction and less far through dense woods. Good for "which hills see the
repeater".

Output: header-less 1-bit tiles (256 x 256, 8192 bytes, bit 1 = black, MSB first) in
<out>/maps/coverage/<z>/<x>/<y>.rmt: a dot pattern where covered, nothing elsewhere; tiles
with no coverage are not written. On the unit: Atlas O, row 7 "Radio coverage".
Needs numpy and tifffile (on VM100: ~/mapvenv/bin/python3, MAPROOT=/mnt/maps/ratspeak-maps).
"""
import argparse, glob, math, os, sys

import numpy as np
import tifffile

RE = 6371000.0
K = 4.0 / 3.0


def dem_root():
    root = (os.environ.get("MAPROOT") or "M:/ratspeak-maps").rstrip("/\\")
    return os.path.join(root, "dem", "1")


class Dem:
    """USGS 1 arc-second tiles USGS_1_n<lat>w<lon>.tif (the name is the north-west corner)."""

    def __init__(self, folder):
        self.folder = folder
        self.cache = {}

    def tile(self, lat_n, lon_w):
        key = (lat_n, lon_w)
        if key not in self.cache:
            names = glob.glob(os.path.join(self.folder, "USGS_1_n%02dw%03d*.tif" % (lat_n, lon_w)))
            if not names:
                self.cache[key] = None
            else:
                with tifffile.TiffFile(names[0]) as t:
                    page = t.pages[0]
                    a = page.asarray().astype(np.float32)
                    tie = page.tags["ModelTiepointTag"].value      # (i, j, k, x, y, z)
                    scale = page.tags["ModelPixelScaleTag"].value  # (sx, sy, sz)
                a[a < -1000] = 0.0                                 # no-data
                self.cache[key] = (a, tie[3], tie[4], scale[0], scale[1])
        return self.cache[key]

    def height(self, lat, lon):
        """Ground height in metres (nearest sample) for arrays of lat / lon. 0 where no data."""
        lat = np.asarray(lat, dtype=np.float64)
        lon = np.asarray(lon, dtype=np.float64)
        out = np.zeros(lat.shape, dtype=np.float32)
        lat_n = np.floor(lat).astype(int) + 1
        lon_w = (-np.floor(lon)).astype(int)
        for ln, lw in set(zip(lat_n.ravel().tolist(), lon_w.ravel().tolist())):
            t = self.tile(ln, lw)
            if t is None:
                continue
            a, x0, y0, sx, sy = t
            m = (lat_n == ln) & (lon_w == lw)
            col = np.clip(((lon[m] - x0) / sx).astype(int), 0, a.shape[1] - 1)
            row = np.clip(((y0 - lat[m]) / sy).astype(int), 0, a.shape[0] - 1)
            out[m] = a[row, col]
        return out


def dest(lat, lon, az, d):
    """Points d metres from (lat, lon) in direction az (radians from north), arrays."""
    la1, lo1 = math.radians(lat), math.radians(lon)
    dr = d / RE
    la2 = np.arcsin(np.sin(la1) * np.cos(dr) + np.cos(la1) * np.sin(dr) * np.cos(az))
    lo2 = lo1 + np.arctan2(np.sin(az) * np.sin(dr) * np.cos(la1), np.cos(dr) - np.sin(la1) * np.sin(la2))
    return np.degrees(la2), np.degrees(lo2)


def viewshed(dem, lat, lon, h_ant, h_rx, radius, step):
    n_az = max(360, int(2 * math.pi * radius / step))
    n_d = int(radius / step)
    az = np.linspace(0, 2 * math.pi, n_az, endpoint=False)
    d = (np.arange(n_d) + 1) * step
    AZ, D = np.meshgrid(az, d, indexing="ij")
    la, lo = dest(lat, lon, AZ, D)
    g = dem.height(la, lo)
    hs = float(dem.height(np.array([lat]), np.array([lon]))[0]) + h_ant
    drop = D * D / (2 * K * RE)
    terrain = (g - drop - hs) / D                        # elevation angle of the ground
    target = (g + h_rx - drop - hs) / D                  # ... of a receiver there
    blocked = np.maximum.accumulate(terrain, axis=1)
    prev = np.concatenate([np.full((n_az, 1), -np.inf), blocked[:, :-1]], axis=1)
    vis = target >= prev
    return vis, n_az, step, hs, float(np.count_nonzero(g == 0)) / g.size


def tile_range(lat, lon, radius, z):
    n = 1 << z
    dlat = math.degrees(radius / RE)
    dlon = dlat / max(0.01, math.cos(math.radians(lat)))
    def xy(la, lo):
        x = (lo + 180) / 360 * n
        r = math.radians(la)
        y = (1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2 * n
        return x, y
    x0, y0 = xy(lat + dlat, lon - dlon)
    x1, y1 = xy(lat - dlat, lon + dlon)
    return range(int(x0), int(x1) + 1), range(int(y0), int(y1) + 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--site", required=True, help="lat,lon of the antenna")
    ap.add_argument("--height", type=float, default=2.0, help="antenna height above ground, m (2)")
    ap.add_argument("--rx-height", type=float, default=1.5, help="receiver height above ground, m (1.5)")
    ap.add_argument("--radius", type=float, default=10.0, help="how far, km (10)")
    ap.add_argument("--step", type=float, default=30.0, help="sample spacing, m (30, the DEM)")
    ap.add_argument("--zooms", default="10-14", help="tile zooms, e.g. 10-14")
    ap.add_argument("--out", required=True, help="folder; tiles go to <out>/maps/coverage/<z>/<x>/<y>.rmt")
    ap.add_argument("--dem", default=dem_root(), help="folder of USGS_1_*.tif")
    a = ap.parse_args()
    lat, lon = (float(v) for v in a.site.split(","))
    radius = a.radius * 1000.0
    dem = Dem(a.dem)
    vis, n_az, step, hs, nodata = viewshed(dem, lat, lon, a.height, a.rx_height, radius, a.step)
    print("site %.5f,%.5f antenna %.0f m above sea level, %d rays x %d samples, %.0f%% covered, %.0f%% no DEM"
          % (lat, lon, hs, n_az, vis.shape[1], 100.0 * vis.mean(), 100.0 * nodata), flush=True)
    z0, z1 = (int(v) for v in a.zooms.split("-")) if "-" in a.zooms else (int(a.zooms),) * 2
    py, px = np.mgrid[0:256, 0:256]
    dots = ((px % 4 == 0) & (py % 4 == 0)) | ((px % 4 == 2) & (py % 4 == 2))
    written = 0
    la1 = math.radians(lat)
    for z in range(z0, z1 + 1):
        n = 1 << z
        xs, ys = tile_range(lat, lon, radius, z)
        for tx in xs:
            for ty in ys:
                gx = (tx + (px + 0.5) / 256.0) / n * 360.0 - 180.0
                gy = np.degrees(np.arctan(np.sinh(math.pi * (1 - 2 * (ty + (py + 0.5) / 256.0) / n))))
                la2, lo2 = np.radians(gy), np.radians(gx)
                dlo = lo2 - math.radians(lon)
                # distance and bearing from the site
                h = np.sin((la2 - la1) / 2) ** 2 + math.cos(la1) * np.cos(la2) * np.sin(dlo / 2) ** 2
                dist = 2 * RE * np.arcsin(np.sqrt(np.clip(h, 0, 1)))
                brg = np.arctan2(np.sin(dlo) * np.cos(la2), math.cos(la1) * np.sin(la2) - math.sin(la1) * np.cos(la2) * np.cos(dlo))
                ai = np.round((brg % (2 * math.pi)) / (2 * math.pi) * n_az).astype(int) % n_az
                di = np.round(dist / step).astype(int) - 1
                inside = (di >= 0) & (di < vis.shape[1])
                cov = np.zeros((256, 256), dtype=bool)
                cov[inside] = vis[ai[inside], di[inside]]
                cov |= dist < step                      # the site itself
                bits = cov & dots
                if not bits.any():
                    continue
                d = os.path.join(a.out, "maps", "coverage", str(z), str(tx))
                os.makedirs(d, exist_ok=True)
                with open(os.path.join(d, "%d.rmt" % ty), "wb") as f:
                    f.write(np.packbits(bits, axis=1).tobytes())
                written += 1
        print("z%d: tiles so far %d" % (z, written), flush=True)
    print("done: %d tiles in %s" % (written, os.path.join(a.out, "maps", "coverage")))


if __name__ == "__main__":
    sys.exit(main())
