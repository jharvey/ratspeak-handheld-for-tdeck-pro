#!/usr/bin/env python3
"""Map stress test on a real unit over USB: random map views between other screens.

Drives the console: `map <lat> <lon> <zoom>` near a place (now and then far away, where the
card has nothing), pan / zoom keys (`k`), layer choices (`maplayers`), the map closed, the
Contact / Inbox / Menu screens and the idle screens in between. Every few steps it asks for
the heartbeat (`hb`) and the map counters (`map`).

It FAILS on a restart ([BOOT] line, reset reason, Guru Meditation, abort), on a map layer
stream that did not decode ("N bad"), or when the unit stops answering. It reports the
slowest main-loop steps ([SLOW] lines), time per map step, tile decode time and the lowest
free memory seen.

    python tools/stress/mapstress.py --port COM6 --map "43.64 -70.92" --minutes 30
"""
import argparse
import random
import re
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("needs pyserial: pip install pyserial")

CRASH = re.compile(r"Guru Meditation|abort\(\) was called|Backtrace:|rst:0x|\[BOOT\]|Rebooting|panic", re.I)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    ap.add_argument("--map", required=True, help='"<lat> <lon>" the card has maps for')
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--log", default="mapstress.log")
    ap.add_argument("--zoom", default="3-16", help="zoom range to visit")
    ap.add_argument("--slow-ms", type=int, default=1500, help="a screen that takes longer to draw counts as slow")
    a = ap.parse_args()
    lat0, lon0 = (float(v) for v in a.map.replace(",", " ").split())
    z0, z1 = (int(v) for v in a.zoom.split("-"))
    seed = a.seed if a.seed is not None else int(time.time())
    rng = random.Random(seed)

    s = serial.Serial()
    s.port = a.port; s.baudrate = 115200; s.timeout = 0.2; s.dtr = False; s.rts = False
    s.open()
    log = open(a.log, "w", encoding="utf-8", errors="replace")
    st = {"last": time.time(), "crash": None, "slow": {}, "bad": 0, "tiles": 0, "streams": 0, "coded": 0,
          "heap_min": None, "ps_min": None, "decode_ms": 0, "packed": 0, "pack_ms": 0, "lines": 0,
          "frames": 0, "build_sum": 0, "build_max": 0, "build_max_after": "", "slow_frames": 0, "cmd": "",
          "stop": False}

    def reader():
        buf = b""
        while not st["stop"]:
            try:
                d = s.read(4096)
            except Exception as e:                      # unplugged
                st["crash"] = st["crash"] or "serial port lost: %s" % e
                return
            if not d:
                continue
            buf += d
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip()
                st["last"] = time.time(); st["lines"] += 1
                log.write("%s %s\n" % (time.strftime("%H:%M:%S"), line))
                if CRASH.search(line) and not st["crash"]:
                    st["crash"] = line
                m = re.match(r"\[SLOW\] (\w+) (\d+) ms", line)
                if m:
                    k, v = m.group(1), int(m.group(2))
                    c = st["slow"].setdefault(k, [0, 0])
                    c[0] += 1; c[1] = max(c[1], v)
                m = re.match(r"\[UI\] frame (\d+)ms \(build (\d+), panel (\d+)\)", line)
                if m:                                   # build = drawing the screen, incl. reading + decoding tiles
                    b = int(m.group(2))
                    st["frames"] += 1; st["build_sum"] += b
                    if b > st["build_max"]:
                        st["build_max"] = b; st["build_max_after"] = st["cmd"]
                    if b > a.slow_ms:
                        st["slow_frames"] += 1
                m = re.search(r"layered packs: (\d+) tiles, (\d+) layer streams \((\d+) compressed\), (\d+) bad", line)
                if m:
                    st["tiles"], st["streams"], st["coded"], st["bad"] = (int(v) for v in m.groups())
                m = re.search(r"\(packed (\d+), (\d+) ms, decode (\d+) ms\)", line)
                if m:
                    st["packed"], st["pack_ms"], st["decode_ms"] = (int(v) for v in m.groups())
                m = re.search(r"\[HB\].* heap=(\d+).* psram=(\d+)", line)
                if m:
                    h, p = int(m.group(1)), int(m.group(2))
                    st["heap_min"] = h if st["heap_min"] is None else min(st["heap_min"], h)
                    st["ps_min"] = p if st["ps_min"] is None else min(st["ps_min"], p)

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    def send(cmd, settle):
        """Send a console line, then wait until the unit has been quiet for `settle` s (max 20 s)."""
        log.write("%s >>> %s\n" % (time.strftime("%H:%M:%S"), cmd))
        t0 = time.time()
        st["cmd"] = cmd
        s.write((cmd + "\n").encode()); s.flush()
        st["last"] = time.time()
        while time.time() - st["last"] < settle and time.time() - t0 < 20:
            time.sleep(0.05)
        return time.time() - t0 - settle

    print("mapstress: %s, seed %d, %.0f min, around %.4f,%.4f" % (a.port, seed, a.minutes, lat0, lon0), flush=True)
    t_first = time.time()
    while st["lines"] == 0 and time.time() - t_first < 60:   # a busy unit can take a while to answer
        send("hb", 2.0)
    if st["lines"] == 0:
        sys.exit("no answer from the unit on %s" % a.port)
    end = time.time() + a.minutes * 60
    steps = 0; map_steps = 0; map_secs = 0.0; worst = (0.0, "")
    silent = 0
    while time.time() < end and not st["crash"]:
        r = rng.random()
        timed = False
        if r < 0.40:
            if rng.random() < 0.12:
                la, lo = rng.uniform(-70, 70), rng.uniform(-179, 179)
            else:
                la, lo = lat0 + rng.uniform(-0.25, 0.25), lon0 + rng.uniform(-0.35, 0.35)
            cmd = "map %.5f %.5f %d" % (la, lo, rng.randint(z0, z1)); timed = True
        elif r < 0.72:
            cmd = "k " + "".join(rng.choice("tvfhtvfhzzwwd") for _ in range(rng.randint(1, 6))); timed = True
        elif r < 0.78:
            cmd = "maplayers %d" % rng.choice([0, 63, 63, rng.randint(0, 63)])
        elif r < 0.84:
            cmd = "map close"
        elif r < 0.92:
            cmd = "k " + rng.choice(["c", "i", "m^", "^", "i^", "c^"])       # other screens, then back
        else:
            send("idlescr %d" % rng.randint(0, 3), 0.3)
            cmd = "idle now"
        before = st["lines"]
        dt = send(cmd, 1.2)
        steps += 1
        if timed:
            map_steps += 1; map_secs += dt
            if dt > worst[0]:
                worst = (dt, cmd)
        if cmd == "idle now":
            send("k i", 1.0)                             # a key leaves the idle screen
        silent = silent + 1 if st["lines"] == before else 0
        if steps % 10 == 0:
            before = st["lines"]
            send("hb", 0.6); send("map", 0.6)
            # No reply: the main loop is busy (a slow message save can take many seconds).
            # Keep asking for up to 45 s before calling it dead, and note how long it was.
            t_wait = time.time()
            while st["lines"] == before and time.time() - t_wait < 45:
                send("hb", 1.5)
            if st["lines"] == before:
                st["crash"] = "unit stopped answering (no reply to hb / map for 45 s)"
            elif time.time() - t_wait > 3:
                stall = time.time() - t_wait
                st["stalls"] = st.get("stalls", 0) + 1
                st["stall_max"] = max(st.get("stall_max", 0), stall)
                print("%s unit did not answer for %.0f s" % (time.strftime("%H:%M:%S"), stall), flush=True)
            print("%s step %d: %d tiles, %d streams (%d compressed), %d bad, heap min %s, %d screens drawn, slowest %d ms"
                  % (time.strftime("%H:%M:%S"), steps, st["tiles"], st["streams"], st["coded"], st["bad"],
                     st["heap_min"], st["frames"], st["build_max"]), flush=True)
            if st["bad"]:
                st["crash"] = "%d map layer streams failed to decode" % st["bad"]
    send("map close", 1.0); send("hb", 0.8)
    st["stop"] = True; th.join(2); s.close(); log.close()
    print("\nmapstress: %d steps (%d map steps), %d screens drawn: mean %d ms to draw (without the panel refresh), "
          "slowest %d ms after '%s', %d over %d ms" % (steps, map_steps, st["frames"],
          st["build_sum"] // max(1, st["frames"]), st["build_max"], st["build_max_after"], st["slow_frames"], a.slow_ms))
    print("layered packs: %d tiles, %d layer streams (%d compressed), %d bad; decode %d ms for %d packed tiles "
          "(%.1f ms each), read + decode %d ms" % (st["tiles"], st["streams"], st["coded"], st["bad"], st["decode_ms"],
                                                   st["packed"], st["decode_ms"] / max(1, st["packed"]), st["pack_ms"]))
    print("lowest free memory seen: heap %s, PSRAM %s" % (st["heap_min"], st["ps_min"]))
    if st.get("stalls"):
        print("unit did not answer for more than 3 s: %d times, longest %.0f s" % (st["stalls"], st["stall_max"]))
    for k, (n, mx) in sorted(st["slow"].items(), key=lambda kv: -kv[1][1])[:8]:
        print("  [SLOW] %-14s %4d times, longest %d ms" % (k, n, mx))
    if st["crash"]:
        print("FAIL: %s" % st["crash"])
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
