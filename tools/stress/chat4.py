#!/usr/bin/env python3
"""Group-chat soak on real units: N T-Decks on USB chat among themselves for hours.

Each unit starts a message to a random other unit every ~--rate seconds (exponential) and
answers most messages it receives after 5-40 s, like people chatting. About half the
messages are typed through the keyboard queue (console "chat <hex>" opens the thread, then
"k <text>~" types it and presses Enter) and must come out as a matching "[TX]" line, so a
dropped or wrong key shows up. The rest use the console "msg" command. Every few minutes each
unit also does a UI tour (Inbox, Contacts, Peers, Atlas with panning, back) and the screen
changes are checked.

Watched all the time: restarts (heartbeat uptime going down, boot lines, Guru / panic /
abort), hangs (no heartbeat and no answer to "hb"), loop steps over 200 ms ([SLOW]), key ->
panel times ("[UI] frame ... N ms after the last key"), internal heap and PSRAM (minimum and
largest block), LXMF queue and outbox. At the end: queues drain, "msgstat" per pair checks
that every numbered message arrived exactly once and how many ended delivered / failed.

  python tools/stress/chat4.py --ports COM3 COM5 COM6 COM9 --minutes 300
Logs: <out>/unit_<port>.log (every line, timestamped), <out>/summary.txt (rewritten every 5 min).
"""
import argparse
import os
import random
import re
import sys
import threading
import time

import serial

WORDS = ("ok on my way where are you at the camp see you soon got it bring water the trail is "
         "muddy heading north now battery fine copy that meet at the truck in ten minutes all good "
         "here any news from base signal is weak up on the ridge going to check the gate back "
         "soon").split()
CRASH = re.compile(r"Guru Meditation|Backtrace:|abort\(\)|panic|rst:0x|Rebooting\.\.\.|\[BOOT\] reset reason")
HB = re.compile(r"\[HB\] up=(\d+) heap=(\d+) int=(\d+) intmin=(\d+) intblk=(\d+) psram=(\d+) psmin=(\d+) "
                r"q=(-?\d+) ob=(\d+)")
FRAME = re.compile(r"\[UI\] frame (\d+)ms .*?(\d+) ms after the last key")
SLOW = re.compile(r"\[SLOW\] (\S+) (\d+) ms")
INMSG = re.compile(r'\[MSG\] in from ([0-9a-f]{32}): "PP (\S+) (\d+)')


def ts():
    return time.strftime("%H:%M:%S")


class Unit:
    def __init__(self, port, outdir):
        self.port = port
        # "COM3.log" is a reserved name on Windows
        self.log = open(os.path.join(outdir, "unit_" + port + ".log"), "a", encoding="utf-8", errors="replace")
        self.lines = []
        self.lock = threading.Lock()
        self.ser = None
        self.alive = True
        self.scan = 0                 # main thread: next line to look at for events
        self.hex = self.name = self.tag = ""
        self.ev = []                  # (time, text) problems seen
        self.hb = None                # last heartbeat tuple
        self.hb_at = time.time()
        self.first_hb = None
        self.min_int = self.min_blk = self.min_ps = 10 ** 9
        self.max_q = self.max_ob = 0
        self.boots = 0
        self.slow = {}                # name -> [count, max ms]
        self.frames = []              # key -> panel ms
        self.ui_ok = self.ui_bad = self.keys_ok = self.keys_bad = 0
        self.last_key = 0             # when the script last pressed a key here
        self.last_send = 0            # the firmware refuses a second send within 3 s
        self.open()
        threading.Thread(target=self.reader, daemon=True).start()

    def open(self):
        for _ in range(60):
            try:
                s = serial.Serial()
                s.port, s.baudrate, s.timeout, s.write_timeout = self.port, 115200, 0.2, 3
                s.dtr, s.rts = True, False
                s.open()
                self.ser = s
                return True
            except Exception:
                time.sleep(2)
        return False

    def reader(self):
        buf = ""
        while self.alive:
            try:
                data = self.ser.read(self.ser.in_waiting or 1)
            except Exception:
                self.note("serial port lost; reopening")
                try:
                    self.ser.close()
                except Exception:
                    pass
                time.sleep(3)
                self.open()
                continue
            if not data:
                continue
            buf += data.decode(errors="replace")
            *done, buf = buf.split("\n")
            for line in done:
                line = line.rstrip("\r")
                with self.lock:
                    self.lines.append((time.time(), line))
                self.log.write("%s %s\n" % (ts(), line))
            self.log.flush()

    def note(self, text):
        self.ev.append((ts(), text))
        self.log.write("%s ### %s\n" % (ts(), text))
        print("%s %s %s" % (ts(), self.port, text), flush=True)

    def mark(self):
        with self.lock:
            return len(self.lines)

    def send(self, cmd):
        if cmd.startswith("k "):
            self.last_key = time.time()
        try:
            self.ser.write((cmd + "\r\n").encode())
        except Exception as e:
            self.note("write failed: %s" % e)
        self.log.write("%s >> %s\n" % (ts(), cmd))

    def wait(self, rx, since, timeout):
        rx = re.compile(rx) if isinstance(rx, str) else rx
        end = time.time() + timeout
        i = since
        while time.time() < end:
            with self.lock:
                new = self.lines[i:]
            for _, line in new:
                m = rx.search(line)
                if m:
                    return m
            i += len(new)
            time.sleep(0.05)
        return None

    def cmd(self, cmd, rx, timeout=10):
        m0 = self.mark()
        self.send(cmd)
        return self.wait(rx, m0, timeout)


def sentence(n):
    out = []
    while len(" ".join(out)) < n:
        out.append(random.choice(WORDS))
    return " ".join(out)[:n].rstrip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ports", nargs="+", required=True)
    ap.add_argument("--minutes", type=float, default=60)
    ap.add_argument("--rate", type=float, default=150, help="mean s between a unit's own new messages")
    ap.add_argument("--reply", type=float, default=0.6, help="chance to answer a message")
    ap.add_argument("--ui", type=float, default=0.5, help="share of messages typed with keys")
    ap.add_argument("--tour", type=float, default=420, help="mean s between UI tours per unit")
    ap.add_argument("--drain", type=float, default=6, help="minutes to let queues drain at the end")
    ap.add_argument("--out", default=os.path.join("stress_out", time.strftime("chat4_%Y%m%d_%H%M")))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    units = [Unit(p, a.out) for p in a.ports]
    by_hex = {}
    time.sleep(1)
    for i, u in enumerate(units):
        m = u.cmd("id", r"\[ID\] dest=([0-9a-f]{32}) name=(.*)", 15)
        if not m:
            print("%s: no answer to id" % u.port)
            sys.exit(1)
        u.hex, u.name, u.tag = m.group(1), m.group(2).strip(), "u%d" % (i + 1)
        by_hex[u.hex] = u
        u.send("hb 60")
        print("%s %s %s %s" % (u.port, u.tag, u.hex, u.name), flush=True)
    for u in units:                      # everyone announces so all know each other
        u.send("a")
        time.sleep(4)
    time.sleep(20)

    sent = {}                            # (from tag, to tag) -> highest n
    unsent = set()                       # (from tag, to tag, n) typed but never went out
    got = {}                             # (from tag, to tag) -> set of n
    t0 = time.time()
    end = t0 + a.minutes * 60
    nxt = {u.port: t0 + random.expovariate(1 / a.rate) for u in units}
    tour = {u.port: t0 + random.uniform(60, a.tour) for u in units}
    replies = []                         # (when, unit, peer unit)
    last_sum = 0

    def send_msg(u, v, why):
        wait = u.last_send + 5 - time.time()
        if wait > 0:
            time.sleep(wait)
        u.last_send = time.time()
        n = sent.get((u.tag, v.tag), 0) + 1
        sent[(u.tag, v.tag)] = n
        text = "PP %s %d %s" % (u.tag, n, sentence(random.randint(12, 80)))
        if random.random() < a.ui:
            m0 = u.mark()
            if not u.cmd("chat " + v.hex, r"\[UI\] chat \S+ (open|NOT open)", 10):
                u.note("chat %s: no answer (UI busy?)" % v.tag)
                u.keys_bad += 1
                return
            m0 = u.mark()
            for i in range(0, len(text), 6):     # a fast typist: ~7 keys a second
                u.send("k " + text[i:i + 6])
                time.sleep(0.8)
            u.send("k ~")
            m = u.wait(r'\[TX\] .*"(PP %s %d [^"]*)"' % (u.tag, n), m0, 30)
            if m and m.group(1).rstrip() == text.rstrip():
                u.keys_ok += 1
            elif m:
                u.keys_bad += 1
                u.note("typed text differs: sent %r got %r" % (text, m.group(1)))
            else:
                u.keys_bad += 1
                unsent.add((u.tag, v.tag, n))
                tail = [l for _, l in u.lines[m0:] if "[TX]" in l or "[MSG]" in l or "[SLOW]" in l][-4:]
                u.note("typed message %d to %s: no [TX] in 30 s (%s)" % (n, v.tag, " | ".join(tail)))
                u.send("k ^^")                   # clear what is left in the compose line
            time.sleep(1.5)
            u.send("k ^")
        else:
            if not u.cmd("msg %s %s" % (v.hex, text), r"\[MSG\] send (queued|outboxed|failed)", 15):
                u.note("msg %d to %s: no answer" % (n, v.tag))

    def tour_once(u):
        u.send("k ^")
        time.sleep(1.5)
        steps = [("I", "Inbox"), ("C", "Contacts"), ("P", "Peers"), ("A", "MAP")]
        random.shuffle(steps)
        for key, want in steps[:random.randint(2, 4)]:
            m = u.cmd("k " + key, r"\[UI\] -> (.*)|(\[MAP\] open.*)", 8)
            got = (m.group(1) or m.group(2)) if m else ""
            if m and re.search(want, got):
                u.ui_ok += 1
            else:
                u.ui_bad += 1
                u.note("tour key %s: wanted %s, got %s" % (key, want, got or "nothing in 8 s"))
            if key == "A" and m:
                for k in random.sample(["TT", "FF", "VV", "HH", "Z", "W"], 3):
                    u.send("k " + k)
                    time.sleep(1.5)
            time.sleep(random.uniform(1, 4))
        u.send("k ^")

    def health():
        now = time.time()
        for u in units:
            with u.lock:
                new = u.lines[u.scan:]
            u.scan += len(new)
            for t, line in new:
                if "[KEY] queue full" in line:
                    u.note(line.strip())
                if CRASH.search(line):
                    u.note("CRASH/RESET: " + line.strip()[:160])
                m = HB.search(line)
                if m:
                    v = tuple(int(x) for x in m.groups())
                    if u.hb and v[0] < u.hb[0]:
                        u.boots += 1
                        u.note("RESTARTED (uptime %d -> %d)" % (u.hb[0], v[0]))
                        u.send("hb 60")
                    if not u.first_hb:
                        u.first_hb = v
                    u.hb, u.hb_at = v, t
                    u.min_int, u.min_blk, u.min_ps = min(u.min_int, v[3]), min(u.min_blk, v[4]), min(u.min_ps, v[6])
                    u.max_q, u.max_ob = max(u.max_q, v[7]), max(u.max_ob, v[8])
                m = SLOW.search(line)
                if m:
                    s = u.slow.setdefault(m.group(1), [0, 0])
                    s[0] += 1
                    s[1] = max(s[1], int(m.group(2)))
                m = FRAME.search(line)
                if m:                            # only frames that follow the script's own key press
                    after = int(m.group(2))
                    since = (t - u.last_key) * 1000
                    if since < 30000 and abs(after - since) < 2000:
                        u.frames.append(after)
                        if after > 3000:
                            u.note("slow key response: " + line.strip())
                m = INMSG.search(line)
                if m:
                    src = by_hex.get(m.group(1))
                    got.setdefault((m.group(2), u.tag), set()).add(int(m.group(3)))
                    if src and now < end and random.random() < a.reply:
                        replies.append((now + random.uniform(5, 40), u, src))
                if "[BOOT] ready" in line:
                    u.send("hb 60")
            if now - u.hb_at > 200:
                if u.cmd("hb", HB, 15):
                    u.hb_at = time.time()
                else:
                    u.note("HANG? no heartbeat for %d s and no answer to hb" % (now - u.hb_at))
                    u.hb_at = time.time()

    def summary(final=False):
        el = (time.time() - t0) / 60
        out = ["chat4 %s, %.0f of %.0f min%s" % (time.strftime("%Y-%m-%d %H:%M"), el, a.minutes,
                                                 " (FINAL)" if final else "")]
        for u in units:
            fr = sorted(u.frames)
            p = (lambda q: fr[min(len(fr) - 1, int(len(fr) * q))] if fr else 0)
            hb = u.hb or (0,) * 9
            out.append("%s %s %s: restarts %d, heap int now %d min %d largest-block min %d, psram min %d, "
                       "lxmf queue max %d, outbox max %d, up %d s" %
                       (u.port, u.tag, u.name, u.boots, hb[2], u.min_int, u.min_blk, u.min_ps, u.max_q, u.max_ob, hb[0]))
            out.append("    keys typed ok %d bad %d, tour screens ok %d bad %d, key->panel ms median %d p95 %d max %d (n=%d)" %
                       (u.keys_ok, u.keys_bad, u.ui_ok, u.ui_bad, p(0.5), p(0.95), fr[-1] if fr else 0, len(fr)))
            if u.slow:
                out.append("    slow steps: " + ", ".join("%s x%d max %d ms" % (k, c, mx) for k, (c, mx) in
                                                          sorted(u.slow.items(), key=lambda kv: -kv[1][1])[:8]))
            for t, e in u.ev[-15:]:
                out.append("    %s %s" % (t, e))
        tot_s = sum(sent.values())
        tot_g = sum(len(s) for s in got.values())
        out.append("messages sent %d, arrived %d" % (tot_s, tot_g))
        for (f, to), n in sorted(sent.items()):
            g = got.get((f, to), set())
            lost = [x for x in range(1, n + 1) if x not in g and (f, to, x) not in unsent]
            out.append("    %s -> %s: sent %d (typed, never went out %d), arrived %d, missing %d%s" %
                       (f, to, n, len([1 for k in unsent if k[:2] == (f, to)]), len(g), len(lost),
                        (" " + str(lost[:12])) if lost else ""))
        txt = "\n".join(out)
        open(os.path.join(a.out, "summary.txt"), "w").write(txt + "\n")
        return txt

    print("running %.0f min, logs in %s" % (a.minutes, a.out), flush=True)
    while time.time() < end:
        health()
        now = time.time()
        for when, u, v in [r for r in replies if r[0] <= now]:
            replies.remove((when, u, v))
            send_msg(u, v, "reply")
        for u in units:
            if now >= nxt[u.port]:
                nxt[u.port] = now + random.expovariate(1 / a.rate)
                send_msg(u, random.choice([v for v in units if v is not u]), "new")
            if now >= tour[u.port]:
                tour[u.port] = now + random.expovariate(1 / a.tour)
                tour_once(u)
        if now - last_sum > 300:
            last_sum = now
            summary()
        time.sleep(0.2)

    print("%s sending stopped; draining %.0f min" % (ts(), a.drain), flush=True)
    dend = time.time() + a.drain * 60
    while time.time() < dend:
        health()
        time.sleep(0.5)
    stats = []
    for u in units:
        for v in units:
            if u is v:
                continue
            m = u.cmd("msgstat " + v.hex, r"\[STAT\].*", 180)
            stats.append("%s about %s: %s" % (u.tag, v.tag, m.group(0) if m else "no answer"))
        u.send("slow")
    time.sleep(3)
    health()
    txt = summary(True) + "\nmsgstat, whole conversation incl. earlier tests (receiver about sender; out = its sends):\n    " + \
        "\n    ".join(stats)
    open(os.path.join(a.out, "summary.txt"), "w").write(txt + "\n")
    print(txt, flush=True)
    for u in units:
        u.send("hb 0")
        u.alive = False


if __name__ == "__main__":
    main()
