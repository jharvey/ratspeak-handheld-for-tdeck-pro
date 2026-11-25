#!/usr/bin/env python3
"""Route points from a GPX file become waypoints (S.2 Track page row 5, console "gpximport").

    python sim\\scripts\\gpx_import.py     (from the repo root; RS_SIM_EXE=<ratspeak-sim.exe>)

A GPX with two <wpt> (one self-closed, without a name), two <rtept> and track points is put
in <SD>/ratdeckpro/routes/. Import: 4 waypoints (track points are not taken), names kept;
a second import adds nothing. Exit 0 = PASS.
"""
import os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("RS_SIM_EXE") or os.path.join(ROOT, "build", "ratspeak-sim.exe")
BASE = os.path.join(tempfile.gettempdir(), "rs_gpx_import")
DATA = os.path.join(BASE, "data")

GPX = """<?xml version="1.0" encoding="UTF-8"?>
<gpx version="1.1" creator="test" xmlns="http://www.topografix.com/GPX/1/1">
  <wpt lat="41.878100" lon="-87.629800"><ele>180</ele><name>Home</name></wpt>
  <wpt lat="41.900000" lon="-87.650000"/>
  <rte><name>Way home</name>
    <rtept lat="42.000000" lon="-87.700000"><name>Bridge over river</name></rtept>
    <rtept
       lat="42.100000"
       lon="-87.800000"><name>Aunt</name></rtept>
  </rte>
  <trk><trkseg><trkpt lat="43.0" lon="-88.0"></trkpt><trkpt lat="43.1" lon="-88.1"></trkpt></trkseg></trk>
</gpx>
"""

SCRIPT = """expect 60000 [BOOT] ready
serial wiz skip
wait 800
serial gpximport
expect 5000 [GPX] W1 Home 41.87810, -87.62980
expect 3000 [GPX] W2 R2 41.90000, -87.65000
expect 3000 [GPX] W3 Bridge over riv 42.00000, -87.70000
expect 3000 [GPX] W4 Aunt 42.10000, -87.80000
expect 3000 [GPX] 1 file(s), 4 point(s) read, 4 waypoint(s) added, 4 in all
serial gpximport
expect 5000 [GPX] 1 file(s), 4 point(s) read, 0 waypoint(s) added, 4 in all
quit
"""


def main():
    shutil.rmtree(BASE, ignore_errors=True)
    os.makedirs(os.path.join(DATA, "sd", "ratdeckpro", "routes"))
    with open(os.path.join(DATA, "sd", "ratdeckpro", "routes", "home.gpx"), "w", newline="\n") as f:
        f.write(GPX)
    sc = os.path.join(BASE, "gpx.txt")
    with open(sc, "w") as f:
        f.write(SCRIPT)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen",
               QT_QPA_FONTDIR=os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts"))
    r = subprocess.run([EXE, "--name", "gpx", "--data", DATA, "--fast", "--script", sc, "--quit"],
                       env=env, capture_output=True, text=True, timeout=240)
    out = r.stdout + r.stderr
    ok = "[SCRIPT] PASS" in out
    print("PASS" if ok else "FAIL")
    if not ok:
        print("\n".join(l for l in out.splitlines() if "GPX" in l or "SCRIPT" in l or "[SD]" in l)[-3000:])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
