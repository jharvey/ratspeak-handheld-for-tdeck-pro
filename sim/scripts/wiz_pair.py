#!/usr/bin/env python3
"""Setup wizard contact import between two simulators (fresh A in its wizard, B with 3 contacts).

    python sim\scripts\wiz_pair.py            (from the repo root; needs sim\build)

A listens (wizard step 9, "wiz listen"); B sends every contact as a card ("shareall <A>", the
Settings S.6 "Share all contacts" action): A saves all 3 at once. Then B exports
ratdeckpro/contacts.txt, the file is copied to A's card, and A imports 3 from it.
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
    a, b = Sim("wizpa", 5595), Sim("wizpb", 5596)
    try:
        time.sleep(15)                              # let both open their TCP serial port
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
        print("A", ah, "B", bh)
        # A is fresh: its wizard is open. B skips it and gets 3 contacts.
        a.send("wiz")
        a.expect(r"\[WIZ\] open=1", 10)
        b.send("wiz skip")
        for hx, nm in (("0000000000000000000000000000a11c", "Alice"), ("0000000000000000000000000000b0bb", "Bob"),
                       ("0000000000000000000000000000ca01", "Carol")):
            b.send("contact %s %s" % (hx, nm))
            time.sleep(0.5)
        a.send("wiz listen")
        a.expect(r"\[WIZ\] listen on", 10)
        time.sleep(6)
        b.send("a")
        time.sleep(4)
        a.send("a")
        time.sleep(15)
        b.send("shareall " + ah)
        m = b.expect(r"\[CARD\] shareall (\d+) of (\d+)", 30)
        print("B shareall", m.group(1), "of", m.group(2))
        if m.group(1) != "3" or m.group(2) != "3":
            raise RuntimeError("B should send 3 cards")
        for _ in range(3):
            a.expect(r"\[WIZ\] contact saved", 180)
        print("A saved 3 cards over the radio")
        # SD: B exports contacts.txt; the file goes onto A's card; A imports it
        b.send("contacts export")
        b.expect(r"\[CONTACTS\] exported 3", 10)
        src = os.path.join(ROOT, "build", "simdata", "wizpb", "sd", "ratdeckpro", "contacts.txt")
        dst = os.path.join(ROOT, "build", "simdata", "wizpa", "sd", "ratdeckpro", "contacts.txt")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        a.send("contacts import")
        a.expect(r"\[CONTACTS\] imported 3", 10)
        print("A imported 3 from contacts.txt")
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
