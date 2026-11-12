#!/usr/bin/env python3
"""Copy the map tiles from one T-Deck Pro's SD card to another's, both on USB.

Nothing is stored on the PC: each tile is read from the source unit (`sdget`) and written
to the target (`sdput`) straight away. Both units must be unlocked.

    pip install pyserial
    python copy_tiles.py --from COM9 --to COM6                 # all zoom levels
    python copy_tiles.py --from COM9 --to COM6 --zoom 3-12     # some levels

Order: maps/home.txt, then zoom by zoom from the lowest. Progress is kept in
pushed_<target dest>.txt next to this script (or --state DIR); run again to resume.
Speed: about 5-7 tiles/s (the target's SD writes are the limit).
"""
import argparse
import os
import re
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from push_tiles import Dev, put, sd_free_mb  # noqa: E402

ROOT = "/ratdeckpro"


def listdir(dev, path):
    """-> (dirs, files) of an SD folder via `sdlsa`."""
    for _ in range(3):
        try:
            return listdir_once(dev, path)
        except RuntimeError:
            dev.drain(1.0)
    raise RuntimeError("no listing for " + path)


def listdir_once(dev, path):
    dev.cmd("sdlsa " + path)
    dirs, files = [], []
    end = time.time() + 20
    while time.time() < end:
        ln = dev.line(end - time.time())
        if ln is None:
            break
        if not ln.startswith("[SDLS]"):
            continue
        if re.match(r"\[SDLS\] (\d+ entries in |cannot open)", ln):
            return dirs, files
        m = re.match(r"\[SDLS\] <dir> (.+)$", ln)
        if m:
            dirs.append(m.group(1).rsplit("/", 1)[-1]); continue
        m = re.match(r"\[SDLS\]\s+(\d+) (.+)$", ln)
        if m:
            files.append(m.group(2).rsplit("/", 1)[-1]); continue
        if re.match(r"\[SDLS\] (\d+ entries|cannot open)", ln):
            return dirs, files
    raise RuntimeError("no listing for " + path)


def get(dev, path):
    for _ in range(4):
        dev.cmd("sdget " + path)
        m, ln = dev.wait(r"\[SDGET\] (\d+) ([0-9a-f]{8})|\[SDGET\] err|\[CMD\] locked", 8)
        if m and "locked" in ln:
            sys.exit("source unit is locked: unlock it first")
        if not m or not m.group(1):
            dev.drain(); continue
        n, crc = int(m.group(1)), int(m.group(2), 16)
        data = dev.buf
        end = time.time() + 8
        while len(data) < n and time.time() < end:
            data += dev.s.read(n - len(data))
        dev.buf = data[n:]
        data = data[:n]
        if len(data) == n and (zlib.crc32(data) & 0xFFFFFFFF) == crc:
            return data
        dev.drain(1.0)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="src", required=True)
    ap.add_argument("--to", dest="dst", required=True)
    ap.add_argument("--zoom", help="only these zoom levels, e.g. 3-12")
    ap.add_argument("--min-free-mb", type=int, default=200)
    ap.add_argument("--state", default=os.path.dirname(os.path.abspath(__file__)))
    a = ap.parse_args()
    zmin, zmax = 0, 99
    if a.zoom:
        p = a.zoom.split("-")
        zmin, zmax = int(p[0]), int(p[-1])

    src, dst = Dev(a.src), Dev(a.dst)
    for d in (src, dst):
        d.drain(1.0)
    dst.cmd("id")
    m, _ = dst.wait(r"\[ID\] dest=([0-9a-f]{32})", 8)
    if not m:
        sys.exit("no answer from " + a.dst)
    donefile = os.path.join(a.state, "pushed_%s.txt" % m.group(1))
    done = set(open(donefile).read().split()) if os.path.exists(donefile) else set()
    log = open(donefile, "a")
    free = sd_free_mb(dst)
    print("target %s card free %s MB, %d files already copied" % (a.dst, free, len(done)), flush=True)

    t0, sent, fails = time.time(), 0, 0

    def copy(rel):
        nonlocal sent, fails
        if rel in done:
            return
        data = get(src, ROOT + rel)
        if data is None or not put(dst, rel, data):
            fails += 1
            print("  failed: " + rel, flush=True)
            if fails > 20:
                sys.exit("too many failures")
            return
        sent += 1
        log.write(rel + "\n")
        if sent % 200 == 0:
            log.flush()
            print("  %d copied (%.1f/s) %s" % (sent, sent / (time.time() - t0), rel), flush=True)
        if sent % 1000 == 0:
            f = sd_free_mb(dst)
            if f is not None and f < a.min_free_mb:
                sys.exit("target card has %d MB free, stopping" % f)

    _, top = listdir(src, ROOT + "/maps")
    if "home.txt" in top:
        copy("/maps/home.txt")
    zooms, _ = listdir(src, ROOT + "/maps")
    for z in sorted(int(z) for z in zooms if z.isdigit()):
        if not (zmin <= z <= zmax):
            continue
        xs, _ = listdir(src, "%s/maps/%d" % (ROOT, z))
        for x in sorted(int(x) for x in xs if x.isdigit()):
            _, ys = listdir(src, "%s/maps/%d/%d" % (ROOT, z, x))
            for y in sorted(ys):
                if re.match(r"\d+\.rmt$", y):
                    copy("/maps/%d/%d/%s" % (z, x, y))
        print("zoom %d done (%d copied so far)" % (z, sent), flush=True)
    log.close()
    print("copied %d files in %.0f s, %d failed" % (sent, time.time() - t0, fails))


if __name__ == "__main__":
    main()
