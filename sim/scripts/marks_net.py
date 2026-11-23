#!/usr/bin/env python3
"""Three-simulator test of markers (items of interest, ATAK-style).

    python sim\\scripts\\marks_net.py         (from the repo root; needs sim\\build, or set
                                               RS_SIM_EXE=<path to ratspeak-sim.exe>)

ANN makes markers, BOB and CAT receive. Steps:
 1 no markers at first
 2 a marker at a point, and one by distance and direction from the own position
 3 sent to a contact: the receiver stores it (from ANN) and the chat row reads
   "[marker] hazard ... of you: bridge out"
 4 the same marker again (same maker, type, within 15 m) updates it, no second copy
 5 sent to a group: every member stores it
 6 the marker pages: list, one marker, send page, new-marker form, nearby contacts
 7 a restart keeps the markers
 8 full (24): a new one replaces the oldest received one, then the oldest; delete one / all
Takes about 8 minutes. Exit 0 = all steps passed. Logs: %TEMP%\\rs_marks_net\\<name>.log
"""
import os, sys, tempfile, time

os.environ.setdefault("RS_SIM_DATA", os.path.join(tempfile.gettempdir(), "rs_marks_net"))
os.environ.setdefault("RS_SIM_AIR_PORT", "48439")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import groups_net as G                                   # Sim, step, results

SENT = r"(sent|queued)"


def main():
    os.makedirs(G.DATA, exist_ok=True)
    for f in os.listdir(G.DATA):
        if f.endswith(".log"):
            os.remove(os.path.join(G.DATA, f))
    sims = {}
    try:
        for i, n in enumerate(("ann", "bob", "cat")):
            sims[n] = G.Sim("mknet_" + n, 5661 + i)
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
            a.ask("mark", r"\[MARK\] 0 markers", 20)
        G.step(1, "no markers at first", s1)

        def s2():
            a.mark()
            a.send("mark new hazard 43.65800 -79.38300 bridge out")
            a.expect(r'\[MARK\] #1 hazard at 43\.65800,-79\.38300 "bridge out" from this radio: [\d.]+ (m|ft|km|mi) N of you', 10)
            a.send("mark rel camp 640 45 tents")
            a.expect(r'\[MARK\] #2 camp at 43\.65\d+,-79\.37\d+ "tents" from this radio: [\d.]+ (m|ft|km|mi) NE of you', 10)
            a.ask("mark", r"\[MARK\] 2 markers", 10)
        G.step(2, "a marker at a point, and one by distance and direction", s2)

        def s3():
            a.mark(); b.mark()
            a.send("mark send 1 " + h["bob"])
            a.expect(r"\[MARK\] send #1 to BOB: " + SENT, 20)
            b.expect(r'\[MARK\] #1 hazard at 43\.65800,-79\.38300 "bridge out" from ANN: [\d.]+ (m|ft|km|mi) N of you', 150)
            b.send("chat " + A)
            time.sleep(2)
            b.ask("grp hist", r'\[GRP\] row \d+ in "\[marker\] hazard [\d.]+ (m|ft|km|mi) N of you: bridge out"', 30)
            b.send("k ^")
        G.step(3, "sent to a contact: stored, chat row \"[marker] ... of you: note\"", s3)

        def s4():
            a.mark(); b.mark()
            a.send("mark new hazard 43.65805 -79.38300 bridge gone")
            a.expect(r'\[MARK\] #1 hazard .* "bridge gone"', 10)
            a.ask("mark", r"\[MARK\] 2 markers", 10)
            a.send("mark send 1 " + h["bob"])
            b.expect(r'\[MARK\] #1 hazard .* "bridge gone" from ANN', 150)
            b.ask("mark", r"\[MARK\] 1 markers", 10)
        G.step(4, "the same marker again updates it, no second copy", s4)

        def s5():
            a.send("grp new TEAM")
            a.send("grp add 1 " + h["bob"])
            a.send("grp add 1 " + h["cat"])
            a.ask("grp", r'\[GRP\] 1: "TEAM" #[0-9a-f]{8} 2 members', 20)
            time.sleep(3)
            a.mark(); b.mark(); c.mark()
            a.send("mark send 2 g1")
            a.expect(r"\[MARK\] send #2 to TEAM: " + SENT, 20)
            b.expect(r'\[MARK\] #2 camp .* "tents" from ANN', 200)
            c.expect(r'\[MARK\] #1 camp .* "tents" from ANN', 200)
        G.step(5, "sent to a group: every member stores it", s5)

        def s6():
            a.send("locshare %s p" % h["bob"])      # BOB then has ANN's position for "nearby"
            b.expect(r"\[LOC\] ANN at 43\.653", 150)
            b.send("k ^")                            # wake the idle screen (that key is used up)
            time.sleep(2)
            b.mark()
            b.send("mark list")
            b.expect(r"\[MARK\] list \(2 markers\)", 10)
            b.send("k J")
            b.expect(r"\[MARK\] nearby \(1 contacts\)", 10)
            b.ask("mark near", r"\[MARK\] near 1: ANN [\d.]+ (m|ft|km|mi)", 10)
            b.send("k J")
            b.expect(r"\[MARK\] list \(2 markers\)", 10)
            a.send("k ^")
            time.sleep(2)
            a.mark()
            a.send("mark list")
            a.expect(r"\[MARK\] list \(2 markers\)", 10)
            a.send("k 3~")                           # row 3 = "New marker"
            a.expect(r"\[MARK\] new \(from my position\)", 10)
            a.send("k 1")                            # type: hazard -> person
            a.send("k 6")                            # save: 200 m N
            a.expect(r'\[MARK\] #3 person at 43\.65\d+,-79\.383\d+ "" from this radio: [\d.]+ (m|ft|km|mi) N of you', 10)
            a.send("k 2")                            # one marker: row 2 = Send
            time.sleep(1)
            b.mark()
            a.send("k ~")                            # first row of the send page: group TEAM
            a.expect(r"\[MARK\] send #3 to TEAM: " + SENT, 20)
            b.expect(r'\[MARK\] #3 person .* from ANN', 200)
            a.send("k ^")
            a.send("k ^")
            time.sleep(1)
        G.step(6, "marker pages: list, nearby, new-marker form, send page", s6)

        def s7():
            b.close()
            time.sleep(3)
            nb = G.Sim("mknet_bob", 5671, fresh=False)
            sims["bob"] = nb
            nb.ident()
            nb.ask("mark", r"\[MARK\] 3 markers", 30)
        G.step(7, "a restart keeps the markers", s7)

        def s8():
            nb = sims["bob"]
            for k in range(21):                       # 3 received + 21 own = 24 (full)
                nb.send("mark new water 44.%05d -79.00000 w%d" % (k * 100, k))
                time.sleep(0.3)
            nb.ask("mark", r"\[MARK\] 24 markers", 20)
            nb.mark()
            nb.send("mark new other 45.00000 -79.00000 extra")
            nb.expect(r'\[MARK\] #1 other at 45\.00000', 10)   # slot 1 = the oldest received (hazard)
            nb.ask("mark", r"\[MARK\] 24 markers", 20)
            nb.send("mark del 1")
            nb.ask("mark", r"\[MARK\] 23 markers", 20)
            nb.send("mark del all")
            nb.ask("mark", r"\[MARK\] 0 markers", 20)
        G.step(8, "full: the oldest received one makes room; delete one and all", s8)
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
