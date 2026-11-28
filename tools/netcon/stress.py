#!/usr/bin/env python3
"""Overnight stress test of the remote console (runs on the relay machine, next to relay.py).

  python3 stress.py --hours 5.5 --units TDP-0E34 TDP-1448 [--switch TDP-0E34]

Every minute: is each unit connected, does a command come back, internal RAM (from the
"[MAIN] beat free_int=" lines), the unit's connection counter (a drop = the unit restarted).
On top of that, on a schedule:
  every 10 min  bulk: "fieldlog dump 48" (~48 KB in one go: throughput, ring overrun)
  every 15 min  screenshot: the mirror's full frame (@HELLO + 320 rows)
  every 20 min  flood: 60 commands sent back to back, then one more must still answer
  every 30 min  relay restart (systemctl): seconds until every unit is back
  every 45 min  path switch on --switch units: home relay set to a dead address, so the unit
                must come back over the internet relay (TLS); then the home address restored
Log: ~/rscon/stress/stress_<start>.log; summary at the end (and in <log>.summary).
"""
import argparse
import os
import re
import socket
import subprocess
import time

CTL = os.path.expanduser("~/rscon/ctl.sock")
HOME_RELAY = "192.168.0.197 4404"
DEAD_RELAY = "192.168.0.250 4404"


def ctl(line, timeout=60):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    out = b""
    try:
        s.connect(CTL)
        s.sendall((line + "\n").encode())
        while True:
            d = s.recv(65536)
            if not d:
                break
            out += d
    except (socket.timeout, OSError) as e:
        out += ("\nCTLERR %s" % e).encode()
    finally:
        s.close()
    return out.decode("utf-8", "replace")


class Test:
    def __init__(self, a):
        self.a = a
        os.makedirs(os.path.expanduser("~/rscon/stress"), exist_ok=True)
        self.path = os.path.expanduser("~/rscon/stress/stress_%s.log" % time.strftime("%Y%m%d_%H%M"))
        self.f = open(self.path, "a")
        self.stats = {u: dict(checks=0, ok=0, fail=0, missing=0, restarts=0, conns=None, minram=10**9,
                              bulk=[], shots=[], floods=[], switch=[], lost=0, paths=set()) for u in a.units}
        self.relay_back = []

    def log(self, msg):
        line = "%s %s" % (time.strftime("%m-%d %H:%M:%S"), msg)
        print(line, flush=True)
        self.f.write(line + "\n")
        self.f.flush()

    def connected(self):
        out = ctl("list", 10)
        return {l.split()[0]: l for l in out.splitlines() if l.strip() and not l.startswith("(")}

    def netcon(self, u):
        out = ctl("run %s 4 netcon" % u, 20)
        m = re.search(r"\[NETCON\] on, ([^,]+), .*?(\d+) connections", out)
        return (m.group(1), int(m.group(2))) if m else (None, None)

    def check(self):
        con = self.connected()
        for u in self.a.units:
            st = self.stats[u]
            st["checks"] += 1
            if u not in con:
                st["missing"] += 1
                self.log("CHECK %s not connected" % u)
                continue
            status, conns = self.netcon(u)
            if status is None:
                st["fail"] += 1
                self.log("CHECK %s no answer to netcon" % u)
                continue
            st["ok"] += 1
            st["paths"].add(status)
            if st["conns"] is not None and conns < st["conns"]:
                st["restarts"] += 1
                self.log("CHECK %s RESTARTED (connection count %d -> %d)" % (u, st["conns"], conns))
            st["conns"] = conns
            rec = ctl("recent %s 400" % u, 10)
            rams = [int(x) for x in re.findall(r"free_int=(\d+)", rec)]
            lost = re.findall(r"\[NETCON\] (\d+) bytes lost", rec)
            if rams:
                st["minram"] = min(st["minram"], min(rams))
            self.log("CHECK %s ok: %s, %d connections, free_int %s%s" % (
                u, status, conns, rams[-1] if rams else "?", ", ring overrun seen" if lost else ""))

    def bulk(self, u):
        t0 = time.time()
        out = ctl("run %s 20 fieldlog dump 48" % u, 40)
        n = len(out.encode())
        m = re.search(r"\[FIELD\] (\d+) bytes, the last (\d+)", out)
        lost = sum(int(x) for x in re.findall(r"(\d+) bytes lost", out))
        self.stats[u]["lost"] += lost
        want = int(m.group(2)) if m else 0
        self.stats[u]["bulk"].append((want, n, lost))
        self.log("BULK %s asked %d bytes, received %d in 20 s%s" % (u, want, n, ", LOST %d" % lost if lost else ""))

    def shot(self, u):
        out = ctl("run %s 8 @M on" % u, 30)
        ctl("send %s @M off" % u, 10)
        hello = "@HELLO" in out
        rows = sum(int(l.split()[2]) for l in out.splitlines() if l.startswith("@FB ") and len(l.split()) > 2)
        self.stats[u]["shots"].append(rows if hello else -1)
        self.log("SHOT %s %s, %d rows" % (u, "frame" if hello else "NO FRAME", rows))

    def flood(self, u):
        for i in range(60):
            ctl("send %s id" % u, 10)
        out = ctl("run %s 5 netcon" % u, 20)
        ok = "[NETCON] on" in out
        self.stats[u]["floods"].append(ok)
        self.log("FLOOD %s 60 commands sent, still answering: %s" % (u, ok))

    def relay_restart(self):
        subprocess.run(["sudo", "systemctl", "restart", "rscon-relay"])
        t0 = time.time()
        time.sleep(2)
        while time.time() - t0 < 180:
            if all(u in self.connected() for u in self.a.units):
                break
            time.sleep(2)
        dt = time.time() - t0
        back = all(u in self.connected() for u in self.a.units)
        self.relay_back.append(dt if back else None)
        self.log("RELAY restarted: %s after %.0f s" % ("all back" if back else "NOT all back", dt))

    def switch(self, u):
        ctl("send %s netcon host %s" % (u, DEAD_RELAY), 10)
        t0 = time.time()
        time.sleep(3)
        via = None
        while time.time() - t0 < 180:
            c = self.connected()
            if u in c and "127.0.0.1" in c[u]:
                via = "internet"
                break
            time.sleep(2)
        dt = time.time() - t0
        status, _ = self.netcon(u) if via else (None, None)
        self.log("SWITCH %s to internet relay: %s after %.0f s (%s)" % (u, via or "NOT CONNECTED", dt, status))
        if via:
            self.shot(u)
        ctl("send %s netcon host %s" % (u, HOME_RELAY), 10)
        t1 = time.time()
        home = False
        while time.time() - t1 < 120:
            c = self.connected()
            if u in c and "127.0.0.1" not in c[u]:
                home = True
                break
            time.sleep(2)
        self.stats[u]["switch"].append((via, round(dt), home, round(time.time() - t1)))
        self.log("SWITCH %s back home: %s after %.0f s" % (u, home, time.time() - t1))
        if not home and u in self.connected():                # still reachable over the internet: retry
            ctl("send %s netcon host %s" % (u, HOME_RELAY), 10)

    def summary(self):
        lines = ["SUMMARY %s .. %s" % (time.strftime("%m-%d %H:%M", time.localtime(self.start)), time.strftime("%m-%d %H:%M"))]
        for u, st in self.stats.items():
            lines.append("%s: checks %d, answered %d, no answer %d, not connected %d, restarts %d, min free_int %s, paths %s"
                         % (u, st["checks"], st["ok"], st["fail"], st["missing"], st["restarts"],
                            st["minram"] if st["minram"] < 10**9 else "?", sorted(st["paths"])))
            lines.append("   bulk (asked, received, lost): %s" % st["bulk"])
            lines.append("   screenshots (rows, -1 = no frame): %s" % st["shots"])
            lines.append("   floods still answering: %s" % st["floods"])
            if st["switch"]:
                lines.append("   path switches (via, s, back home, s): %s" % st["switch"])
        lines.append("relay restarts, seconds until all back (None = not all): %s" % self.relay_back)
        txt = "\n".join(lines)
        self.log(txt)
        open(self.path + ".summary", "w").write(txt + "\n")

    def run(self):
        self.start = time.time()
        end = self.start + self.a.hours * 3600
        last = dict(bulk=0, shot=0, flood=0, relay=time.time(), switch=time.time() - 30 * 60)
        self.log("START units %s, switch %s, %.1f h" % (self.a.units, self.a.switch, self.a.hours))
        while time.time() < end:
            t = time.time()
            self.check()
            if t - last["bulk"] > 600:
                last["bulk"] = t
                for u in self.a.units: self.bulk(u)
            if t - last["shot"] > 900:
                last["shot"] = t
                for u in self.a.units: self.shot(u)
            if t - last["flood"] > 1200:
                last["flood"] = t
                for u in self.a.units: self.flood(u)
            if t - last["relay"] > 1800:
                last["relay"] = t
                self.relay_restart()
            if t - last["switch"] > 2700:
                last["switch"] = t
                for u in self.a.switch: self.switch(u)
            if len(self.relay_back) % 2 == 0 and int((t - self.start) / 3600) != int((time.time() - self.start) / 3600):
                self.summary()
            time.sleep(max(5, 60 - (time.time() - t)))
        self.summary()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=5.5)
    ap.add_argument("--units", nargs="+", default=["TDP-0E34", "TDP-1448"])
    ap.add_argument("--switch", nargs="*", default=["TDP-0E34"])
    Test(ap.parse_args()).run()
