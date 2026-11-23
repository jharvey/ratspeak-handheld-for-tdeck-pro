#!/usr/bin/env python3
"""Timed random-task test: does the radio keep up with a user?

Instead of random keys (monkey.py), it runs random TASKS, each a short realistic sequence
of key presses with a time budget on every step:

  atlas     open the Atlas, pan / zoom a random number of times in random directions, close
  settings  open Settings, a random section, move down a random number of rows, back out
  contact   open Contacts, a random contact's page, back
  chat      open the Inbox, a chat, back
  markers   Atlas, the marker list, move, back
  layers    Atlas, the layers page, switch a random layer twice (back as it was), back
  library   open the Library, a file, a few pages, back
  peers     open Peers, move, back

Each key is sent with the console's "k" command (the same path as the keyboard) and the step
waits for the firmware's own timing line "[UI] frame ... N ms after the last key": N is what
a user waits from the key to the finished screen. A step fails when its sign (screen change,
page, log line) does not come within its budget. The run cycles through energy modes
(console pm<n>) and reports, per task and per mode, runs, failures and the step times
(median, 95th percentile, worst), plus the heap at the start and end (heartbeat).

  python tools/stress/tasks.py --minutes 10                  (simulator, realistic e-ink timing)
  python tools/stress/tasks.py --port COM3 --minutes 10      (a real unit over USB)
  options: --modes 0,1,2  --seed N  --out <folder>  --budget-scale 1.5 (looser budgets)

Simulator: RS_SIM_EXE (default sim/build/ratspeak-sim.exe); the device gets contacts, a few
chats, markers and the Library files; with --card <folder holding ratdeckpro/> that card too.
Exit 0 = no step over budget and no failure.
"""
import argparse, json, os, random, re, shutil, socket, statistics, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Step budgets in ms (key to finished screen on the unit). --budget-scale multiplies them.
B_KEY = 2000        # a key that redraws (move, pan, zoom, back)
B_OPEN = 4000       # a key that opens a screen that reads storage (map, chat, library file)


class Link:
    """Console line I/O with a reader thread; serial (real unit) or TCP (simulator)."""

    def __init__(self, port=None, tcp=None):
        self.lines, self.lock, self.alive = [], threading.Lock(), True
        if port:
            import serial
            self.ser = serial.Serial(port, 115200, timeout=0.2)
            self.read = lambda: self.ser.read(4096)
            self.write = lambda b: self.ser.write(b)
        else:
            end = time.time() + 90
            self.sock = None
            while time.time() < end and self.sock is None:
                try:
                    self.sock = socket.create_connection(("127.0.0.1", tcp), timeout=1)
                except OSError:
                    time.sleep(1)
            if not self.sock:
                raise RuntimeError("no simulator serial port")
            self.sock.settimeout(0.2)

            def rd():
                try:
                    return self.sock.recv(65536)
                except socket.timeout:
                    return b""
            self.read = rd
            self.write = lambda b: self.sock.sendall(b)
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        pend = b""
        while self.alive:
            try:
                d = self.read()
            except Exception:
                time.sleep(0.2)
                continue
            if not d:
                continue
            pend += d
            *ls, pend = pend.split(b"\n")
            with self.lock:
                for l in ls:
                    self.lines.append((time.time(), l.decode(errors="replace").rstrip("\r")))

    def mark(self):
        with self.lock:
            return len(self.lines)

    def send(self, s):
        self.write((s + "\n").encode())

    def wait(self, pat, timeout, start):
        rx = re.compile(pat)
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                for i in range(start, len(self.lines)):
                    m = rx.search(self.lines[i][1])
                    if m:
                        return m, i + 1
            time.sleep(0.02)
        return None, start


FRAME = r"\[UI\] frame (\d+)ms .*?, (\d+) ms after the last key"


class Runner:
    def __init__(self, link, scale, log):
        self.l, self.scale, self.log = link, scale, log
        self.steps = []          # (task, mode, step, ms or None, ok)

    def settle(self, quiet=0.8, limit=5.0):
        """Wait until the screen is still (no frame for `quiet` s), so a late redraw of the
        previous step is not counted against this one."""
        end = time.time() + limit
        last = self.l.mark()
        still = time.time()
        while time.time() < end:
            time.sleep(0.1)
            now = self.l.mark()
            with self.l.lock:
                new_frames = any("[UI] frame" in self.l.lines[i][1] for i in range(last, now))
            last = now
            if new_frames:
                still = time.time()
            elif time.time() - still >= quiet:
                return

    def key(self, task, mode, k, sign=None, budget=B_KEY, label=None):
        """Press one key; wait for its sign (default: a frame) within the budget."""
        budget = int(budget * self.scale)
        self.settle()
        st = self.l.mark()
        t0 = time.time()
        self.l.send("k " + k)
        ms, ok = None, True
        if sign:
            m, _ = self.l.wait(sign, budget / 1000.0 + 1.0, st)
            ok = m is not None
            ms = int((time.time() - t0) * 1000)
            fm, _ = self.l.wait(FRAME, 1.5, st)          # the frame for that screen, if any
            if fm:
                ms = max(int(fm.group(2)), 0) if int(fm.group(2)) < 60000 else ms
        else:
            fm, _ = self.l.wait(FRAME, budget / 1000.0 + 1.0, st)
            ok = fm is not None
            ms = int(fm.group(2)) if fm else None
        over = ms is not None and ms > budget
        self.steps.append((task, mode, label or k, ms, ok and not over))
        if not ok or over:
            self.log("   %s %s step %s: %s (budget %d ms)" % (task, mode, label or k,
                     "no sign" if not ok else "%d ms OVER" % ms, budget))
        time.sleep(0.15)
        return ok

    def home(self):
        for _ in range(3):
            self.l.send("k ^")
            time.sleep(0.4)
        self.l.send("k I")
        time.sleep(0.8)


def tasks(r, rnd):
    def atlas(m):
        r.key("atlas", m, "A", r"\[MAP\] open", B_OPEN, "open")
        for _ in range(rnd.randint(1, 8)):
            r.key("atlas", m, rnd.choice("TFVHZW"), None, B_KEY, "pan/zoom")
        r.key("atlas", m, "^", r"\[MAP\] closed", B_KEY, "close")

    def settings(m):
        r.key("settings", m, "M", None, B_KEY, "menu")
        r.key("settings", m, "S", r"\[UI\] -> Settings", B_KEY, "open")
        r.key("settings", m, str(rnd.randint(1, 9)), r"\[UI\] page ", B_KEY, "section")
        for _ in range(rnd.randint(0, 5)):
            r.key("settings", m, "V", None, B_KEY, "row down")
        r.key("settings", m, "^", r"\[UI\] page ", B_KEY, "back")

    def contact(m):
        r.key("contact", m, "C", r"\[UI\] -> Contacts", B_KEY, "open")
        for _ in range(rnd.randint(0, 3)):
            r.key("contact", m, "V", None, B_KEY, "row down")
        r.key("contact", m, "~", None, B_OPEN, "contact page")
        r.key("contact", m, "^", None, B_KEY, "back")

    def chat(m):
        r.key("chat", m, "I", r"\[UI\] -> Inbox", B_KEY, "inbox")
        r.key("chat", m, "~", r"\[HIST\]", B_OPEN, "open chat")
        r.key("chat", m, "^", None, B_KEY, "back")

    def markers(m):
        r.key("markers", m, "A", r"\[MAP\] open", B_OPEN, "atlas")
        r.key("markers", m, "J", r"\[MARK\] list", B_KEY, "list")
        for _ in range(rnd.randint(0, 3)):
            r.key("markers", m, "V", None, B_KEY, "row down")
        r.key("markers", m, "^", None, B_KEY, "back")
        r.key("markers", m, "^", r"\[MAP\] closed", B_KEY, "close")

    def layers(m):
        r.key("layers", m, "A", r"\[MAP\] open", B_OPEN, "atlas")
        r.key("layers", m, "O", r"\[MAP\] layers page", B_KEY, "page")
        d = str(rnd.randint(1, 6))
        r.key("layers", m, d, r"\[MAP\] layer ", B_KEY, "switch")
        r.key("layers", m, d, r"\[MAP\] layer ", B_KEY, "switch back")
        r.key("layers", m, "^", None, B_OPEN, "map again")
        r.key("layers", m, "^", r"\[MAP\] closed", B_KEY, "close")

    def library(m):
        r.key("library", m, "M", None, B_KEY, "menu")
        r.key("library", m, "R", r"\[LIB\] \d+ files", B_OPEN, "open")
        for _ in range(rnd.randint(0, 4)):
            r.key("library", m, "V", None, B_KEY, "row down")
        r.key("library", m, "~", r"\[LIB\] open ", B_OPEN, "open file")
        for _ in range(rnd.randint(1, 4)):
            r.key("library", m, "N", None, B_OPEN, "next page")
        r.key("library", m, "^", None, B_KEY, "list")
        r.key("library", m, "^", None, B_KEY, "close")

    def peers(m):
        r.key("peers", m, "P", r"\[UI\] -> Peers", B_KEY, "open")
        for _ in range(rnd.randint(0, 3)):
            r.key("peers", m, "V", None, B_KEY, "row down")
        r.key("peers", m, "I", r"\[UI\] -> Inbox", B_KEY, "leave")

    return {"atlas": atlas, "settings": settings, "contact": contact, "chat": chat,
            "markers": markers, "layers": layers, "library": library, "peers": peers}


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))] if v else None


def main():
    ap = argparse.ArgumentParser(description="Timed random-task test (see the top of the file).")
    ap.add_argument("--port", help="a real unit's serial port (else a simulator)")
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--modes", default="0,1,2", help="energy modes to cycle (console pm<n>)")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--budget-scale", type=float, default=1.0)
    ap.add_argument("--card", help="simulator: folder holding ratdeckpro/ for the SD card")
    ap.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "rs_tasks"))
    a = ap.parse_args()
    seed = a.seed if a.seed is not None else int(time.time())
    rnd = random.Random(seed)
    os.makedirs(a.out, exist_ok=True)
    report = open(os.path.join(a.out, "report.txt"), "w", encoding="utf-8")

    def log(*x):
        s = " ".join(str(v) for v in x)
        print(s, flush=True)
        report.write(s + "\n")
        report.flush()

    proc = None
    if a.port:
        link = Link(port=a.port)
        log("unit on", a.port, "(opening the port may restart it: waiting for it)")
        link.wait(r"\[BOOT\] ready", 240, 0)
        time.sleep(8)
    else:
        exe = os.environ.get("RS_SIM_EXE") or os.path.join(ROOT, "sim", "build", "ratspeak-sim.exe")
        data = os.path.join(a.out, "simdata")
        shutil.rmtree(data, ignore_errors=True)
        os.makedirs(os.path.join(data, "sd", "ratdeckpro", "library"))
        lib = os.path.join(ROOT, "tools", "sdcard", "library")
        for f in os.listdir(lib):
            shutil.copy(os.path.join(lib, f), os.path.join(data, "sd", "ratdeckpro", "library"))
        if a.card:
            shutil.copytree(os.path.join(a.card, "ratdeckpro"), os.path.join(data, "sd", "ratdeckpro"), dirs_exist_ok=True)
        open(os.path.join(data, "sim.ini"), "w").write("gps = 35.28330,-86.37000\n")
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", RS_SIM_AIR_PORT="48447",
                   QT_QPA_FONTDIR=os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts"))
        proc = subprocess.Popen([exe, "--name", "tasks", "--data", data, "--serial-tcp", "5741"],   # no --fast: real e-ink timing
                                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        link = Link(tcp=5741)
        link.wait(r"\[BOOT\] ready", 120, 0)
        time.sleep(3)
        for c in ["wiz skip", "name TASKS"] + ["contact a%d0000000000000000000000000000a%d Friend %d" % (i, i, i) for i in range(1, 7)] + \
                 ["msgfill 40 4 120", "mark new hazard 35.29000 -86.37000 test", "mark new camp 35.27500 -86.36000 tents"]:
            link.send(c)
            time.sleep(0.6)
        time.sleep(5)
    link.send("hb 30")
    r = Runner(link, a.budget_scale, log)
    T = tasks(r, rnd)
    modes = [int(m) for m in a.modes.split(",") if m.strip()]
    heap = {}
    log("seed", seed, "minutes", a.minutes, "modes", modes, "budgets: key %d ms, open %d ms (x%.1f)" % (B_KEY, B_OPEN, a.budget_scale))
    end = time.time() + a.minutes * 60
    per_mode = (a.minutes * 60) / max(1, len(modes))
    runs = []
    for m in modes:
        link.send("pm%d" % m)
        time.sleep(2)
        st = link.mark()
        r.home()
        mend = min(end, time.time() + per_mode)
        while time.time() < mend:
            name = rnd.choice(list(T))
            t0 = time.time()
            n0 = len(r.steps)
            try:
                T[name](m)
            except Exception as e:
                log("   task", name, "error", e)
            runs.append((name, m, time.time() - t0, all(s[4] for s in r.steps[n0:])))
            r.home()
        with link.lock:
            hb = [int(x.group(1)) for t, l in link.lines[st:] for x in [re.search(r"\[HB\] .*?heap=(\d+)", l)] if x]
        heap[m] = (hb[0], min(hb), hb[-1]) if hb else None
    log("\n== results (step times in ms: key to finished screen) ==")
    bad = 0
    for name in T:
        for m in modes:
            st = [s for s in r.steps if s[0] == name and s[1] == m]
            if not st:
                continue
            v = [s[3] for s in st if s[3] is not None]
            fails = sum(1 for s in st if not s[4])
            bad += fails
            nr = sum(1 for x in runs if x[0] == name and x[1] == m)
            log("%-9s mode %d: %3d runs, %4d steps, %3d over budget / failed; median %s, p95 %s, worst %s" % (
                name, m, nr, len(st), fails, pct(v, 50), pct(v, 95), max(v) if v else None))
    for m in modes:
        log("mode %d heap (start, lowest, end): %s" % (m, heap.get(m)))
    slow = sorted((s for s in r.steps if s[3] is not None), key=lambda s: -s[3])[:10]
    log("slowest steps:", [(s[0], s[1], s[2], s[3]) for s in slow])
    json.dump({"seed": seed, "steps": r.steps, "runs": runs, "heap": heap}, open(os.path.join(a.out, "steps.json"), "w"))
    log("seed %d: %s" % (seed, "PASS" if bad == 0 else "FAIL: %d steps over budget or failed" % bad))
    link.alive = False
    if proc:
        proc.terminate()
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
