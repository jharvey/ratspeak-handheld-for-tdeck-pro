#!/usr/bin/env python3
"""Copy files (any size) to a T-Deck Pro's SD card over USB - e.g. library texts.

    pip install pyserial
    python push_files.py --port COM5 notes\\*.txt                 # -> /ratdeckpro/library/
    python push_files.py --port COM5 --dest /library ghostnet.txt
    python push_files.py --port socket://127.0.0.1:5591 a.txt    # simulator (--serial-tcp)

Uses the firmware's `sdput` command in pieces of 8 KB (the first piece writes the file,
the next ones append at their offset; the unit checks the length before every append).
The unit must be unlocked and have a card. The library viewer (MENU -> Library) reads
plain text: .txt / .md / .csv are fine; PDFs need a text version first.
"""
import argparse
import glob
import os
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "maptiles"))
from push_tiles import Dev  # noqa: E402

PIECE = 8192


def put_piece(dev, rel, data, offset):
    crc = zlib.crc32(data) & 0xFFFFFFFF
    cmd = "sdput %d %08x %s" % (len(data), crc, rel) + (" %d" % offset if offset else "")
    for _ in range(4):
        dev.cmd(cmd)
        m, ln = dev.wait(r"\[SDPUT\] (ready|err.*)|\[CMD\] locked", 6)
        if m and "locked" in ln:
            sys.exit("the unit is locked: unlock it first")
        if not m or m.group(1) != "ready":
            if ln and "offset" in ln:
                return False                 # file length differs: caller restarts the file
            dev.drain()
            continue
        dev.s.write(data)
        m, ln = dev.wait(r"\[SDPUT\] (ok|err.*)", 15)
        if m and m.group(1) == "ok":
            return True
        dev.drain(1.0)
    return False


def push_file(dev, path, dest):
    name = os.path.basename(path).replace(" ", "_")
    rel = dest.rstrip("/") + "/" + name
    data = open(path, "rb").read()
    for attempt in range(2):
        ok = True
        for off in range(0, max(len(data), 1), PIECE):
            if not put_piece(dev, rel, data[off:off + PIECE] or b"\n", off):
                ok = False
                break
        if ok:
            print("  %s -> %s (%d bytes)" % (path, rel, len(data)), flush=True)
            return True
    print("  FAILED: " + path, flush=True)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--dest", default="/library", help="folder under /ratdeckpro (default /library)")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    paths = [p for pat in a.files for p in (glob.glob(pat) or [pat])]
    dev = Dev(a.port)
    dev.drain(1.0)
    bad = sum(0 if push_file(dev, p, a.dest) else 1 for p in paths)
    print("%d files, %d failed" % (len(paths), bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
