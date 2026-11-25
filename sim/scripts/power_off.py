#!/usr/bin/env python3
"""Switch off for storage (S.5 "Switch off", console "poweroff"), 2026-11-24.

    python sim\\scripts\\power_off.py       (from the repo root; needs sim\\build, or set
                                            RS_SIM_EXE=<path to ratspeak-sim.exe>)

Five runs on the same data folder (the simulator ends where the real radio goes into deep
sleep; the next run is the wake-up, its cause given by RS_SIM_WAKE):
  1. boot, Power page row 13 "Switch off", Enter twice -> "[POWER] off ..."
  2. power comes back (a solar panel, the restart button): stays off, never reaches ready
  3. the 12 h battery look: stays off
  4. the top button, held: starts, reports the time it was off, the flag is cleared
  5. a normal start after that: no off message
Exit 0 = all PASS.
"""
import os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("RS_SIM_EXE") or os.path.join(ROOT, "build", "ratspeak-sim.exe")
BASE = os.path.join(tempfile.gettempdir(), "rs_power_off")
DATA = os.path.join(BASE, "data")

S_OFF = """expect 60000 [BOOT] ready
serial wiz skip
wait 500
key M
wait 800
key S
wait 800
key 5
wait 1200
key V
wait 250
key V
wait 250
key V
wait 250
key V
wait 250
key V
wait 250
key V
wait 250
key V
wait 250
key V
wait 250
key V
wait 250
key V
wait 250
key V
wait 250
shot poweroff-01-row.png
key ENTER
expect 3000 [UI] Enter again
key ENTER
expect 8000 [POWER] off (menu)
expect 8000 [POWER] off: deep sleep
shot poweroff-02-screen.png
wait 500
quit
"""
S_STAY = """wait 4000
quit
"""
S_ON = """expect 60000 [BOOT] ready
serial pw
expect 3000 [POWER] last switched off
quit
"""
S_PLAIN = """expect 60000 [BOOT] ready
serial pw
expect 3000 lxmf_queue=
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
        r = subprocess.run([EXE, "--name", "pwoff", "--data", DATA, "--fast", "--script", sc, "--quit"],
                           env=env, capture_output=True, text=True, timeout=180)
        return r.stdout + r.stderr
    except subprocess.TimeoutExpired as e:
        return (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")


def check(name, ok, out):
    print("%s  %s" % ("PASS" if ok else "FAIL", name))
    if not ok:
        print("\n".join(out.splitlines()[-25:]))
    return ok


def main():
    shutil.rmtree(BASE, ignore_errors=True)
    os.makedirs(DATA)
    ok = True
    o = run("off", S_OFF)
    ok &= check("poweroff: saves, draws, sleeps",
                "[POWER] off (menu)" in o and "[POWER] off: deep sleep" in o, o)
    o = run("stay_power", S_STAY, "power")
    ok &= check("power comes back: stays off",
                "power or restart, staying off" in o and "[BOOT] ready" not in o, o)
    o = run("stay_timer", S_STAY, "timer")
    ok &= check("12 h battery look: stays off",
                "12 h battery look, staying off" in o and "[BOOT] ready" not in o, o)
    o = run("on", S_ON, "button")
    ok &= check("top button: starts and reports",
                "[POWER] on (top button): off" in o and "[POWER] last switched off: off" in o, o)
    o = run("plain", S_PLAIN, "power")
    ok &= check("next start is normal",
                "[BOOT] ready" in o and "staying off" not in o and "[POWER] on (top button)" not in o, o)
    print("ALL PASS" if ok else "SOME FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
