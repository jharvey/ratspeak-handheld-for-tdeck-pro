#!/usr/bin/env python3
"""Screenshot of a unit over the WiFi remote console (relay on the build VM).

  python tools/netcon/shot.py TDP-0E34 out.png [--host debian-build] [--secs 6]

Starts the screen mirror (@M on) through the relay, collects the full frame (@HELLO +
@FB rows) and saves it as a PNG at twice the size. Then stops the mirror.
"""
import argparse
import base64
import subprocess
import sys

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("unit")
    ap.add_argument("out")
    ap.add_argument("--host", default="debian-build")
    ap.add_argument("--secs", type=float, default=6)
    a = ap.parse_args()
    r = subprocess.run(["ssh", a.host, "python3 rscon/rscon.py run %s %g @M on" % (a.unit, a.secs)],
                       capture_output=True, text=True)
    subprocess.run(["ssh", a.host, "python3 rscon/rscon.py send %s @M off" % a.unit], capture_output=True)
    W = H = 0
    fb = None
    got = 0
    for l in r.stdout.splitlines():
        l = l.strip()
        if l.startswith("@HELLO"):
            _, w, h = l.split()
            W, H = int(w), int(h)
            fb = bytearray((W // 8) * H)
        elif l.startswith("@FB") and fb is not None:
            p = l.split()
            try:
                y, d = int(p[1]), base64.b64decode(p[4])
                fb[y * (W // 8):y * (W // 8) + len(d)] = d
                got += int(p[2])
            except (ValueError, IndexError):
                pass
        elif l.startswith("ERR"):
            sys.exit(l)
    if fb is None:
        sys.exit("no frame from %s (is it connected? ssh %s python3 rscon/rscon.py list)" % (a.unit, a.host))
    im = Image.frombytes("1", (W, H), bytes(fb)).point(lambda v: 255 - v)
    im.convert("L").resize((W * 2, H * 2), Image.NEAREST).save(a.out)
    print("saved %s (%dx%d, %d rows received)" % (a.out, W, H, got))


if __name__ == "__main__":
    main()
