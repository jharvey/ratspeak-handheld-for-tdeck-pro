#!/usr/bin/env python3
"""Put RatSpeak on a brand-new T-Deck Pro over USB (no local build, no PlatformIO project).

A new unit needs four things, all in the repo:
  0x0000   firmware/boot/bootloader.bin    (from the build VM, same build as the app)
  0x8000   firmware/boot/partitions.bin    (partitions/standalone-16mb.csv)
  0xe000   firmware/boot/boot_app0.bin     (start from the first app slot)
  0x10000  firmware/tdeck_pro_standalone.bin (the app; sha256 checked against its .txt)

  python tools/flash_new_device.py COM7             # one unit
  python tools/flash_new_device.py COM7 COM10       # several, one after the other
  python tools/flash_new_device.py --all            # every serial port that is not listed in --skip
  python tools/flash_new_device.py --erase COM7     # blank settings and data first (a used unit: settings,
                                                    # identity and messages are LOST)
  python tools/flash_new_device.py --dry-run COM7   # print what would be written, touch nothing

If a unit does not answer: hold the TOP side button, press and release the BOTTOM side button
(reset), let go of the top one. That is the chip's USB download mode. Run the script again.

After writing, the script waits for the unit to start and prints its address. A new unit then
shows the first-run wizard. An existing unit (no --erase) keeps its settings and messages.
Units already running RatSpeak only need tools/flash_app.py (the app alone).
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flash_app                                    # noqa: E402  (the app writer and the sha256 check)

ROOT = flash_app.ROOT
BOOT = os.path.join(ROOT, "firmware", "boot")
PARTS = [(0x0000, "bootloader.bin"), (0x8000, "partitions.bin"), (0xe000, "boot_app0.bin")]


def esptool(port, args, tries=4):
    """Run esptool with the ROM loader (the stub fails on these boards). Returns its output or None."""
    for _ in range(tries):
        r = subprocess.run(flash_app.esptool_cmd() + ["--chip", "esp32s3", "--port", port, "--baud", "115200",
                                                      "--before", "default_reset", "--after", "no_reset",
                                                      "--no-stub"] + args, capture_output=True, text=True)
        out = r.stdout + r.stderr
        if r.returncode == 0:
            return out
        time.sleep(3)
    print(out.strip().splitlines()[-1] if out.strip() else "no answer")
    return None


ERASE = [(0x9000, 0x7000)] + [(0x810000 + i * 0x100000, 0x100000) for i in range(7)] + [(0xF10000, 0xF0000)]


def erase_data(port):
    """Blank NVS, OTA data, LittleFS and the core dump (everything after the boot files but the apps)."""
    import tempfile
    tmp = tempfile.mkdtemp()
    for off, size in ERASE:
        blank = os.path.join(tmp, "ff_%x.bin" % size)
        if not os.path.exists(blank):
            with open(blank, "wb") as f:
                f.write(b"\xff" * size)
        out = esptool(port, ["write_flash", "--flash_size", "16MB", "0x%x" % off, blank])
        if not out or "Hash of data verified" not in out:
            print("%s FAILED blanking 0x%x" % (port, off))
            return False
        print("%s 0x%x blank" % (port, off))
    return True


def flash_boot(port):
    for off, name in PARTS:
        out = esptool(port, ["write_flash", "--flash_size", "16MB", "0x%x" % off, os.path.join(BOOT, name)])
        if not out or "Hash of data verified" not in out:
            print("%s FAILED writing %s at 0x%x" % (port, name, off))
            return False
        print("%s 0x%x %s ok" % (port, off, name))
    return True


def wait_ready(port, secs=240):
    """Read the console until the unit says it has started; print its address."""
    try:
        import serial
    except ImportError:
        print("%s written (pyserial not installed: start-up not checked)" % port)
        return True
    end = time.time() + secs
    while time.time() < end:
        try:
            s = serial.Serial()
            s.port = port; s.baudrate = 115200; s.timeout = 0.5; s.dtr = False; s.rts = False
            s.open()
        except Exception:
            time.sleep(2)                       # the port goes away while the unit restarts
            continue
        try:
            buf, asked = b"", 0.0
            while time.time() < end:
                buf += s.read(4096)
                if time.time() - asked > 5:     # "id" answers once the firmware is up
                    s.write(b"id\n"); s.flush(); asked = time.time()
                for line in buf.decode("utf-8", "replace").splitlines():
                    if line.startswith("[ID] dest="):
                        print("%s started: %s" % (port, line[5:].strip()))
                        return True
        except Exception:
            pass
        finally:
            try:
                s.close()
            except Exception:
                pass
        time.sleep(2)
    print("%s written, but it did not answer within %d s (first start formats its storage: "
          "look at the screen, or run again)" % (port, secs))
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ports", nargs="*")
    ap.add_argument("-e", "--env", default="tdeck_pro_standalone")
    ap.add_argument("--all", action="store_true", help="every serial port found, except --skip")
    ap.add_argument("--skip", nargs="*", default=[], help="ports to leave alone with --all")
    ap.add_argument("--erase", action="store_true", help="erase the whole flash first: ALL data on the unit is lost")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    ports = list(a.ports)
    if a.all:
        from serial.tools import list_ports
        skip = {p.upper() for p in a.skip}
        ports += [p.device for p in list_ports.comports() if p.device.upper() not in skip and p.device not in ports]
    if not ports:
        sys.exit("no port given (COMx ..., or --all)")
    fw = flash_app.check(a.env)
    for _, name in PARTS:
        if not os.path.isfile(os.path.join(BOOT, name)):
            sys.exit("missing firmware/boot/%s" % name)
    if a.dry_run:
        for port in ports:
            print("%s: %s0x0 bootloader, 0x8000 partitions, 0xe000 boot_app0, 0x10000 %s (%d bytes)"
                  % (port, "ERASE ALL, " if a.erase else "", os.path.basename(fw), os.path.getsize(fw)))
        return

    bad = []
    for port in ports:
        print("== %s" % port)
        if not esptool(port, ["chip_id"]):
            print("%s does not answer. Hold the TOP side button, press and release the BOTTOM one, "
                  "let go of the top one, then run this again." % port)
            bad.append(port); continue
        if a.erase:
            # The ESP32-S3 ROM loader (the stub fails on these boards) has no erase_flash, so the
            # settings (NVS + OTA data) and the data partition (LittleFS + core dump) are overwritten
            # with blank (0xFF) pieces; compressed, so each piece is quick to send.
            print("%s erasing settings and data (a few minutes)" % port)
            if not erase_data(port):
                print("%s erase FAILED" % port)
                bad.append(port); continue
        # the app first (highest piece first), the boot files last: the unit only becomes
        # startable when everything it needs is already there
        if not (flash_app.flash(port, fw, reset=False) and flash_boot(port)):
            bad.append(port); continue
        esptool(port, ["--after", "hard_reset", "read_mac"], tries=2)      # restart into the firmware
        if not wait_ready(port):
            bad.append(port)
    print("\n%d of %d done%s" % (len(ports) - len(bad), len(ports), "; FAILED: " + ", ".join(bad) if bad else ""))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
