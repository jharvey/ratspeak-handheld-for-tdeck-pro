#!/usr/bin/env python3
"""Three-simulator test of relay mode (a Reticulum transport node on BOB).

    python sim\\scripts\\relay_net.py         (from the repo root; RS_SIM_EXE=<ratspeak-sim.exe>)

ANN and CAT are out of each other's range (each simulator's air_deny.txt lists the other's
air id); BOB hears both. Steps:
 1 relay off: ANN does not hear CAT's announce
 2 BOB "relay 2": the relay comes on (after the 10 s settle)
 3 CAT announces again: ANN now hears it through BOB (2 hops)
 4 ANN sends CAT a message: it arrives through BOB, and ANN sees it delivered
 5 BOB "relay 0": the relay goes off
Exit 0 = all passed. Logs: %TEMP%\\rs_relay_net\\<name>.log
"""
import os, sys, tempfile, time

os.environ.setdefault("RS_SIM_DATA", os.path.join(tempfile.gettempdir(), "rs_relay_net"))
os.environ.setdefault("RS_SIM_AIR_PORT", "48443")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import groups_net as G                                   # Sim, step, results


def absent(s, pattern, seconds):
    try:
        s.expect(pattern, seconds)
    except RuntimeError:
        return
    raise RuntimeError("%s: seen but must not be: %s" % (s.name, pattern))


def air_id(s):
    end = time.time() + 30
    p = os.path.join(s.data, "air_id.txt")
    while time.time() < end:
        if os.path.exists(p):
            return open(p).read().strip()
        time.sleep(0.5)
    raise RuntimeError(s.name + ": no air_id.txt")


def main():
    os.makedirs(G.DATA, exist_ok=True)
    for f in os.listdir(G.DATA):
        if f.endswith(".log"):
            os.remove(os.path.join(G.DATA, f))
    sims = {}
    try:
        for i, n in enumerate(("ann", "bob", "cat")):
            sims[n] = G.Sim("rl_" + n, 5701 + i)
        a, b, c = sims["ann"], sims["bob"], sims["cat"]
        ids = {n: air_id(s) for n, s in sims.items()}
        open(os.path.join(a.data, "air_deny.txt"), "w").write(ids["cat"] + "\n")   # ANN and CAT: out of range
        open(os.path.join(c.data, "air_deny.txt"), "w").write(ids["ann"] + "\n")
        h = {n: s.ident() for n, s in sims.items()}
        for n, s in sims.items():
            s.send("wiz skip")
            time.sleep(1)
            s.send("name " + n.upper())
        print("radios:", h, "air:", ids, flush=True)
        time.sleep(3)

        def s1():
            a.mark()
            c.send("a")
            b.expect(r"new peer " + h["cat"], 60)
            absent(a, h["cat"], 30)
        G.step(1, "relay off: ANN does not hear CAT", s1)

        def s2():
            b.mark()
            b.send("relay 2")
            b.expect(r"\[RELAY\] on: relaying for other units", 30)
            b.ask("relay", r"\[RELAY\] setting: Always, relaying yes", 10)
        G.step(2, "BOB relay on", s2)

        def s3():
            a.mark()
            time.sleep(5)
            c.send("a")
            try:
                a.expect(r"new peer " + h["cat"] + r".*hops=2", 120)
                c.mark()                             # and the other way: CAT learns ANN through BOB
                time.sleep(5)
                a.send("a")
                c.expect(r"new peer " + h["ann"] + r".*hops=2", 120)
            finally:
                print("   BOB:", b.ask("relay", r"\[RELAY\] node: [^\r\n]*", 10).group(0), flush=True)
        G.step(3, "CAT's announce reaches ANN through BOB (2 hops)", s3)

        def s4():
            a.send("contact %s CAT" % h["cat"])
            c.send("contact %s ANN" % h["ann"])
            time.sleep(2)
            a.mark(); c.mark()
            a.send("chat " + h["cat"])
            time.sleep(2)
            a.send("t:hello via bob")
            c.expect(r'\[MSG\] in from ' + h["ann"] + r': "hello via bob"', 240)
            a.ask("grp hist", r'\[GRP\] row \d+ out "hello via bob" \[delivered', 180, every=10)
        G.step(4, "ANN -> CAT through BOB, delivered", s4)

        def s5():
            b.mark()
            b.send("relay 0")
            b.expect(r"\[RELAY\] off", 30)
        G.step(5, "BOB relay off", s5)
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
