#!/usr/bin/env python3
"""Power cuts while the radio switches itself off (S.5 Switch off), then the next start:
the radio must never be stuck, and its Home and daily-contact settings must survive.

    python tools/stress/offcut.py --cycles 20
    (from the repo root; RS_SIM_EXE=<ratspeak-sim.exe>, else sim/build/ratspeak-sim.exe)

Each cycle on the same data folder:
  1. start (top button held), wait for boot; Home and the daily contact are set
  2. "poweroff", then a hard kill 0-1.5 s later (a power cut in the middle of saving,
     drawing the off screen, or the deep sleep itself)
  3. start again, half the cycles as "power came back" (charger / restart button) and half
     as the top button held, and check:
       - power came back: the radio either starts normally (the cut came before the "off"
         mark was saved) or says it stays off; nothing else (no hang, no crash)
       - top button: it starts ([BOOT] ready) every time
       - after a start: Home and the daily contact are still there
Logs: <out>/cycle_NNN.log (kept only for failed cycles unless --keep-logs), summary.txt.
Exit 0 = every cycle passed.
"""
import argparse, os, random, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pingpong import ROOT, SIM, SimDevice  # noqa: E402

OUT_DEFAULT = os.path.join(ROOT, "sim", "build", "offcut")


def start(name, data, log, port, air, wake):
    if wake:
        os.environ["RS_SIM_WAKE"] = wake
    else:
        os.environ.pop("RS_SIM_WAKE", None)
    return SimDevice(name, port, data, log, 0, air)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cycles", type=int, default=20)
    ap.add_argument("--out", default=OUT_DEFAULT)
    ap.add_argument("--tcp", type=int, default=5731)
    ap.add_argument("--air", type=int, default=48131)
    ap.add_argument("--seed", type=int, default=int(time.time()))
    ap.add_argument("--keep-logs", action="store_true")
    a = ap.parse_args()
    random.seed(a.seed)
    shutil.rmtree(a.out, ignore_errors=True)
    data = os.path.join(a.out, "data")
    os.makedirs(data)
    fails, stats = [], {"came_on": 0, "stayed_off": 0, "button": 0}
    print("seed %d, %d cycles" % (a.seed, a.cycles), flush=True)

    # First start: settings that must survive every cut.
    log0 = os.path.join(a.out, "setup.log")
    d = start("offcut", data, log0, a.tcp, a.air, None)
    i, _ = d.wait_for(r"\[BOOT\] ready", 120)
    if i is None:
        print("FAIL: first boot")
        return 1
    d.command("wiz skip")
    time.sleep(1)
    d.command("home set 41.8781 -87.6298", r"\[HOME\] set", 10)
    d.command("offc on", r"\[CONTACT\] daily contact on", 10)
    d.close()

    for c in range(1, a.cycles + 1):
        log = os.path.join(a.out, "cycle_%03d.log" % c)
        why = None
        # 1. start with the button (works whether or not the last cycle left it "off")
        d = start("offcut", data, log, a.tcp, a.air, "button")
        try:
            i, _ = d.wait_for(r"\[BOOT\] ready", 150)
            if i is None and d.command("pw", r"\[POWER\] mode=", 15):
                i = 0                    # booted before the TCP console connected
            if i is None:
                why = "no boot on the top button"
                raise RuntimeError
            stats["button"] += 1
            m = d.command("home", r"\[HOME\] (not set|[-0-9.]+, [-0-9.]+)", 10)
            if not m or "not set" in m.group(0):
                why = "Home lost"
                raise RuntimeError
            m = d.command("offc", r"\[CONTACT\] daily contact (on|off)", 10)
            if not m or m.group(1) != "on":
                why = "daily contact setting lost"
                raise RuntimeError
            # 2. switch off and cut the power at a random moment
            d.send("poweroff")
            time.sleep(random.uniform(0.0, 1.5))
        except RuntimeError:
            pass
        finally:
            d.alive = False
            try:
                d.proc.kill()
                d.proc.wait(15)          # the next start must not meet this one's TCP port
            except Exception:
                pass
            try:
                d.sock.close()
            except Exception:
                pass
            d.log.close()
        if not why:
            # 3. the next start: power came back, or the button
            wake = random.choice(["power", "button"])
            time.sleep(1)
            # Script mode: every line from the first moment of boot is captured (a radio that
            # stays off says so before a TCP console could connect).
            sc = os.path.join(a.out, "next.txt")
            with open(sc, "w") as f:
                f.write("expect 150000 [BOOT] ready\nwait 500\nquit\n")
            env = dict(os.environ, QT_QPA_PLATFORM="offscreen", RS_SIM_WAKE=wake, RS_SIM_AIR_PORT=str(a.air),
                       QT_QPA_FONTDIR=os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts"))
            try:
                r = subprocess.run([SIM, "--name", "offcut", "--data", data, "--fast", "--script", sc, "--quit"],
                                   env=env, capture_output=True, text=True, timeout=240)
                out = r.stdout + r.stderr
            except subprocess.TimeoutExpired:
                out = ""
            with open(log + ".2", "w", encoding="utf-8", errors="replace") as f:
                f.write(out)
            ready, off = "[BOOT] ready" in out, "staying off" in out
            if not ready and not off:
                why = "next start (%s): neither ready nor off" % wake
            elif wake == "button" and not ready:
                why = "button start stayed off"
            elif off:
                stats["stayed_off"] += 1
            else:
                stats["came_on"] += 1
        if why:
            fails.append((c, why))
            print("cycle %d: FAIL %s" % (c, why), flush=True)
        else:
            if not a.keep_logs:
                for f in (log, log + ".2"):
                    try:
                        os.remove(f)
                    except OSError:
                        pass
            if c % 5 == 0:
                print("cycle %d ok (%s)" % (c, stats), flush=True)
    summary = "offcut seed %d: %d cycles, %d failed; %s" % (a.seed, a.cycles, len(fails), stats)
    with open(os.path.join(a.out, "summary.txt"), "w") as f:
        f.write(summary + "\n" + "\n".join("cycle %d: %s" % x for x in fails) + "\n")
    print(summary)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
