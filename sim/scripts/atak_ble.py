#!/usr/bin/env python3
"""ATAK feed over the Bluetooth link, in the simulator (its UDP stand-in for BLE, CotBle.cpp).

    python sim\\scripts\\atak_ble.py         (from the repo root; needs sim\\build, or set
                                             RS_SIM_EXE=<path to ratspeak-sim.exe>)

This script plays the phone's bridge app. Steps:
 1 "cot link ble" + "cot on": the radio waits for a phone ("waiting for phone")
 2 the phone connects: "[BLE] phone connected"; a marker reaches the phone as one whole CoT
   event (cut into 244-byte pieces, ends with "</event>" and a newline), valid XML
 3 ATAK on the phone sends a unit and a marker: the radio shows the unit and adds the marker
 4 broken / cut events from the phone are dropped, the radio keeps working
 5 the phone goes away ("BYE"): "[BLE] phone disconnected"; "cot link wifi" turns Bluetooth off
Exit 0 = all passed. Log: %TEMP%\\rs_atak_ble\\atakble.log
"""
import os, socket, sys, tempfile, time
import xml.etree.ElementTree as ET

os.environ.setdefault("RS_SIM_DATA", os.path.join(tempfile.gettempdir(), "rs_atak_ble"))
os.environ.setdefault("RS_SIM_AIR_PORT", "48461")
BLE = int(os.environ.setdefault("RS_SIM_BLE_PORT", "4391"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import groups_net as G

ATAK_UNIT = ('<?xml version="1.0" encoding="UTF-8"?><event version="2.0" uid="ANDROID-ble-test" '
             'type="a-f-G-U-C" how="m-g" time="%s" start="%s" stale="%s"><point lat="43.70000" '
             'lon="-79.40000" hae="0" ce="10" le="10"/><detail><contact callsign="PHONE ONE"/></detail></event>')
ATAK_MARK = ('<?xml version="1.0" encoding="UTF-8"?><event version="2.0" uid="ble-mark-1" '
             'type="b-m-p-s-m" how="h-g-i-g-o" time="%s" start="%s" stale="%s"><point lat="43.71000" '
             'lon="-79.41000" hae="0" ce="10" le="10"/><detail><contact callsign="water: creek"/></detail></event>')


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


def main():
    os.makedirs(G.DATA, exist_ok=True)
    s = G.Sim("atakble", 5781)
    ph = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ph.settimeout(0.5)
    to = ("127.0.0.1", BLE)
    got = bytearray()

    def drain(sec):
        end = time.time() + sec
        while time.time() < end:
            try:
                d, _ = ph.recvfrom(4096)
                got.extend(d)
            except socket.timeout:
                pass

    def send_chunks(data, size=180):
        for i in range(0, len(data), size):
            ph.sendto(data[i:i + size], to)
            time.sleep(0.02)

    try:
        s.ident()
        s.send("wiz skip")
        time.sleep(1)
        s.send("name ANN")

        def s1():
            s.mark()
            s.send("cot link ble")
            s.send("cot on")
            s.expect(r"\[BLE\] advertising as \"RatSpeak-[0-9a-f]{4}\", code \d{6}", 15)
            s.ask("cot", r"\[COT\] feed on, every 30 s, link Bluetooth \(waiting for phone", 10)
        G.step(1, "Bluetooth link on: waiting for a phone", s1)

        def s2():
            s.send("mark new camp 43.65800 -79.38300 tents & <fire>")
            time.sleep(1)
            s.mark()
            ph.sendto(b"HELLO", to)
            s.expect(r"\[BLE\] phone connected", 10)
            got.clear()
            drain(8)
            text = got.decode("utf-8", "replace")
            ev = [e for e in text.split("\n") if e.strip()]
            if not ev:
                raise RuntimeError("no event reached the phone")
            for e in ev:
                if not e.startswith("<?xml") or not e.endswith("</event>"):
                    raise RuntimeError("not one whole event: %r" % e[:80])
                ET.fromstring(e.split("?>", 1)[1])
            if not any('callsign="camp: tents &amp; &lt;fire&gt;"' in e for e in ev):
                raise RuntimeError("the marker is not among %d events" % len(ev))
            print("   %d events, %d bytes" % (len(ev), len(text)), flush=True)
        G.step(2, "the phone connects and gets whole, valid CoT events (the marker)", s2)

        def s3():
            s.mark()
            t = time.time()
            send_chunks((ATAK_UNIT % (iso(t), iso(t), iso(t + 300))).encode())
            send_chunks((ATAK_MARK % (iso(t), iso(t), iso(t + 300))).encode())
            s.expect(r"\[COT\] ATAK unit \"PHONE ONE\" 43\.70000,-79\.40000", 10)
            s.expect(r"\[MARK\] #\d+ water at 43\.71000,-79\.41000 \"creek\" from ATAK", 10)
        G.step(3, "ATAK's unit and marker from the phone reach the radio", s3)

        def s4():
            s.mark()
            ph.sendto(b"<?xml junk <event uid=\"x\" type=", to)       # cut, no end
            ph.sendto(b"\x00\xff garbage </event>", to)               # an end with no event
            ph.sendto(b"X" * 3500, to)                                  # far too long
            t = time.time()
            send_chunks((ATAK_UNIT.replace("PHONE ONE", "PHONE TWO").replace("ble-test", "ble-two")
                         % (iso(t), iso(t), iso(t + 300))).encode())
            s.expect(r"\[COT\] ATAK unit \"PHONE TWO\"", 10)
            s.ask("cot", r"link Bluetooth \(connected", 10)
        G.step(4, "broken events are dropped, the next good one still arrives", s4)

        def s5():
            s.mark()
            ph.sendto(b"BYE", to)
            s.expect(r"\[BLE\] phone disconnected", 10)
            s.send("cot link wifi")
            s.expect(r"\[BLE\] off", 10)
            s.ask("cot", r"\[COT\] feed on, every 30 s, to 239\.2\.3\.1:6969", 10)
        G.step(5, "the phone leaves; back to WiFi turns Bluetooth off", s5)
    finally:
        s.close()
    print("%d of %d steps passed" % (sum(r[2] for r in G.results), len(G.results)))
    sys.exit(0 if all(r[2] for r in G.results) else 1)


if __name__ == "__main__":
    main()
