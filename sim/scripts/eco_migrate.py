#!/usr/bin/env python3
"""Energy mode numbering with Eco (2026-11-24): a unit saved before Eco keeps its mode.

    python sim\\scripts\\eco_migrate.py      (from the repo root; needs sim\\build, or set
                                            RS_SIM_EXE=<path to ratspeak-sim.exe>)

Eco came in after Balanced, so Saver and everything after it moved up by one. The mode is
now saved as "mode3"; an older "mode2" (Full 0, Balanced 1, Saver 2, Ultra-low 3, On demand 4,
Check-in 5, Custom 6) and the oldest "mode" (Full 0, Balanced 1, Saver 2, Custom 3) are read
once and moved. For each case: write rs_power.nvs, boot, check the mode in force, check
"mode3" is saved after a change. Exit 0 = all PASS.
"""
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("RS_SIM_EXE") or os.path.join(ROOT, "build", "ratspeak-sim.exe")
BASE = os.path.join(tempfile.gettempdir(), "rs_eco_migrate")

CASES = [   # (key, saved value, mode expected)
    ("mode2", 1, "Balanced"),
    ("mode2", 2, "Saver"),
    ("mode2", 3, "Ultra-low"),
    ("mode2", 5, "Check-in"),
    ("mode2", 6, "Custom"),
    ("mode", 2, "Saver"),
    ("mode", 3, "Custom"),
    ("mode3", 2, "Eco"),
]

SCRIPT = """expect 60000 [BOOT] ready
serial wiz skip
wait 500
serial pw
expect 3000 [POWER] mode=
quit
"""


def run(name, key, val):
    data = os.path.join(BASE, name)
    shutil.rmtree(data, ignore_errors=True)
    os.makedirs(os.path.join(data, "nvs"))
    with open(os.path.join(data, "nvs", "rs_power.nvs"), "w", newline="") as f:
        f.write("%s\t1\t%02x00000000000000\r\n" % (key, val))
    sc = os.path.join(BASE, "eco_migrate.txt")
    with open(sc, "w") as f:
        f.write(SCRIPT)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen",
               QT_QPA_FONTDIR=os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts"))
    r = subprocess.run([EXE, "--name", name, "--data", data, "--fast", "--script", sc, "--quit"],
                       env=env, capture_output=True, text=True, timeout=180)
    m = re.search(r"\[POWER\] mode=([A-Za-z -]+?)(?: \(| cpu=)", r.stdout)
    return m.group(1).strip() if m else None


def main():
    os.makedirs(BASE, exist_ok=True)
    ok = True
    for i, (key, val, want) in enumerate(CASES):
        got = run("ecomig%d" % i, key, val)
        good = got == want
        ok &= good
        print("%s %s=%d -> %s (want %s)" % ("PASS" if good else "FAIL", key, val, got, want), flush=True)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
