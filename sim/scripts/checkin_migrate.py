#!/usr/bin/env python3
"""Check-in time migration: an old unit saved the check-in minute ("ciMin", 0-59); the new
firmware keeps a time of day ("ciStart") and turns ciMin m into 7:m.

    python sim\scripts\checkin_migrate.py      (from the repo root; needs sim\build)

Writes rs_power.nvs with ciMin = 45 for device "ckmig", runs checkin_migrate.txt (expects
07:45), then checks the saved NVS has ciStart and no ciMin. Exit 0 = PASS.
"""
import os, shutil, subprocess, sys

SIM = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(SIM, "build", "simdata", "ckmig")
if os.path.isdir(DATA):
    shutil.rmtree(DATA)
os.makedirs(os.path.join(DATA, "nvs"))
with open(os.path.join(DATA, "nvs", "rs_power.nvs"), "w", newline="") as f:
    f.write("ciEvery\t3\t3c00000000000000\r\nciMin\t1\t2d00000000000000\r\nmode2\t1\t0400000000000000\r\n")
r = subprocess.run(["cmd", "/c", os.path.join(SIM, "run-script.bat"), "scripts\checkin_migrate.txt", "ckmig"],
                   cwd=SIM, capture_output=True, text=True)
out = r.stdout.strip().splitlines()
print(out[-1] if out else r.stderr)
nvs = open(os.path.join(DATA, "nvs", "rs_power.nvs")).read()
ok = r.returncode == 0 and "ciStart" in nvs and "ciMin" not in nvs
print("NVS:", "ciStart saved, ciMin gone" if ok else nvs)
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
