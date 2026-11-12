#!/usr/bin/env python3
"""Copy map tiles (or any files) to a T-Deck Pro's SD card over USB, no card reader.

Uses the firmware's `sdput` console command. The unit must be unlocked (no password
prompt on screen) and have a card inserted.

    pip install pyserial
    python push_tiles.py --port COM9 --src sdcard          # sdcard/ratdeckpro/... -> card
    python push_tiles.py --port COM9 --src sdcard --zoom 3-10

Order: maps/home.txt, then zoom by zoom from the lowest, so a stopped copy still gives a
usable map. Progress is kept in <src>/pushed_<dest hash>.txt: run again to resume
(--fresh starts over). Files already listed there are skipped.

Speed: about 8-20 tiles/s (8 KB each), depending on the card.
Space: FAT32 cards with 32 KB clusters use 32 KB per 8 KB tile; --max-mb stops before
the card fills (default: stop when the device reports less than 200 MB free).
"""
import argparse
import os
import re
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("needs pyserial: pip install pyserial")


class Dev:
    def __init__(self, port):
        if "://" in port:                   # e.g. socket://127.0.0.1:5577 (simulator --serial-tcp)
            self.s = serial.serial_for_url(port, timeout=0.2)
            self.buf = b""
            return
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, 115200, 0.2
        s.dtr, s.rts = True, False          # do not reset the ESP32-S3
        s.open()
        self.s = s
        self.buf = b""

    def line(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            i = self.buf.find(b"\n")
            if i >= 0:
                ln, self.buf = self.buf[:i], self.buf[i + 1:]
                return ln.decode("utf-8", "replace").strip()
            self.buf += self.s.read(self.s.in_waiting or 1)
        return None

    def cmd(self, text):
        self.s.write((text + "\n").encode())

    def wait(self, pattern, timeout):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            ln = self.line(end - time.time())
            if ln is None:
                break
            m = rx.search(ln)
            if m:
                return m, ln
        return None, None

    def drain(self, secs=0.5):
        end = time.time() + secs
        while time.time() < end:
            self.s.read(4096)
        self.buf = b""


def sd_free_mb(dev):
    dev.cmd("sdinfo")
    m, _ = dev.wait(r"\[SD\] (?:OK|mounted OK) (\d+)/(\d+) MB", 8)
    if not m:
        return None
    return int(m.group(2)) - int(m.group(1))


def put(dev, rel, data):
    crc = zlib.crc32(data) & 0xFFFFFFFF
    for attempt in range(4):
        dev.cmd("sdput %d %08x %s" % (len(data), crc, rel))
        m, ln = dev.wait(r"\[SDPUT\] (ready|err.*)|\[CMD\] locked", 6)
        if not m:
            dev.drain(); continue
        if "locked" in ln:
            sys.exit("the unit is locked: unlock it on the screen first")
        if m.group(1) != "ready":
            if "usage" in ln or "no SD" in ln:
                sys.exit(ln)
            dev.drain(); continue
        dev.s.write(data)
        m, ln = dev.wait(r"\[SDPUT\] (ok|err.*)", 10)
        if m and m.group(1) == "ok":
            return True
        dev.drain(1.0)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--src", default="sdcard", help="folder that holds ratdeckpro/")
    ap.add_argument("--zoom", help="only these zoom levels, e.g. 3-10")
    ap.add_argument("--min-free-mb", type=int, default=200)
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--keep-header", action="store_true",
                    help="send .rmt files with their 8-byte header (firmware before 2026-11-12 needs it); "
                         "default: header stripped, 8192 bytes = exactly two 4 KB clusters")
    a = ap.parse_args()

    root = os.path.join(a.src, "ratdeckpro")
    if not os.path.isdir(root):
        sys.exit("no %s" % root)
    zmin, zmax = 0, 99
    if a.zoom:
        p = a.zoom.split("-")
        zmin, zmax = int(p[0]), int(p[-1])

    files = []
    for dp, _, fns in os.walk(root):
        for fn in fns:
            full = os.path.join(dp, fn)
            rel = "/" + os.path.relpath(full, root).replace(os.sep, "/")
            m = re.match(r"/maps/(\d+)/(\d+)/(\d+)\.rmt$", rel)
            if m:
                z = int(m.group(1))
                if not (zmin <= z <= zmax):
                    continue
                key = (1, z, int(m.group(2)), int(m.group(3)))
            elif rel == "/maps/home.txt":
                key = (0, 0, 0, 0)
            else:
                continue
            files.append((key, rel, full))
    files.sort()

    dev = Dev(a.port)
    dev.drain(1.0)
    dev.cmd("id")
    m, _ = dev.wait(r"\[ID\] dest=([0-9a-f]{32})", 8)
    if not m:
        sys.exit("no answer to 'id' on %s" % a.port)
    dest = m.group(1)
    donefile = os.path.join(a.src, "pushed_%s.txt" % dest)
    done = set()
    if os.path.exists(donefile) and not a.fresh:
        done = set(open(donefile).read().split())
    todo = [f for f in files if f[1] not in done]
    free = sd_free_mb(dev)
    print("%s dest %s: %d files, %d to send, card free %s MB" % (a.port, dest, len(files), len(todo),
                                                               free if free is not None else "?"))
    if free is None:
        sys.exit("no SD card answer (sdinfo)")

    log = open(donefile, "a")
    t0 = time.time()
    sent = fails = 0
    for i, (_, rel, full) in enumerate(todo):
        if i % 500 == 0 and i:
            free = sd_free_mb(dev)
            if free is not None and free < a.min_free_mb:
                print("stopping: card has %d MB free (< %d)" % (free, a.min_free_mb))
                break
        data = open(full, "rb").read()
        if not a.keep_header and rel.endswith(".rmt") and len(data) == 8200 and data[:4] == b"RMT1":
            data = data[8:]
        if put(dev, rel, data):
            sent += 1
            log.write(rel + "\n")
            if sent % 200 == 0:
                log.flush()
                rate = sent / (time.time() - t0)
                print("  %d / %d  (%.1f files/s, %s)" % (sent, len(todo), rate, rel), flush=True)
        else:
            fails += 1
            print("  failed: %s" % rel, flush=True)
            if fails > 20:
                sys.exit("too many failures")
    log.close()
    print("sent %d files in %.0f s, %d failed" % (sent, time.time() - t0, fails))


if __name__ == "__main__":
    main()
