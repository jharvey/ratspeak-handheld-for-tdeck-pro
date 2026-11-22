#!/usr/bin/env python3
"""Three-simulator test of status sharing (position / battery to a contact or a group).

    python sim\\scripts\\status_net.py        (from the repo root; needs sim\\build, or set
                                               RS_SIM_EXE=<path to ratspeak-sim.exe>)

ANN shares, BOB and CAT receive. Steps:
 1 automatic sharing is off by default
 2 one-time position            3 one-time battery           4 one-time both
 5 automatic, both: first update at once, no new-message alert on the receiver
 6 the change threshold: nothing changed = nothing at the next interval, a keep-alive at the
   second; battery -8 points = sent; battery -2 = not sent; moved 120 m = sent
 7 automatic battery only (CAT: marked read at once) and position only (BOB)
 8 standby: the GPS is off, the battery goes alone
 9 a group: one-time and automatic, each member gets it
10 a restart keeps the automatic shares; "ashare off" stops them all
11 the receiver has the sender's position and battery ("cpos")
Uses the simulator-only console commands "fake bat <pct>" and "fake move <metres north>".
Takes about 15 minutes. Exit 0 = all steps passed. Logs: %TEMP%\\rs_status_net\\<name>.log
"""
import os, sys, tempfile, time

os.environ.setdefault("RS_SIM_DATA", os.path.join(tempfile.gettempdir(), "rs_status_net"))
os.environ.setdefault("RS_SIM_AIR_PORT", "48437")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import groups_net as G                                   # Sim, step, results

SENT = r"(sent|queued)"


def absent(s, pattern, seconds):
    """pattern must not appear within seconds (from the last mark / match)."""
    try:
        s.expect(pattern, seconds)
    except RuntimeError:
        return
    raise RuntimeError("%s: seen but must not be: %s" % (s.name, pattern))


def main():
    os.makedirs(G.DATA, exist_ok=True)
    for f in os.listdir(G.DATA):
        if f.endswith(".log"):
            os.remove(os.path.join(G.DATA, f))
    sims = {}
    try:
        for i, n in enumerate(("ann", "bob", "cat")):
            sims[n] = G.Sim("stnet_" + n, 5641 + i)
        a, b, c = sims["ann"], sims["bob"], sims["cat"]
        h = {n: s.ident() for n, s in sims.items()}
        for n, s in sims.items():
            s.send("wiz skip")
            time.sleep(1)
            s.send("name " + n.upper())
        print("radios:", h, flush=True)
        time.sleep(6)
        for s in sims.values():
            s.send("a")
            time.sleep(5)
        time.sleep(15)
        for n, s in sims.items():
            for m in sims:
                if m != n:
                    s.send("contact %s %s" % (h[m], m.upper()))
        time.sleep(3)
        A = h["ann"]

        def s1():
            a.ask("loc", r"\[LOC\] auto-share: 0 on", 20)
        G.step(1, "automatic sharing is off by default", s1)

        def s2():
            a.mark(); b.mark()
            a.send("locshare %s p" % h["bob"])
            a.expect(r"\[LOC\] share " + SENT + r": My location: 43\.653\d\d, -79\.383\d\d \([^)]*\)\r?\n", 30)
            b.expect(r"\[LOC\] ANN at 43\.653", 150)
            b.mark()
            b.send("cpos")
            b.expect(r"\[LOC\] cpos #\d+ " + A, 10)
            absent(b, r"\[LOC\] cbat #\d+ " + A, 3)
        G.step(2, "one-time position: on the receiver's map, no battery", s2)

        def s3():
            a.send("fake bat 61")
            time.sleep(5)
            a.mark(); b.mark()
            a.send("locshare %s b" % h["bob"])
            a.expect(r"\[LOC\] share " + SENT + r": My battery: 61% \(\d\d:\d\d\)", 30)
            b.expect(r"\[LOC\] ANN battery 61%", 150)
        G.step(3, "one-time battery", s3)

        def s4():
            a.send("fake bat 58")
            time.sleep(5)
            a.mark(); b.mark()
            a.send("locshare %s pb" % h["bob"])
            a.expect(r"\[LOC\] share " + SENT + r": My location: 43\.653\d\d, -79\.383\d\d \([^)]*\)\. Battery 58%", 30)
            b.expect(r"\[LOC\] ANN battery 58%", 150)
        G.step(4, "one-time position + battery", s4)

        def s5():
            time.sleep(5)
            a.mark(); b.mark()
            a.send("ashare %s 1 pb" % h["bob"])
            a.expect(r"\[LOC\] auto-share every 1 min, position \+ battery", 10)
            a.expect(r"\[LOC\] auto " + SENT + r": Loc: 43\.653\d\d, -79\.383\d\d \d\d:\d\d Bat 58%", 40)
            b.expect(r'\[MSG\] in from ' + A + r': "Loc: ', 150)
            absent(b, r"\[MSG\] inbound message", 6)
        G.step(5, "automatic, both: first update at once, no alert on the receiver", s5)

        def s6():
            # last update just went (T0). Nothing changes: none at T0+60 s, a keep-alive by T0+120 s.
            a.mark()
            absent(a, r"\[LOC\] auto " + SENT, 80)
            a.expect(r"\[LOC\] auto " + SENT + r": Loc: .* Bat 58%", 70)
            a.send("fake bat 50")                    # -8 points: goes at the next interval
            a.expect(r"\[LOC\] auto " + SENT + r": Loc: .* Bat 50%", 90)
            a.send("fake bat 48")                    # -2 points: not enough
            absent(a, r"\[LOC\] auto " + SENT, 85)
            b.mark()
            a.send("fake move 120")                  # 120 m north: goes at once (an interval has passed)
            a.expect(r"\[LOC\] auto " + SENT + r": Loc: 43\.654\d\d, .* Bat 48%", 40)
            b.expect(r"\[LOC\] ANN at 43\.654", 150)
        G.step(6, "change threshold (battery 5 points, 50 m, keep-alive at 2 intervals)", s6)

        def s7():
            a.mark(); b.mark(); c.mark()
            a.send("ashare %s 1 b" % h["cat"])
            a.expect(r"\[LOC\] auto " + SENT + r": Bat: 48% \d\d:\d\d", 40)
            c.expect(r"\[LOC\] ANN battery 48%", 150)
            c.expect(r"\[LOC\] status update: no alert, marked read", 20)
            a.send("ashare %s 1 p" % h["bob"])
            a.send("fake move 300")
            a.expect(r"\[LOC\] auto " + SENT + r": Loc: 43\.655\d\d, -79\.383\d\d \d\d:\d\d\r?\n", 150)
        G.step(7, "automatic battery only (marked read on the receiver) and position only", s7)

        def s8():
            a.send("ashare %s 1 pb" % h["bob"])
            time.sleep(2)
            a.send("fake bat 30")
            a.send("standby now")
            time.sleep(3)
            a.mark(); b.mark()
            a.expect(r"\[LOC\] auto " + SENT + r": Bat: 30% \d\d:\d\d", 200)
            a.expect(r"\[LOC\] auto " + SENT + r": Bat: 30% \d\d:\d\d", 200)   # BOB's and CAT's
            b.expect(r"\[LOC\] ANN battery 30%", 200)
            a.send("standby wake")
            time.sleep(3)
        G.step(8, "standby: GPS off, the battery goes alone", s8)

        def s9():
            a.send("ashare off")
            time.sleep(2)
            a.send("grp new TEAM")
            a.send("grp add 1 " + h["bob"])
            a.send("grp add 1 " + h["cat"])
            a.ask("grp", r'\[GRP\] 1: "TEAM" #[0-9a-f]{8} 2 members', 20)
            a.send("fake bat 77")
            time.sleep(5)
            a.mark(); b.mark(); c.mark()
            a.send("locshare g1 pb")
            a.expect(r"\[LOC\] share " + SENT + r": My .* Battery 77%", 30)
            b.expect(r"\[LOC\] ANN battery 77%", 200)
            c.expect(r"\[LOC\] ANN battery 77%", 200)
            a.send("fake bat 70")
            time.sleep(12)
            a.mark(); b.mark(); c.mark()
            a.send("ashare g1 1 pb")
            a.expect(r"\[LOC\] auto " + SENT + r": Loc: .* Bat 70%", 60)
            b.expect(r"\[LOC\] ANN battery 70%", 200)
            c.expect(r"\[LOC\] ANN battery 70%", 200)
        G.step(9, "a group: one-time and automatic reach every member", s9)

        def s10():
            a.send("ashare %s 1 b" % h["cat"])
            a.ask("loc", r"\[LOC\] auto-share: 2 on", 20)
            a.close()
            time.sleep(3)
            na = G.Sim("stnet_ann", 5651, fresh=False)
            sims["ann"] = na
            na.ident()
            na.ask("loc", r"\[LOC\] auto " + h["cat"] + r" every 1 min battery ", 30)
            na.ask("loc", r"\[LOC\] auto [0-9a-f]{32} every 1 min position \+ battery ", 30)
            na.ask("loc", r"\[LOC\] auto-share: 2 on", 20)
            na.send("ashare off")
            na.ask("loc", r"\[LOC\] auto-share: 0 on", 20)
        G.step(10, "a restart keeps the automatic shares; all can be stopped", s10)

        def s11():
            m = b.ask("cpos", r"\[LOC\] cpos #\d+ " + A + r" ANN 43\.65\d+,-79\.38\d+ at \d+", 20)
            b.ask("cpos", r"\[LOC\] cbat #\d+ " + A + r" \d+% at \d+", 20)
            c.ask("cpos", r"\[LOC\] cbat #\d+ " + A + r" 70% at \d+", 20)
        G.step(11, "the receivers hold the sender's position and battery", s11)
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
