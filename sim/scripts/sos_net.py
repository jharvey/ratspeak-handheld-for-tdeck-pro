#!/usr/bin/env python3
"""Three-simulator test of SOS, quick replies and notes.

    python sim\\scripts\\sos_net.py          (from the repo root; needs sim\\build, or set
                                              RS_SIM_EXE=<path to ratspeak-sim.exe>)

ANN sends the SOS, BOB and CAT are saved contacts. Steps:
 1 a hold (the top button held 3 s = "btn hold") starts a 5 s countdown; a key cancels it
 2 the countdown runs out: "SOS! ANN needs help. <lat, lon> ..." goes to both contacts;
   each sounds the alarm (even in quiet hours) and a key silences it
 3 a position update reaches the contacts; the status reads ACTIVE
 4 a restart keeps the SOS: it is sent again 30 s after the start
 5 a second hold ends it: "SOS cancelled" reaches the contacts
 6 SOS to a group: only the members get it
 7 quick reply: MIC, Q, 1 in a chat sends "OK"
 8 a new note from the Library is saved as a file
Takes about 6 minutes. Exit 0 = all steps passed. Logs: %TEMP%\\rs_sos_net\\<name>.log
"""
import os, sys, tempfile, time

os.environ.setdefault("RS_SIM_DATA", os.path.join(tempfile.gettempdir(), "rs_sos_net"))
os.environ.setdefault("RS_SIM_AIR_PORT", "48447")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import groups_net as G                                   # Sim, step, results


def main():
    os.makedirs(G.DATA, exist_ok=True)
    for f in os.listdir(G.DATA):
        if f.endswith(".log"):
            os.remove(os.path.join(G.DATA, f))
    sims = {}
    try:
        for i, n in enumerate(("ann", "bob", "cat")):
            sims[n] = G.Sim("sosnet_" + n, 5741 + i)
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
        b.send("quiet hour 23")                  # BOB is in quiet hours: the alarm still sounds
        b.send("quiet 1")

        def s1():
            a.mark()
            a.send("btn hold")
            a.expect(r"\[SOS\] countdown started \(console\)", 10)
            time.sleep(1.5)
            a.send("k x")
            a.expect(r"\[SOS\] countdown cancelled \(key\)", 10)
            time.sleep(6)
            a.ask("sos", r"\[SOS\] off; sends to saved contacts .* 0 sent", 10)
        G.step(1, "a hold starts the countdown; a key cancels it", s1)

        def s2():
            a.mark(); b.mark(); c.mark()
            a.send("btn hold")
            a.expect(r"\[SOS\] countdown started", 10)
            a.expect(r"\[SOS\] sent to 2 of 2 \(saved contacts .*\): SOS! ANN needs help\.", 15)
            b.expect(r"\[SOS\] ALARM from ANN", 150)
            c.expect(r"\[SOS\] ALARM from ANN", 150)
            time.sleep(4)
            b.send("k x")
            b.expect(r"\[SOS\] alarm silenced \(key\)", 10)
        G.step(2, "SOS sent to both contacts, alarm in quiet hours, a key silences it", s2)

        def s3():
            a.mark(); b.mark(); c.mark()
            a.send("sos update")
            a.expect(r"\[SOS\] update to 2 of 2 .*SOS update: ", 10)
            b.expect(r"\[SOS\] update from ANN", 150)
            c.expect(r"\[SOS\] update from ANN", 150)
            a.ask("sos", r"\[SOS\] ACTIVE; sends to saved contacts", 10)
        G.step(3, "a position update reaches the contacts", s3)

        def s4():
            a.close()
            time.sleep(3)
            na = G.Sim("sosnet_ann", 5751, fresh=False)
            sims["ann"] = na
            na.ident()
            na.mark()                                # the boot line may come before the socket
            na.ask("sos", r"\[SOS\] ACTIVE", 20)
            na.expect(r"\[SOS\] sent to 2 of 2", 60)
        G.step(4, "a restart keeps the SOS and sends it again", s4)

        def s5():
            a2 = sims["ann"]
            a2.mark(); b.mark(); c.mark()
            a2.send("btn hold")
            a2.expect(r"\[SOS\] cancel to 2 of 2 .*SOS cancelled: ANN is OK now\.", 10)
            a2.expect(r"\[SOS\] off \(console\)", 10)
            b.expect(r"\[SOS\] ANN: cancelled", 150)
            c.expect(r"\[SOS\] ANN: cancelled", 150)
            a2.ask("sos", r"\[SOS\] off;", 10)
        G.step(5, "a second hold ends the SOS", s5)

        def s6():
            a2 = sims["ann"]
            a2.send("grp new TEAM")
            a2.send("grp add 1 " + h["bob"])
            a2.ask("grp", r'\[GRP\] 1: "TEAM" #[0-9a-f]{8} 1 members', 20)
            time.sleep(3)
            a2.send("sos to g1")
            a2.expect(r"\[SOS\] off; sends to group TEAM", 10)
            a2.mark(); b.mark(); c.mark()
            a2.send("sos now")
            a2.expect(r"\[SOS\] sent to 1 of 1 \(group TEAM\)", 15)
            b.expect(r"\[SOS\] ALARM from ANN", 200)
            time.sleep(20)
            with c.lock:
                if "[SOS] ALARM" in c.buf[c.pos:]:
                    raise AssertionError("CAT is not in TEAM but got the SOS")
            b.send("k x")
            a2.send("sos off")
            a2.expect(r"\[SOS\] off \(console\)", 10)
            a2.send("sos to all")
        G.step(6, "SOS to a group: only the members get it", s6)

        def s7():
            a2 = sims["ann"]
            a2.mark(); b.mark()
            b.send("k ^")
            time.sleep(2)
            b.send("chat " + h["ann"])
            time.sleep(2)
            b.send("k !")
            time.sleep(1.5)
            b.send("k Q")
            time.sleep(1.5)
            b.send("k 1")
            b.expect(r"\[MSG\] quick reply 1: OK", 10)
            a2.expect(r'\[MSG\] in from %s: "OK"' % h["bob"], 150)
            b.send("k ^")
        G.step(7, "quick reply: MIC, Q, 1 sends OK", s7)

        def s8():
            c.mark()
            c.send("k ^")
            time.sleep(2)
            c.send("libnote")
            time.sleep(2)
            c.send("k Meet at the bridge 5pm~")
            c.expect(r"\[LIB\] note saved: Note-\d{8}-\d{6}\.txt \(22 chars\)", 15)
            c.send("lib")
            c.expect(r"\[LIB\] \d+ files", 10)
        G.step(8, "a new note from the Library is saved as a file", s8)
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
