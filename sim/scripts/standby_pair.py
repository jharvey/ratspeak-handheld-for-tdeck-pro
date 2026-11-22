#!/usr/bin/env python3
"""Two-simulator test: a message arriving in standby is received and does not end standby.

    python sim\\scripts\\standby_pair.py        (from the repo root; needs sim\\build, or
                                                 set RS_SIM_EXE=<path to ratspeak-sim.exe>)

Starts A and B headless with --serial-tcp. B goes into standby ("standby now"), A sends it a
message. On B: "[MSG] in from <A>" appears, "standby" still prints state=standby, a console
key ("k T") does not end it either, and "standby wake" does. Exit 0 = PASS.
"""
import os, re, shutil, socket, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("RS_SIM_EXE") or os.path.join(ROOT, "build", "ratspeak-sim.exe")
DATA = os.environ.get("RS_SIM_DATA") or os.path.join(tempfile.gettempdir(), "rs_standby_pair")


class Sim:
    def __init__(self, name, port):
        data = os.path.join(DATA, name)
        if os.path.isdir(data):
            shutil.rmtree(data)
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen",
                   QT_QPA_FONTDIR=os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts"))
        self.p = subprocess.Popen([EXE, "--name", name, "--data", data, "--serial-tcp", str(port)], env=env,
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
    a, b = Sim("sbypa", 5595), Sim("sbypb", 5596)
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
        time.sleep(6)
        b.send("a")
        time.sleep(4)
        a.send("a")
        time.sleep(15)
        print("both announced")
        b.send("standby now")
        b.expect(r"\[STANDBY\] enter \(console\)", 10)
        b.expect(r"\[POWER\] \S+ \(standby\): .*lora on, gps Off, wifi off", 10)
        a.send("msg " + bh + " hello standby")
        b.expect(r"\[MSG\] in from " + ah + r': "hello standby"', 180)
        print("B received the message in standby")
        time.sleep(3)
        b.send("standby")
        b.expect(r"\[STANDBY\] option=off .*state=standby", 10)
        b.send("k T")                               # a console key is not use
        time.sleep(3)
        b.send("standby")
        b.expect(r"state=standby", 10)
        b.send("standby wake")
        b.expect(r"\[STANDBY\] leave \(console\)", 10)
        b.send("standby")
        b.expect(r"state=normal", 10)
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
