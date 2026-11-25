#!/usr/bin/env python3
"""Two-simulator test: a message to a contact that is switched off is kept and sent again
by itself when that contact is heard again.

    python sim\\scripts\\held_net.py     (from the repo root; RS_SIM_EXE=<ratspeak-sim.exe>)

ANN and BOB know each other. BOB's simulator is closed (the radio is off). ANN sends: the
message fails or gets no proof and is kept ("[HELD] kept"). BOB starts again and announces:
ANN resends, BOB shows the message, ANN's list is empty again. Exit 0 = PASS.
"""
import os, sys, tempfile, time

os.environ.setdefault("RS_SIM_DATA", os.path.join(tempfile.gettempdir(), "rs_held_net"))
os.environ.setdefault("RS_SIM_AIR_PORT", "48449")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import groups_net as G


def main():
    os.makedirs(G.DATA, exist_ok=True)
    sims = {}
    try:
        for i, n in enumerate(("ann", "bob")):
            sims[n] = G.Sim("hd_" + n, 5791 + i)
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
            a.ask("held", r"\[HELD\] on, 0 waiting", 10)
            b.close()                                # BOB is switched off
            time.sleep(3)
            a.send("chat " + h["bob"])
            time.sleep(2)
            a.mark()
            a.send("t:call home tonight")
            a.expect(r'\[HELD\] kept for BOB \((not reached|no delivery proof)\): "call home tonight"', 400)
            a.ask("held", r"\[HELD\] on, 1 waiting", 10)
        G.step(1, "contact off: the message is kept", s1)

        def s2():
            a.mark()
            sims["bob"] = G.Sim("hd_bob", 5792, fresh=False)   # BOB switches on: announces at start
            bb = sims["bob"]
            bb.ident()
            time.sleep(8)
            bb.mark()
            bb.send("a")
            a.expect(r'\[HELD\] BOB was heard: resent \(try 1\): "call home tonight"', 200)
            bb.expect(r'\[MSG\] in from ' + h["ann"] + r': "call home tonight"', 200)
            a.expect(r'\[HELD\] delivered to BOB after 1 resend', 200)
            a.ask("held", r"\[HELD\] on, 0 waiting", 10)
        G.step(2, "contact back on: resent, delivered, list empty", s2)

        def s3():                                    # turned off: a failure is not kept
            a.send("held off")
            sims["bob"].close()
            time.sleep(3)
            a.mark()
            a.send("t:not kept")
            time.sleep(150)
            a.ask("held", r"\[HELD\] off, 0 waiting", 10)
        G.step(3, "held off: nothing is kept", s3)
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
