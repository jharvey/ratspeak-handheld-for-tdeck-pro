#!/usr/bin/env python3
"""Power-cut test: kill the simulator at random moments while it saves, restart, check.

Each cycle: start the simulator (same data folder), wait for boot, start a burst of message
saves (`msgfill`), and kill the process at a random moment (a hard kill: whatever the
firmware had not written yet is lost, like a power cut). Some cycles kill during boot
instead. After every restart it checks:
  - the firmware boots ([BOOT] ready) and the message store starts (no "deferred");
  - the identity (destination hash) never changes;
  - no saved message is lost: the records now on flash >= records before the cycle +
    saves the heartbeat had confirmed before the kill;
  - every listed record is readable (msgstat bad=0).

  python tools/stress/powercut.py --cycles 100
  python tools/stress/powercut.py --cycles 30 --flashkb 1024     (full-flash removal too)
Logs: <out>/cycle_NNN.log (only kept for failed cycles unless --keep-logs), summary.txt
"""
import argparse
import os
import random
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pingpong import ROOT, SIM, SimDevice, parse_kv  # noqa: E402

PEERS = 4
PEER_HEX = ["f0" * 15 + "%02x" % i for i in range(PEERS)]


def total_records(dev):
    """(records, bad, ok) over the msgfill test conversations."""
    total = bad = 0
    for p in PEER_HEX:
        m = dev.command("msgstat %s" % p, r"\[STAT\].*", 60)
        if not m:
            return None, None, False
        kv = parse_kv(m.group(0))
        if "ERROR" in m.group(0) and int(kv.get("in", "0")) == 0:
            continue  # conversation not created yet
        total += int(kv.get("in", "0")) + int(kv.get("out", "0"))
        bad += int(kv.get("bad", "0"))
    return total, bad, True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cycles", type=int, default=50)
    ap.add_argument("--flashkb", type=int, default=0)
    ap.add_argument("--boot-kill", type=float, default=0.15, help="share of cycles killed during boot")
    ap.add_argument("--out", default=os.path.join(ROOT, "sim", "build", "powercut"))
    ap.add_argument("--tcp", type=int, default=5711)
    ap.add_argument("--air", type=int, default=48111)
    ap.add_argument("--seed", type=int, default=int(time.time()))
    ap.add_argument("--keep-logs", action="store_true")
    ap.add_argument("--reset-at", type=int, default=1000,
                    help="delete the test conversations above this many records (normal flash only)")
    a = ap.parse_args()
    random.seed(a.seed)
    if not os.path.exists(SIM):
        sys.exit("build the simulator first: sim\\build-sim.bat")
    os.makedirs(a.out, exist_ok=True)
    data = os.path.join(a.out, "data")
    shutil.rmtree(data, ignore_errors=True)

    identity = None
    records = 0          # records known to be on flash before this cycle
    failures = []
    boot_times = []
    kills_in_boot = kills_in_save = 0
    for cycle in range(1, a.cycles + 1):
        log = os.path.join(a.out, "cycle_%03d.log" % cycle)
        dev = SimDevice("pcut", a.tcp, data, log, a.flashkb, a.air)
        problems = []
        try:
            t0 = time.time()
            if cycle > 1 and random.random() < a.boot_kill:
                # Power cut during boot (recovery, migration, identity load).
                time.sleep(random.uniform(0.3, 6.0))
                kills_in_boot += 1
                dev.proc.kill()
                dev.proc.wait(10)
                dev.close()
                continue
            i, _ = dev.wait_for(r"\[BOOT\] ready", 90)
            if i is None:
                problems.append("no [BOOT] ready within 90 s")
                raise RuntimeError
            boot_times.append(time.time() - t0)
            with dev.lock:
                text = "\n".join(dev.lines)
            if "Initialization deferred" in text:
                problems.append("message store did not start: " +
                                re.search(r".*Initialization deferred.*", text).group(0))
            m = dev.command("id", r"\[ID\] dest=([0-9a-f]{32})", 15)
            if not m:
                problems.append("no id")
            elif identity is None:
                identity = m.group(1)
            elif m.group(1) != identity:
                problems.append("identity changed %s -> %s" % (identity, m.group(1)))
            now, bad, ok = total_records(dev)
            if not ok:
                problems.append("msgstat did not answer")
            else:
                if bad:
                    problems.append("%d unreadable record(s)" % bad)
                if not a.flashkb and now < records:
                    problems.append("records lost: %d before, %d now" % (records, now))
                records = now
            if problems:
                raise RuntimeError
            if not a.flashkb and records > a.reset_at:
                # Stay below the 85 % mark: there the firmware removes old messages on purpose
                # and "records only grow" would no longer hold.
                start = dev.mark()
                dev.send("rmconv all")
                for _ in range(PEERS):   # one "[MSG] rmconv <peer>: deleted" line per conversation
                    i, _m = dev.wait_for(r"\[MSG\] rmconv [0-9a-f]{32}: ", 180, start)
                    if i is None:
                        break
                    start = i + 1
                time.sleep(2)
                records, bad, ok = total_records(dev)
                if not ok:
                    problems.append("msgstat did not answer after rmconv")
                    raise RuntimeError
            if dev.proc.poll() is not None:
                problems.append("simulator exited by itself, code 0x%X" % (dev.proc.returncode & 0xFFFFFFFF))
                raise RuntimeError
            # Save burst, then the power cut at a random moment inside it.
            dev.send("hb 1")
            dev.send("msgfill %d %d %d" % (random.randint(40, 200), PEERS, random.choice([20, 120, 600])))
            time.sleep(random.uniform(0.2, 6.0))
            hbs = [l for l in dev.lines if l.startswith("[HB]")]
            confirmed = int(parse_kv(hbs[-1]).get("inc", "0")) if hbs else 0
            if dev.proc.poll() is not None:   # died during the burst (checked build: a trap)
                problems.append("simulator exited by itself during saves, code 0x%X" % (dev.proc.returncode & 0xFFFFFFFF))
                raise RuntimeError
            kills_in_save += 1
            dev.proc.kill()
            dev.proc.wait(10)
            # Saves the heartbeat confirmed must survive the cut (next cycle checks).
            records += confirmed
        except RuntimeError:
            pass
        finally:
            dev.close()
            if problems:
                failures.append((cycle, problems))
                print("cycle %d: FAIL %s" % (cycle, "; ".join(problems)))
                if records and not a.flashkb:
                    pass
            elif not a.keep_logs and os.path.exists(log):
                os.remove(log)
        if cycle % 10 == 0:
            print("%s  cycle %d/%d  records %d  failures %d" % (
                time.strftime("%H:%M:%S"), cycle, a.cycles, records, len(failures)))
        if failures and failures[-1][0] == cycle and any("boot" in p or "start" in p for p in failures[-1][1]):
            break  # a unit that no longer boots: stop and keep the data folder for a look

    lines = ["simulator: %s" % SIM, "power-cut test: %d cycles (%d killed during boot, %d during saves), seed %d%s" % (
        cycle, kills_in_boot, kills_in_save, a.seed, ", flashkb %d" % a.flashkb if a.flashkb else ""),
        "records at the end: %d; boot time %.1f-%.1f s" % (
            records, min(boot_times) if boot_times else 0, max(boot_times) if boot_times else 0)]
    for c, p in failures:
        lines.append("FAIL cycle %d: %s (log cycle_%03d.log)" % (c, "; ".join(p), c))
    lines.append("RESULT: %s" % ("PASS" if not failures else "FAIL"))
    text = "\n".join(lines)
    print(text)
    with open(os.path.join(a.out, "summary.txt"), "w") as f:
        f.write(text + "\n")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
