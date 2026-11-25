#!/usr/bin/env python3
"""Many simulated radios on one channel: does every message still arrive, and how fast?

    python tools/stress/mesh_many.py                  (8 radios, 3 rounds)
    python tools/stress/mesh_many.py --radios 16 --rounds 5 --gap 2
    (from the repo root; RS_SIM_EXE=<ratspeak-sim.exe>, else sim/build/ratspeak-sim.exe)

All radios share one simulated channel (everyone hears everyone; collisions and the
half-duplex radio are simulated by the sim's "air"). Each radio announces, saves the next
one as a contact, and in every round sends it one message (a ring), all rounds started
`--gap` seconds apart per radio, so many are on the air at once. A message counts as
delivered when the receiver logs it within --wait seconds.

Prints per round and in total: delivered / sent, median and worst delivery time, and the
radios that lost something. Exit 0 when at least --pass percent arrived (default 95).
Not part of the regression (it loads the PC for minutes).
"""
import argparse, os, re, statistics, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
os.environ.setdefault("RS_SIM_DATA", os.path.join(tempfile.gettempdir(), "rs_mesh_many"))
os.environ.setdefault("RS_SIM_AIR_PORT", "48463")
sys.path.insert(0, os.path.join(ROOT, "sim", "scripts"))
import groups_net as G


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--radios", type=int, default=8)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--gap", type=float, default=3.0, help="seconds between two radios starting a send")
    ap.add_argument("--wait", type=int, default=240, help="seconds a message may take")
    ap.add_argument("--pass", dest="passpct", type=float, default=95.0)
    a = ap.parse_args()
    os.makedirs(G.DATA, exist_ok=True)
    sims, sent, lost = [], [], {}
    try:
        for i in range(a.radios):
            sims.append(G.Sim("mm_%02d" % i, 5801 + i))
            time.sleep(1.5)
        ids = [s.ident() for s in sims]
        for i, s in enumerate(sims):
            s.send("wiz skip")
            time.sleep(0.5)
            s.send("name R%02d" % i)
        time.sleep(4)
        print("announcing %d radios ..." % a.radios, flush=True)
        for s in sims:                               # everybody announces, spaced out
            s.send("a")
            time.sleep(4)
        time.sleep(15)
        for i, s in enumerate(sims):
            j = (i + 1) % a.radios
            s.send("contact %s R%02d" % (ids[j], j))
            time.sleep(0.3)
            s.send("chat " + ids[j])
        time.sleep(3)
        for rnd in range(a.rounds):
            marks = []
            for i, s in enumerate(sims):
                j = (i + 1) % a.radios
                sims[j].mark()
                text = "r%d from %02d" % (rnd, i)
                s.send("t:" + text)
                marks.append((i, j, text, time.time()))
                time.sleep(a.gap)
            ok, times = 0, []
            for i, j, text, t0 in marks:
                left = max(5, a.wait - (time.time() - t0))
                try:
                    sims[j].expect(r'\[MSG\] in from ' + ids[i] + ': "' + re.escape(text) + '"', left)
                    ok += 1
                    times.append(time.time() - t0)
                except Exception:
                    lost.setdefault(i, []).append(text)
                sent.append((i, j, text))
            print("round %d: %d of %d delivered%s" % (
                rnd + 1, ok, len(marks),
                ", median %.0f s, worst %.0f s" % (statistics.median(times), max(times)) if times else ""), flush=True)
    finally:
        for s in sims:
            try:
                s.close()
            except Exception:
                pass
    n = len(sent)
    bad = sum(len(v) for v in lost.values())
    pct = 100.0 * (n - bad) / n if n else 0.0
    print("TOTAL: %d of %d delivered (%.1f %%), %d radios, gap %.1f s" % (n - bad, n, pct, a.radios, a.gap))
    for i, v in sorted(lost.items()):
        print("  lost from radio %02d: %s" % (i, ", ".join(v)))
    return 0 if n and pct >= a.passpct else 1


if __name__ == "__main__":
    sys.exit(main())
