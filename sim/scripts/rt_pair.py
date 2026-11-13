#!/usr/bin/env python3
"""Range test between two simulators over the shared virtual radio.

    python sim\scripts\rt_pair.py            (from the repo root; needs sim\build)

Both start range-test mode with the other (every 5 s), run ~45 s, then: each has sent
pings, got replies (pongs carry the RSSI / SNR the other heard), heard the other's pings,
and wrote a CSV under sd/ratdeckpro/rangetest/. Exit 0 = PASS.
"""
import os, re, shutil, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "ratspeak-sim.exe")


class Sim:
    def __init__(self, name, port):
        data = os.path.join(ROOT, "build", "simdata", name)
        if os.path.isdir(data):
            shutil.rmtree(data)
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QPA_FONTDIR=os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts"))
        self.p = subprocess.Popen([EXE, "--name", name, "--serial-tcp", str(port)], env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.port, self.buf, self.name = port, "", name
        self.s = None

    def connect(self, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            try:
                self.s = socket.create_connection(("127.0.0.1", self.port), timeout=1)
                self.s.settimeout(0.3)
                return
            except OSError:
                time.sleep(1)
        raise RuntimeError(self.name + ": no serial port")

    def send(self, line):
        self.s.sendall((line + "\n").encode())

    def expect(self, pattern, timeout):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            m = rx.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            try:
                d = self.s.recv(65536)
                if d:
                    self.buf += d.decode("utf-8", "replace")
            except socket.timeout:
                pass
        raise RuntimeError("%s: not seen within %ds: %s" % (self.name, timeout, pattern))

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass
        self.p.terminate()


def main():
    a, b = Sim("rtpa", 5597), Sim("rtpb", 5598)
    try:
        time.sleep(15)
        for s in (a, b):
            s.connect()
        def ident(s):
            for _ in range(30):
                s.send("id")
                try:
                    return s.expect(r"\[ID\] dest=([0-9a-f]{32})", 3).group(1)
                except RuntimeError:
                    pass
            raise RuntimeError(s.name + ": no id")
        bh, ah = ident(b), ident(a)
        for s in (a, b):
            s.send("wiz skip")
        print("A", ah, "B", bh)
        a.send("rt start %s 5" % bh)
        a.expect(r"\[RT\] start", 10)
        b.send("rt start %s 5" % ah)
        b.expect(r"\[RT\] start", 10)
        a.expect(r"\[RT\] pong \d+: here (-?\d+) dBm", 60)
        b.expect(r"\[RT\] pong \d+: here", 60)
        time.sleep(30)
        res = {}
        for s in (a, b):
            s.send("rt")
            m = s.expect(r"\[RT\] on=1 sent=(\d+) replies=(\d+) lost=(\d+) heard=(\d+) file=(\S+)", 10)
            res[s.name] = m.groups()
            print(s.name, "sent", m.group(1), "replies", m.group(2), "lost", m.group(3), "heard", m.group(4), m.group(5))
            if int(m.group(2)) < 3 or int(m.group(4)) < 3:
                raise RuntimeError(s.name + ": too few replies / pings heard")
        for s in (a, b):
            s.send("rt stop")
            s.expect(r"\[RT\] stop", 10)
        for name in ("rtpa", "rtpb"):
            f = os.path.join(ROOT, "build", "simdata", name, "sd", "ratdeckpro", "rangetest", res[name][4])
            rows = open(f).read().strip().splitlines()
            print(name, len(rows) - 1, "CSV rows;", rows[1] if len(rows) > 1 else "")
            if len(rows) < 8:
                raise RuntimeError(name + ": CSV too short")
        print("PASS")
        return 0
    except Exception as e:
        print("FAIL:", e)
        return 1
    finally:
        a.close()
        b.close()


if __name__ == "__main__":
    sys.exit(main())
