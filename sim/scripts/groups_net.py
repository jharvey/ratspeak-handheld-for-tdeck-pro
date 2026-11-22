#!/usr/bin/env python3
"""Group messages between four simulated radios (ANN, BOB, CAT, DAN) on one radio channel.

    python sim\\scripts\\groups_net.py      (from the repo root; needs sim\\build, or set
                                            RS_SIM_EXE=<path to ratspeak-sim.exe>)

Steps (each prints PASS or FAIL; exit 0 only when all pass):
  1  ANN makes the group TEAM with BOB and CAT (console) and sends to it: both get an ordinary
     direct message (they do not have the group yet); ANN's group chat shows "delivered 2/2".
  2  ANN shares the group with BOB (console) and with CAT (keys: Contacts, G, group, 4 Share).
     Each opens ANN's chat, Enter on the empty draft joins. BOB did not know CAT: CAT is saved
     as a contact by the join.
  3  ANN sends again: BOB and CAT see it in their group chat ("ANN: ...").
     CAT's chat with ANN shows "[TEAM] ..." and Enter opens the group chat.
  4  Reply to the group: BOB answers in the group chat; ANN and CAT see "BOB: ..." in theirs.
  5  Three members: ANN adds DAN and sends: "delivered 3/3"; DAN (no group) sees a direct
     message, shown as "[TEAM] ..." in ANN's chat.
  6  A changed group: ANN shares again with BOB; Enter updates BOB's copy to 3 members and
     BOB's next group message reaches DAN.
  7  A member that is not reachable: DAN is switched off, ANN sends: the others show
     delivered, DAN does not ("who got it").
  8  A restart keeps groups: BOB is stopped and started again; the group is still there.
Logs: <data>/<name>.log (data = RS_SIM_DATA or %TEMP%\\rs_groups_net).
"""
import os, re, shutil, socket, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("RS_SIM_EXE") or os.path.join(ROOT, "build", "ratspeak-sim.exe")
DATA = os.environ.get("RS_SIM_DATA") or os.path.join(tempfile.gettempdir(), "rs_groups_net")
AIR = os.environ.get("RS_SIM_AIR_PORT") or "48431"


class Sim:
    def __init__(self, name, port, fresh=True):
        self.name, self.port = name, port
        self.data = os.path.join(DATA, name)
        if fresh and os.path.isdir(self.data):
            shutil.rmtree(self.data)
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", RS_SIM_AIR_PORT=AIR,
                   QT_QPA_FONTDIR=os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts"))
        self.p = subprocess.Popen([EXE, "--name", name, "--data", self.data, "--fast", "--serial-tcp", str(port)],
                                  env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.buf, self.pos, self.lock, self.alive = "", 0, threading.Lock(), True
        self.log = open(os.path.join(DATA, name + ".log"), "a", encoding="utf-8", errors="replace")
        self.s = None
        end = time.time() + 90
        while time.time() < end and self.s is None:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=1)
            except OSError:
                time.sleep(1)
        if self.s is None:
            raise RuntimeError(name + ": no serial port")
        self.s.settimeout(0.5)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while self.alive:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not d:
                break
            t = d.decode("utf-8", "replace")
            with self.lock:
                self.buf += t
            self.log.write(t)
            self.log.flush()

    def send(self, line):
        self.s.sendall((line + "\n").encode())

    def mark(self):
        with self.lock:
            self.pos = len(self.buf)

    def expect(self, pattern, timeout):
        """First match after the last mark() / match; moves on past it."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while True:
            with self.lock:
                m = rx.search(self.buf, self.pos)
                if m:
                    self.pos = m.end()
                    return m
            if time.time() > end:
                raise RuntimeError("%s: not seen within %ds: %s" % (self.name, timeout, pattern))
            time.sleep(0.2)

    def ask(self, cmd, pattern, timeout, every=4):
        """Send cmd again and again until its output matches pattern."""
        end = time.time() + timeout
        while True:
            self.mark()
            self.send(cmd)
            try:
                return self.expect(pattern, every)
            except RuntimeError:
                if time.time() > end:
                    raise RuntimeError("%s: \"%s\" never showed %s" % (self.name, cmd, pattern))

    def keys(self, *steps):
        for k in steps:
            self.send("k " + k)
            time.sleep(1.5)

    def ident(self):
        return self.ask("id", r"\[ID\] dest=([0-9a-f]{32})", 90, 3).group(1)

    def close(self):
        self.alive = False
        try:
            self.s.close()
        except Exception:
            pass
        self.p.terminate()
        try:
            self.p.wait(10)
        except Exception:
            pass
        self.log.close()


results = []


def step(n, what, fn):
    t0 = time.time()
    try:
        fn()
        results.append((n, what, True, ""))
        print("PASS %d %s (%d s)" % (n, what, time.time() - t0), flush=True)
    except Exception as e:
        results.append((n, what, False, str(e)))
        print("FAIL %d %s: %s" % (n, what, e), flush=True)


def main():
    os.makedirs(DATA, exist_ok=True)
    for f in os.listdir(DATA):
        if f.endswith(".log"):
            os.remove(os.path.join(DATA, f))
    sims = {}
    try:
        for i, n in enumerate(("ann", "bob", "cat", "dan")):
            sims[n] = Sim("grpnet_" + n, 5611 + i)
        a, b, c, d = sims["ann"], sims["bob"], sims["cat"], sims["dan"]
        h = {n: s.ident() for n, s in sims.items()}
        for n, s in sims.items():
            s.send("wiz skip")
            time.sleep(1)
            s.send("name " + n.upper())
        print("radios:", h, flush=True)
        time.sleep(6)
        for s in sims.values():                      # everyone hears everyone
            s.send("a")
            time.sleep(5)
        time.sleep(15)
        a.send("contact %s BOB" % h["bob"])
        a.send("contact %s CAT" % h["cat"])
        for s in (b, c, d):
            s.send("contact %s ANN" % h["ann"])
        time.sleep(3)

        def hist(s, row, timeout=150):               # a row of the open chat
            return s.ask("grp hist", r"\[GRP\] row \d+ " + row, timeout)

        def s1():
            a.send("grp new TEAM")
            a.send("grp add 1 " + h["bob"])
            a.send("grp add 1 " + h["cat"])
            a.ask("grp", r'\[GRP\] 1: "TEAM" #[0-9a-f]{8} 2 members', 20)
            b.mark(); c.mark(); a.mark()
            a.send("grp send 1 hello team")
            a.expect(r'\[GRP\] send "TEAM" .*"hello team": \d now, \d queued, 0 failed', 20)
            b.expect(r"\[MSG\] in from " + h["ann"] + r': "hello team"', 150)
            c.expect(r"\[MSG\] in from " + h["ann"] + r': "hello team"', 150)
            a.send("grp open 1")
            hist(a, r'out "hello team" \[delivered 2/2\]')
        step(1, "send to a group of two; members without the group get a direct message", s1)

        def join(s, expect_line):
            s.send("chat " + h["ann"])
            time.sleep(3)
            s.ask("grp hist", r'\[GRP\] row \d+ in "\[group\] TEAM \d/\d ', 30)
            s.mark()
            s.send("k ~")                            # Enter on the empty draft
            s.expect(expect_line, 20)
            s.send("k ^")
            time.sleep(1)

        def s2():
            b.mark(); c.mark(); a.mark()
            a.send("grp share 1 " + h["bob"])
            a.expect(r'\[GRP\] share "TEAM" to BOB: 3 of 3 cards', 20)
            for _ in range(3):
                b.expect(r"\[MSG\] in from " + h["ann"] + r': "Group card: TEAM #', 200)
            join(b, r'\[GRP\] joined "TEAM" #[0-9a-f]{8}: 2 members, 1 new contacts')
            b.ask("grp", r"\[GRP\]    " + h["cat"] + " CAT", 20)
            # CAT gets it through the keys on ANN: wake, Contacts, G, Enter (group 1), 4 Share, V, Enter.
            a.keys("^", "^", "C", "G", "~", "4", "V", "~")
            a.expect(r'\[GRP\] share "TEAM" to CAT: 3 of 3 cards', 20)
            for _ in range(3):
                c.expect(r"\[MSG\] in from " + h["ann"] + r': "Group card: TEAM #', 200)
            join(c, r'\[GRP\] joined "TEAM" #[0-9a-f]{8}: 2 members, 1 new contacts')
        step(2, "share a group (console and keys) and join it; unknown members become contacts", s2)

        def s3():
            b.mark(); c.mark()
            a.send("grp send 1 second one")
            b.expect(r"\[MSG\] in from " + h["ann"] + r': "second one"', 150)
            c.expect(r"\[MSG\] in from " + h["ann"] + r': "second one"', 150)
            b.send("grp open 1")
            hist(b, r'in "ANN: second one"', 30)
            c.send("chat " + h["ann"])               # ANN's own chat: tagged, Enter opens the group
            hist(c, r'in "\[TEAM\] second one"', 30)
            c.mark()
            c.send("k ~")
            c.expect(r'\[HIST\] group "TEAM": [1-9]', 20)
            hist(c, r'in "ANN: second one"', 30)
        step(3, "a group message shows in the members' group chat", s3)

        def s4():
            a.mark(); c.mark()
            b.send("grp open 1")
            time.sleep(2)
            b.send("t:bob to all")
            a.expect(r"\[MSG\] in from " + h["bob"] + r': "bob to all"', 150)
            c.expect(r"\[MSG\] in from " + h["bob"] + r': "bob to all"', 150)
            a.send("grp open 1")
            hist(a, r'in "BOB: bob to all"', 30)
            c.send("grp open 1")
            hist(c, r'in "BOB: bob to all"', 30)
            hist(b, r'out "bob to all" \[delivered 2/2\]')
        step(4, "reply to the group", s4)

        def s5():
            a.send("contact %s DAN" % h["dan"])
            a.send("grp add 1 " + h["dan"])
            a.ask("grp", r'\[GRP\] 1: "TEAM" #[0-9a-f]{8} 3 members', 20)
            d.mark(); a.mark()
            a.send("grp send 1 now three")
            a.expect(r'"now three": \d now, \d queued, 0 failed', 20)
            d.expect(r"\[MSG\] in from " + h["ann"] + r': "now three"', 200)
            a.send("grp open 1")
            hist(a, r'out "now three" \[delivered 3/3\]', 200)
            d.send("chat " + h["ann"])
            hist(d, r'in "\[TEAM\] now three"', 30)
        step(5, "send to a group of three; a radio without the group shows [TEAM]", s5)

        def s6():
            b.mark(); a.mark(); d.mark()
            a.send("grp share 1 " + h["bob"])
            a.expect(r'\[GRP\] share "TEAM" to BOB: 4 of 4 cards', 20)
            for _ in range(4):
                b.expect(r"\[MSG\] in from " + h["ann"] + r': "Group card: TEAM #', 240)
            join(b, r'\[GRP\] updated "TEAM" #[0-9a-f]{8}: 3 members, 1 new contacts')
            b.send("grp send 1 bob to three")
            d.expect(r"\[MSG\] in from " + h["bob"] + r': "bob to three"', 240)
        step(6, "share again after adding a member: the other radio's copy is updated", s6)

        def s7():
            d.close()
            time.sleep(3)
            a.mark()
            a.send("grp send 1 dan is away")
            a.expect(r'"dan is away": \d now, \d queued, 0 failed', 20)
            a.send("grp open 1")
            m = a.ask("grp who", r'\[GRP\] who "dan is away": BOB=delivered CAT=delivered DAN=(\w+)', 240)
            print("   DAN shows \"%s\" once the others are delivered" % m.group(1), flush=True)
            if m.group(1) == "delivered":
                raise RuntimeError("DAN is off but shows delivered")
            try:
                a.ask("grp who", r"DAN=failed", 420, 10)
                m2 = a.ask("grp hist", r'out "dan is away" \[([^\]]*)\]', 20)
                print("   DAN shows \"failed\" later; the chat row reads \"%s\"" % m2.group(1), flush=True)
            except RuntimeError:
                m2 = a.ask("grp hist", r'out "dan is away" \[([^\]]*)\]', 20)
                print("   after 7 more minutes DAN is still not \"failed\"; the chat row reads \"%s\"" % m2.group(1), flush=True)
        step(7, "a member that is not reachable is not shown as delivered", s7)

        def s8():
            b.close()
            time.sleep(3)
            nb = Sim("grpnet_bob", 5621, fresh=False)
            sims["bob"] = nb
            nb.ident()
            nb.ask("grp", r'\[GRP\] 1: "TEAM" #[0-9a-f]{8} 3 members', 30)
            nb.ask("grp", r"\[GRP\]    " + h["dan"] + " DAN", 20)
        step(8, "a restart keeps groups", s8)
    except Exception as e:
        results.append((0, "setup", False, str(e)))
        print("FAIL setup:", e, flush=True)
    finally:
        for s in sims.values():
            try:
                s.close()
            except Exception:
                pass
    bad = [r for r in results if not r[2]]
    print("%d of %d steps passed" % (len(results) - len(bad), len(results)))
    return 1 if bad or not results else 0


if __name__ == "__main__":
    sys.exit(main())
