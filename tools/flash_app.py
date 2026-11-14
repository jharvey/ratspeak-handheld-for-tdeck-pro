#!/usr/bin/env python3
"""Flash the committed firmware image onto units over USB - no local build.

The image is built on the build VM (tools/vm_build.py --commit) and committed as
firmware/<env>.bin with firmware/<env>.txt (source commit, sha256). This checks the sha256,
then writes the app at 0x10000 in 256 KB pieces with the ROM loader (the esptool stub fails
on these boards), retrying each piece, highest piece first so the image only becomes valid
once the first piece lands. Settings, identity, messages and the SD card are not touched.

  python tools/flash_app.py COM5 COM9            # firmware/tdeck_pro_standalone.bin
  python tools/flash_app.py -e <env> COM3
  python tools/flash_app.py --bin other.bin COM6  # any image (no sha256 check)
"""
import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIECE = 0x40000
APP = 0x10000


def esptool_cmd():
    e = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")
    return [sys.executable, e] if os.path.isfile(e) else [sys.executable, "-m", "esptool"]


def check(env):
    fw = os.path.join(ROOT, "firmware", env + ".bin")
    info = os.path.join(ROOT, "firmware", env + ".txt")
    if not os.path.isfile(fw) or not os.path.isfile(info):
        sys.exit("no %s / .txt - build with tools/vm_build.py --commit (or git pull)" % fw)
    want = [l.split()[1] for l in open(info) if l.startswith("sha256 ")]
    got = hashlib.sha256(open(fw, "rb").read()).hexdigest()
    if not want or want[0] != got:
        sys.exit("sha256 mismatch: %s is not the image %s describes" % (fw, info))
    src = [l.strip() for l in open(info) if l.startswith("source ")]
    print("image %s (%d bytes, %s)" % (fw, os.path.getsize(fw), src[0] if src else "?"))
    return fw


def flash(port, fw):
    data = open(fw, "rb").read()
    with tempfile.TemporaryDirectory() as tmp:
        offs = list(range(0, len(data), PIECE))
        for off in reversed(offs):
            path = os.path.join(tmp, "p%07x.bin" % (APP + off))
            open(path, "wb").write(data[off:off + PIECE])
            after = "hard_reset" if off == 0 else "no_reset"
            for attempt in range(6):
                r = subprocess.run(esptool_cmd() + ["--chip", "esp32s3", "--port", port, "--baud", "115200",
                                                    "--before", "default_reset", "--after", after, "--no-stub",
                                                    "write_flash", "0x%x" % (APP + off), path],
                                   capture_output=True, text=True)
                if "Hash of data verified" in r.stdout:
                    print("%s 0x%x ok" % (port, APP + off))
                    break
                time.sleep(3)
            else:
                print("%s FAILED at 0x%x" % (port, APP + off))
                return False
    print("%s flashed OK" % port)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ports", nargs="+")
    ap.add_argument("-e", "--env", default="tdeck_pro_standalone")
    ap.add_argument("--bin", help="flash this image instead (no sha256 check)")
    a = ap.parse_args()
    fw = a.bin or check(a.env)
    bad = [p for p in a.ports if not flash(p, fw)]
    if bad:
        sys.exit("failed: " + " ".join(bad))


if __name__ == "__main__":
    main()
