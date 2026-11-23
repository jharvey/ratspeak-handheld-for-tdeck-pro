#!/usr/bin/env python3
"""Two-simulator test of the ATAK feed (Cursor-on-Target XML over WiFi UDP).

    python sim\\scripts\\cot_net.py           (from the repo root; needs sim\\build, or set
                                               RS_SIM_EXE=<path to ratspeak-sim.exe>)

ANN sends the feed, BOB plays the ATAK phone ("cot listen 6969" prints each event that
arrives, and its XML). The simulated WiFi delivers only to a unit's own address, so ANN sends
to BOB's address here; on real WiFi the default is ATAK's multicast group 239.2.3.1:6969.
Steps:
 1 the feed is off by default
 2 "cot now": ANN itself (friendly unit, battery), a contact's shared position and a marker
   arrive; every event is well-formed CoT XML with the right type, callsign and point
 3 the feed on: events come every interval by themselves; off: they stop
 4 receive: a phone running ATAK (played by "cot fake" on ANN) is listed on BOB with its
   position; its marker becomes a marker from ATAK (a repeat adds nothing); BOB does not
   send that marker back to ATAK
 5 a restart keeps the settings
Takes about 4 minutes. Exit 0 = all steps passed. Logs: %TEMP%\\rs_cot_net\\<name>.log
"""
import os, re, sys, tempfile, time
import xml.etree.ElementTree as ET

os.environ.setdefault("RS_SIM_DATA", os.path.join(tempfile.gettempdir(), "rs_cot_net"))
os.environ.setdefault("RS_SIM_AIR_PORT", "48441")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import groups_net as G                                   # Sim, step, results


def absent(s, pattern, seconds):
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
        for i, n in enumerate(("ann", "bob")):
            sims[n] = G.Sim("cot_" + n, 5681 + i)
        a, b = sims["ann"], sims["bob"]
        h = {n: s.ident() for n, s in sims.items()}
        for n, s in sims.items():
            s.send("wiz skip")
            time.sleep(1)
            s.send("name " + n.upper())
        print("radios:", h, flush=True)
        time.sleep(4)
        for s in sims.values():
            s.send("a")
            time.sleep(4)
        time.sleep(10)
        a.send("contact %s BOB" % h["bob"])
        b.send("contact %s ANN" % h["ann"])
        a.send("wifi join HomeNet password123")
        b.send("wifi join HomeNet password123")
        aip = a.expect(r'\[WIFI\] connected "HomeNet" ip (\d+\.\d+\.\d+\.\d+)', 30).group(1)
        bip = b.expect(r'\[WIFI\] connected "HomeNet" ip (\d+\.\d+\.\d+\.\d+)', 30).group(1)
        print("BOB ip", bip, flush=True)
        b.send("cot listen 6969")
        b.expect(r"\[COT\] listen on", 10)

        def s1():
            a.ask("cot", r"\[COT\] feed off, every 30 s, to 239\.2\.3\.1:6969", 10)
        G.step(1, "the feed is off by default", s1)

        def s2():
            b.send("locshare %s p" % h["ann"])           # ANN then holds BOB's position
            a.expect(r"\[LOC\] BOB at 43\.65", 150)
            a.send("mark del all")
            a.send("mark new hazard 43.65800 -79.38300 bridge <out> & \"gone\"")
            a.expect(r"\[MARK\] #1 hazard", 10)
            a.send("cot to %s 6969" % bip)
            a.ask("cot", r"\[COT\] feed off, every 30 s, to " + re.escape(bip) + r":6969", 10)
            b.mark()
            a.send("cot now")
            a.expect(r"\[COT\] sent 3 events to " + re.escape(bip), 10)
            seen = {}
            for _ in range(3):
                m = b.expect(r"\[COT\] xml (<\?xml.*</event>)\r?\n", 20)
                ev = ET.fromstring(m.group(1).encode())            # well-formed, or this raises
                cs = ev.find("detail/contact").get("callsign")
                pt = ev.find("point")
                seen[cs] = (ev.get("type"), float(pt.get("lat")), float(pt.get("lon")), ev)
            print("  events:", {k: v[0] for k, v in seen.items()}, flush=True)
            assert "ANN" in seen and seen["ANN"][0] == "a-f-G-U-C", "ANN itself missing"
            assert seen["ANN"][3].find("detail/status").get("battery") is not None, "no battery"
            assert "BOB" in seen and seen["BOB"][0] == "a-f-G-U-C", "contact BOB missing"
            mk = [k for k in seen if k.startswith("hazard: ")]
            assert mk and seen[mk[0]][0] == "b-m-p-s-m", "marker missing"
            assert mk[0] == 'hazard: bridge <out> & "gone"', "callsign not escaped right: %r" % mk[0]
            assert abs(seen[mk[0]][1] - 43.658) < 1e-4 and abs(seen[mk[0]][2] + 79.383) < 1e-4
            assert seen[mk[0]][3].find("detail/color").get("argb") == str(-65536), "hazard not red"
            uid = seen["ANN"][3].get("uid")
            assert uid == "RatSpeak-" + h["ann"][:16], "uid %s" % uid
        G.step(2, "cot now: self, contact and marker as well-formed CoT XML", s2)

        def s3():
            a.send("cot every 5")
            a.send("cot on")
            b.mark()
            b.expect(r"\[COT\] rx from .* a-f-G-U-C \"ANN\"", 20)
            b.expect(r"\[COT\] rx from .* a-f-G-U-C \"ANN\"", 20)   # and again an interval later
            a.send("cot off")
            time.sleep(6)
            b.mark()
            absent(b, r"\[COT\] rx from", 15)
        G.step(3, "on: events every interval; off: they stop", s3)

        def s4():
            b.mark()
            a.send("cot fake %s u ALPHA_1 43.65500 -79.38000" % bip)
            b.expect(r'\[COT\] ATAK unit "ALPHA 1" 43\.65500,-79\.38000', 20)
            b.ask("cot atak", r'\[COT\] atak 1: "ALPHA 1" 43\.65500,-79\.38000', 10)
            b.mark()
            a.send("cot fake %s m camp:_river_camp 43.66000 -79.39000" % bip)
            b.expect(r'\[MARK\] #\d+ camp at 43\.66000,-79\.39000 "river camp" from ATAK', 20)
            b.mark()
            a.send("cot fake %s m camp:_river_camp 43.66000 -79.39000" % bip)   # the same again
            b.expect(r'\[COT\] rx from .* ANDROID-camp:-river-camp', 20)
            absent(b, r'\[MARK\] #\d+ camp', 5)
            b.ask("mark", r'\[MARK\] \d+: #\d+ camp 43\.66000,-79\.39000 "river camp" ATAK', 10)
            # BOB's own feed (to ANN) must not carry the marker that came from ATAK
            a.send("cot listen 6969")
            a.expect(r"\[COT\] listen on", 10)
            b.send("cot to %s 6969" % aip)
            a.mark()
            b.send("cot now")
            b.expect(r"\[COT\] sent \d+ events", 10)
            a.expect(r'\[COT\] rx from .* a-f-G-U-C "BOB"', 20)
            absent(a, r'river camp', 6)
            a.send("cot listen 0")
        G.step(4, "receive: an ATAK unit and an ATAK marker; not sent back", s4)

        def s5():
            a.close()
            time.sleep(3)
            na = G.Sim("cot_ann", 5691, fresh=False)
            sims["ann"] = na
            na.ident()
            na.ask("cot", r"\[COT\] feed off, every 5 s, to " + re.escape(bip) + r":6969", 30)
        G.step(5, "a restart keeps the settings", s5)
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
