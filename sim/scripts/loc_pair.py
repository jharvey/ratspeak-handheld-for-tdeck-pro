#!/usr/bin/env python3
"""Two-simulator test of "share my location" -> contact position on the map, robust to other
simulators on the same virtual radio (B is addressed by its hash, not "Peers row 1").

    python sim\\scripts\\loc_pair.py            (from the repo root; needs sim\\build)

Starts A and B headless with --serial-tcp, waits until A has heard B's announce, sends
"locshare <B>" on A, and checks on B: the message arrives, the position is stored as A's
last position ("[LOC] <A> at 43.65320,-79.38320" with the sim GPS), and "cpos" lists it.
Exit 0 = PASS.
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
    a, b = Sim("locpa", 5591), Sim("locpb", 5592)
    try:
        time.sleep(15)                              # let both open their TCP serial port
        for s in (a, b):
            s.connect()
        def ident(s):                               # "[BOOT] ready" may be before we connected
            for _ in range(30):
                s.send("id")
                try:
                    return s.expect(r"\[ID\] dest=([0-9a-f]{32})", 3).group(1)
                except RuntimeError:
                    pass
            raise RuntimeError(s.name + ": no id")
        bh, ah = ident(b), ident(a)
        for s in (a, b):                            # fresh devices open the setup wizard
            s.send("wiz skip")
        print("A", ah, "B", bh)
        # The boot announces may have passed before we connected: announce again, give the
        # (shared) virtual radio time, then share.
        time.sleep(6)
        b.send("a")
        time.sleep(4)
        a.send("a")
        time.sleep(15)
        print("both announced")
        a.send("locshare " + bh)
        a.expect(r"\[LOC\] share (sent|queued)", 60)
        b.expect(r"\[MSG\] inbound message", 120)
        m = b.expect(r"\[LOC\] (\S+) at (-?\d+\.\d+),(-?\d+\.\d+)", 30)
        print("B stored", m.group(1), m.group(2), m.group(3))
        b.send("cpos")
        b.expect(r"\[LOC\] cpos #\d+ " + ah, 10)
        b.expect(r"\[LOC\] cpos 1 contacts", 10)
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
