#!/usr/bin/env python3
"""Long run of the stored-radio life in the simulator: switched off, a daily contact every
"day", now and then a real use, for many days in a row on the same data folder.

    python tools/stress/off_cycles.py                 (14 days)
    python tools/stress/off_cycles.py --days 60
    (from the repo root; RS_SIM_EXE=<ratspeak-sim.exe>, else sim/build/ratspeak-sim.exe)

Day 0: daily contact on (2 min), Home set, switch off. Then per day:
  - a battery look (timer wake, not the contact time): must stay off
  - the daily contact: must start, send its status, switch off again by itself
  - every 5th day the owner switches on with the button, sends a message to a contact that
    does not exist (it is kept or fails), opens the map, and switches off again
It checks each step, and watches for growth: the size of the flash image and of the NVS
folder at the start and the end, and the number of files in the data folder.
Exit 0 = every day passed and the data folder grew by less than --grow KB (default 200).
Not part of the regression (about 3 minutes per day).
"""
import argparse, os, shutil, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.environ.get("RS_SIM_EXE") or os.path.join(ROOT, "sim", "build", "ratspeak-sim.exe")
BASE = os.path.join(tempfile.gettempdir(), "rs_off_cycles")
DATA = os.path.join(BASE, "data")

S_DAY0 = """expect 60000 [BOOT] ready
serial wiz skip
wait 500
serial home set 41.8781 -87.6298
serial offc stay 2
serial offc at 2000
serial offc on
expect 3000 [CONTACT] daily contact on
serial poweroff
expect 8000 [POWER] off: deep sleep
wait 500
quit
"""
S_STAY = """wait 4000
quit
"""
S_CONTACT = """expect 60000 [BOOT] ready
expect 200000 [CONTACT] status sent to
expect 400000 [CONTACT] window over
expect 20000 [POWER] off: deep sleep
wait 500
quit
"""
S_USE = """expect 60000 [BOOT] ready
serial pw
expect 3000 [POWER] last switched off
serial msg 0123456789abcdef0123456789abcdef day %d
expect 5000 [MSG] send
key A
wait 1500
serial home go
expect 3000 [NAV] open kind=3
wait 1000
key G
wait 500
serial trip
expect 5000 [TRIP] 11 checks
serial poweroff
expect 8000 [POWER] off: deep sleep
wait 500
quit
"""


def run(tag, script, wake=None):
    sc = os.path.join(BASE, tag + ".txt")
    with open(sc, "w") as f:
        f.write(script)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen",
               QT_QPA_FONTDIR=os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts"))
    env.pop("RS_SIM_WAKE", None)
    if wake:
        env["RS_SIM_WAKE"] = wake
    try:
        r = subprocess.run([EXE, "--name", "offcyc", "--data", DATA, "--fast", "--script", sc, "--quit"],
                           env=env, capture_output=True, text=True, timeout=900)
        return r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return "TIMEOUT"


def size_kb(path):
    total, files = 0, 0
    for d, _, fs in os.walk(path):
        for f in fs:
            try:
                total += os.path.getsize(os.path.join(d, f))
                files += 1
            except OSError:
                pass
    return total / 1024.0, files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", type=int, default=14)
    ap.add_argument("--grow", type=float, default=200.0, help="allowed growth of the data folder, KB")
    a = ap.parse_args()
    shutil.rmtree(BASE, ignore_errors=True)
    os.makedirs(DATA)
    bad = []
    o = run("day0", S_DAY0)
    if "[POWER] off: deep sleep" not in o:
        print("FAIL day 0: could not switch off\n" + "\n".join(o.splitlines()[-15:]))
        return 1
    kb0, files0 = size_kb(DATA)
    print("day 0: switched off; data folder %.0f KB, %d files" % (kb0, files0), flush=True)
    t0 = time.time()
    for day in range(1, a.days + 1):
        o = run("look", S_STAY, "timer")
        if "battery look, staying off" not in o or "[BOOT] ready" in o:
            bad.append("day %d: the battery look did not stay off" % day)
        o = run("contact", S_CONTACT, "contact")
        if "[SCRIPT] PASS" not in o or "[POWER] off (daily contact done)" not in o:
            bad.append("day %d: the daily contact did not finish" % day)
        if day % 5 == 0:
            o = run("use", S_USE % day, "button")
            if "[SCRIPT] PASS" not in o:
                bad.append("day %d: the use by hand failed" % day)
            o = run("usecheck", S_STAY, "power")
            if "staying off" not in o:
                bad.append("day %d: not off after the use by hand" % day)
        kb, files = size_kb(DATA)
        print("day %d: %s; data %.0f KB, %d files; %.0f min so far" % (
            day, "ok" if not bad or not bad[-1].startswith("day %d:" % day) else "PROBLEM", kb, files,
            (time.time() - t0) / 60), flush=True)
    kb1, files1 = size_kb(DATA)
    grew = kb1 - kb0
    print("data folder: %.0f KB -> %.0f KB (%+.0f KB), files %d -> %d" % (kb0, kb1, grew, files0, files1))
    for b in bad:
        print("  " + b)
    if grew > a.grow:
        print("  the data folder grew by more than %.0f KB" % a.grow)
    ok = not bad and grew <= a.grow
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
