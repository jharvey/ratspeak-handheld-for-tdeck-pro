#!/usr/bin/env python3
"""Two-simulator test: a message that carries only Sideband's telemetry field (no position
text) puts the sender's position and battery on the receiver (Atlas, contact page).

    python sim\\scripts\\telemetry_net.py     (from the repo root; RS_SIM_EXE=<ratspeak-sim.exe>)

ANN sends with the bench command "telesend" (the telemetry entry is built the same way a
one-time share builds it, as Sideband's Telemeter.packed). Exit 0 = PASS.
"""
import os, sys, tempfile, time

os.environ.setdefault("RS_SIM_DATA", os.path.join(tempfile.gettempdir(), "rs_telemetry_net"))
os.environ.setdefault("RS_SIM_AIR_PORT", "48445")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import groups_net as G


def main():
    os.makedirs(G.DATA, exist_ok=True)
    sims = {}
    try:
        for i, n in enumerate(("ann", "bob")):
            sims[n] = G.Sim("tl_" + n, 5721 + i)
        a, b = sims["ann"], sims["bob"]
        h = {n: s.ident() for n, s in sims.items()}
        for n, s in sims.items():
            s.send("wiz skip")
            time.sleep(1)
            s.send("name " + n.upper())
        time.sleep(4)
        for s in sims.values():
            s.send("a")
            time.sleep(5)
        time.sleep(10)
        a.send("contact %s BOB" % h["bob"])
        b.send("contact %s ANN" % h["ann"])
        time.sleep(2)

        def s1():
            b.mark()
            a.send("telesend %s 43.70000 -79.40000 55" % h["bob"])
            b.expect(r"\[LOC\] telemetry from ANN: position battery", 150)
            b.expect(r"\[LOC\] ANN at 43\.70000,-79\.40000", 10)
            b.ask("cpos", r"\[LOC\] cpos #\d+ " + h["ann"] + r" ANN 43\.70000,-79\.40000", 20)
            b.ask("cpos", r"\[LOC\] cbat #\d+ " + h["ann"] + r" 55% at \d+", 20)
        G.step(1, "telemetry only: position and battery stored", s1)
    except Exception as e:
        G.results.append((0, "setup", False, str(e)))
        print("FAIL setup:", e, flush=True)
    finally:
        for s in sims.values():
            try:
                s.close()
            except Exception:
                pass
    bad = [r for r in G.results if not r[2]]
    print("%d of %d steps passed" % (len(G.results) - len(bad), len(G.results)))
    return 1 if bad or not G.results else 0


if __name__ == "__main__":
    sys.exit(main())
