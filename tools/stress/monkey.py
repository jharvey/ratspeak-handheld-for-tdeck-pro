#!/usr/bin/env python3
"""Random-key "monkey" test on the simulator.

Each round starts a fresh simulator (headless) and feeds it a random script: key taps
(letters, digits, Enter, Back, Backspace, Space, SYM / ALT combos) with short random
pauses, and now and then a hardware change (GPS, USB power, SD card in / out, battery
level). The firmware heartbeat (`hb`) runs every 20 s so a stuck main loop shows.

A round FAILS when the simulator exits without "[SCRIPT] PASS", prints a crash / trap /
assert line, or the heartbeat stops for 60 s while the script still runs. The script of a
failed round is kept (<out>/round_<n>.txt with its log) and replays the same keys:
  sim\\run-script.bat <out>\\round_<n>.txt monkeyrepro fresh

Use the checked simulator build (bounds / UB traps) with RS_SIM_EXE:
  set RS_SIM_EXE=sim\\build-checked\\ratspeak-sim.exe
Examples:
  python tools/stress/monkey.py --rounds 20 --keys 3000
  python tools/stress/monkey.py --minutes 480 --keys 5000 --out monkey_night
"""
import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SIM = os.path.join(ROOT, os.environ.get("RS_SIM_EXE") or os.path.join("sim", "build", "ratspeak-sim.exe"))

LETTERS = [chr(c) for c in range(ord("A"), ord("Z") + 1)]
# Weighted key choices: navigation keys are common, so the monkey moves between screens.
KEYS = ([(k, 3) for k in LETTERS] + [(d, 1) for d in "0123456789"] +
        [("ENTER", 14), ("SHIFT", 14), ("BKSP", 6), ("SPACE", 4), ("MIC", 1)] +
        [("SYM+" + k, 1) for k in LETTERS] + [("ALT+" + k, 1) for k in LETTERS])
HW = ["gps on", "gps off", "usb on", "usb off", "sd out", "sd in", "battery 5", "battery 60", "battery 100"]
BAD = re.compile(r"runtime error|Assertion|assert|abort|Guru|panic|terminate called|"
                 r"stack smashing|buffer overflow|segmentation|Access violation|EXCEPTION", re.I)


def make_script(rng, n_keys):
    keys, weights = zip(*KEYS)
    lines = ["# monkey", "expect 60000 [BOOT] ready", "serial hb 20", "wait 500"]
    for i in range(n_keys):
        r = rng.random()
        if r < 0.004:
            lines.append(rng.choice(HW))
        elif r < 0.006:
            lines.append("wait %d" % rng.randint(2000, 8000))   # let timers / idle run
        else:
            lines.append("key " + rng.choices(keys, weights)[0])
            if rng.random() < 0.3:
                lines.append("wait %d" % rng.randint(20, 400))
    lines += ["sd in", "wait 1500", "serial hb", "wait 1000", "quit"]
    return "\n".join(lines) + "\n"


def run_round(n, seed, args, out):
    rng = random.Random(seed)
    script = make_script(rng, args.keys)
    spath = os.path.join(out, "round_%03d.txt" % n)
    with open(spath, "w") as f:
        f.write("# seed %d\n" % seed + script)
    name = "monkey_%d" % n
    data = os.path.join(out, "data")
    shutil.rmtree(os.path.join(data, name), ignore_errors=True)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    env.setdefault("QT_QPA_FONTDIR", os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts"))
    log_path = os.path.join(out, "round_%03d.log" % n)
    cmd = [SIM, "--name", name, "--data", os.path.join(data, name), "--fast", "--script", spath, "--quit"]
    t0 = time.time()
    state = {"last_hb": time.time(), "bad": None, "pass": False, "lines": 0, "keys": 0}
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
                             cwd=os.path.join(ROOT, "sim"))

        def reader():
            for raw in p.stdout:
                line = raw.decode("utf-8", "replace").rstrip()
                if line.startswith("[UI] frame") or line.startswith("[EINK]"):
                    continue
                log.write(line + "\n")
                state["lines"] += 1
                if line.startswith("[HB]"):
                    state["last_hb"] = time.time()
                elif line.startswith("[SCRIPT] key"):
                    state["keys"] += 1
                elif line.startswith("[SCRIPT] PASS"):
                    state["pass"] = True
                elif line.startswith("[SCRIPT] FAIL"):
                    state["bad"] = state["bad"] or line
                if BAD.search(line) and not line.startswith("[SCRIPT]") and not state["bad"]:
                    state["bad"] = line

        th = threading.Thread(target=reader, daemon=True)
        th.start()
        hung = False
        while p.poll() is None:
            time.sleep(1)
            if time.time() - state["last_hb"] > 60 and time.time() - t0 > 90:
                hung = True
                p.kill()
                break
        p.wait()
        th.join(5)
    ok = state["pass"] and not state["bad"] and not hung and p.returncode == 0
    why = ("heartbeat stopped (main loop stuck)" if hung else state["bad"] or
           ("" if state["pass"] else "exited %s without PASS" % p.returncode))
    if ok:
        shutil.rmtree(os.path.join(data, name), ignore_errors=True)
        os.remove(spath)
        os.remove(log_path)
    return ok, why, state["keys"], time.time() - t0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--minutes", type=float, default=0, help="run rounds until this much time has passed")
    ap.add_argument("--keys", type=int, default=3000, help="key taps per round")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--out", default=os.path.join(ROOT, "monkey_out"))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    seed0 = args.seed if args.seed is not None else int(time.time())
    print("monkey: simulator %s, seed %d, %d keys per round" % (SIM, seed0, args.keys), flush=True)
    end = time.time() + args.minutes * 60 if args.minutes else None
    fails, total_keys, n = [], 0, 0
    while (end and time.time() < end) or (not end and n < args.rounds):
        ok, why, keys, secs = run_round(n, seed0 + n, args, args.out)
        total_keys += keys
        print("%s round %d seed %d: %s, %d keys, %.0f s%s" % (time.strftime("%H:%M:%S"), n, seed0 + n,
              "ok" if ok else "FAIL", keys, secs, "" if ok else " - " + why), flush=True)
        if not ok:
            fails.append((n, seed0 + n, why))
        n += 1
    print("monkey: %d rounds, %d keys, %d failed" % (n, total_keys, len(fails)))
    for f in fails:
        print("  round %d seed %d: %s  (script %s)" % (f[0], f[1], f[2],
              os.path.join(args.out, "round_%03d.txt" % f[0])))
    print("RESULT: %s" % ("PASS" if not fails else "FAIL"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
