#!/usr/bin/env python3
"""Fresh-start fleet test: new radios are set up and get to know each other through the
screens, the way a person would do it, along randomly chosen paths.

    python tools/stress/fresh_fleet.py                       # 4 simulated radios, seed = time
    python tools/stress/fresh_fleet.py --radios 3 --seed 7
    python tools/stress/fresh_fleet.py --ports COM3 COM5      # real units: ERASES them first
    (RS_SIM_EXE=<ratspeak-sim.exe>, else sim/build/ratspeak-sim.exe)

Each radio:
  1. starts brand new: an empty data folder (simulator) or a full erase + flash
     (tools/flash_new_device.py --erase), and an SD card with the library on it (copied into
     the simulator's card folder, or sent with tools/sdcard/push_files.py);
  2. walks the setup wizard with key presses: all defaults (Enter x10), or own picks
     (a typed name, time zone, 12/24 h, energy mode, idle screen, Minimal / Typical /
     Everything, no password), or 0 = skip, then a name and an announce by keys
     (M H N or M P R);
  3. then every pair of radios becomes contacts, each direction by a random path the
     radio's mode allows:
       rename - Peers (P), the other radio's row, Enter, 2 (Rename), keep the announced
                name or type a nickname, Enter
       card   - the other radio sends its own card: C, MyID, Enter, 3 (Share), the
                recipient (from Contacts, or P for Peers), Enter; this radio opens the
                chat (Inbox) and Enter on the empty line saves it
     (Minimal has no Peers: a Minimal radio only gets contacts as cards, and can only send
      its card to radios it has saved already.)
Checks: every radio hears every other by its announced name (not the address), each saved
contact has the expected name, nobody lists itself, nothing extra, the wizard is done on all,
the Library has the uploaded files, no crash or restart. The screens are read with the
console command "ui" (screen, selection, rows) and "contacts".
Logs and summary.txt in --out. Exit 0 = pass.
"""
import argparse
import glob
import os
import random
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pingpong import ROOT, SerialDevice, SimDevice  # noqa: E402

LIB_SRC = os.path.join(ROOT, "tools", "sdcard", "library")
OUT_DEFAULT = os.path.join(ROOT, "sim", "build", "fresh_fleet")

# Key matrix (sim/app/DeviceWidget.cpp, remote/ScreenMirror.h): raw = row*10 + col + 1,
# | 0x80 while pressed.
KEYPOS = {}
for i, ch in enumerate("QWERTYUIOP"):
    KEYPOS[ch] = (0, 9 - i)
for i, ch in enumerate("ASDFGHJKL"):
    KEYPOS[ch] = (1, 9 - i)
KEYPOS["BKSP"] = (1, 0)
for i, k in enumerate(["ALT", "Z", "X", "C", "V", "B", "N", "M", "$", "ENTER"]):
    KEYPOS[k] = (2, 9 - i)
KEYPOS.update({"RSHIFT": (3, 0), "SYM": (3, 1), "SPACE": (3, 2), "MIC": (3, 3), "BACK": (3, 4)})
DIGIT = {"1": "W", "2": "E", "3": "R", "4": "S", "5": "D", "6": "F", "7": "Z", "8": "X", "9": "C", "0": "MIC"}
WIZ_DONE = 9                      # WZ_DONE: Enter = announce + finish
ENTRY_NAME = 2                    # Entry::Name


class Fail(Exception):
    pass


class Radio:
    def __init__(self, idx, dev, rnd, log):
        self.idx, self.dev, self.rnd, self.log = idx, dev, rnd, log
        self.hash = self.name = None
        self.mode = None          # 0 Minimal, 1 Typical, 2 Everything
        self.plan = ""
        self.want_name = None     # the name typed during setup, if any

    # -- keys ------------------------------------------------------------------------
    def _raw(self, key, down):
        r, c = KEYPOS[key]
        self.dev.send("@K %d" % ((r * 10 + c + 1) | (0x80 if down else 0)))
        time.sleep(0.04)

    def key(self, k, pause=0.35):
        if k in DIGIT:            # digits: SYM held + letter
            self._raw("SYM", True)
            self._raw(DIGIT[k], True)
            self._raw(DIGIT[k], False)
            self._raw("SYM", False)
        else:
            self._raw(k, True)
            self._raw(k, False)
        time.sleep(pause)

    def press(self, k, pause=0.35):
        """A key on a screen that may have gone idle meanwhile: wake it first (the waking key
        does nothing else), then the key."""
        self.wake()
        self.key(k, pause)

    def type(self, text):
        for ch in text:
            self.key("SPACE" if ch == " " else ch, 0.15)
        time.sleep(0.3)

    # -- reading the screen ----------------------------------------------------------
    def ui(self, timeout=15):
        start = self.dev.mark()
        self.dev.send("ui")
        i, m = self.dev.wait_for(r"\[UI\] screen=", timeout, start)
        if i is None:
            raise Fail("%s: no answer to 'ui'" % self.label())
        head = dict(re.findall(r"(\w+)=(\S+)", self.dev.lines[i]))
        n = int(head.get("rows", 0))
        rows, srows = [], []
        end = time.time() + 5
        while time.time() < end:
            with self.dev.lock:
                tail = self.dev.lines[i + 1:]
            rows = [r for r in tail if r.startswith("[UI] row ")]
            srows = [r for r in tail if r.startswith("[UI] srow ")]
            if len(rows) >= n or len(srows) >= n:
                break
            time.sleep(0.1)
        head["rowlist"] = []
        for r in rows[:n]:
            mm = re.match(r"\[UI\] row (\d+) ([0-9a-f]{32}) (.*) saved=(\d) self=(\d)", r)
            if mm:
                head["rowlist"].append({"i": int(mm.group(1)), "hash": mm.group(2), "name": mm.group(3),
                                        "saved": mm.group(4) == "1", "self": mm.group(5) == "1"})
        head["srowlist"] = srows[:n]
        return head

    def wait_ui(self, cond, what, timeout=20):
        end = time.time() + timeout
        u = None
        while time.time() < end:
            u = self.ui()
            if cond(u):
                return u
            time.sleep(0.5)
        raise Fail("%s: %s (screen %s)" % (self.label(), what, {k: v for k, v in (u or {}).items()
                                                                if k not in ("rowlist", "srowlist")}))

    def label(self):
        return "radio %d (%s)" % (self.idx, self.name or "?")

    def wake(self):
        u = self.ui()
        if u.get("idle") == "1":
            self.key("SPACE", 0.8)
            u = self.wait_ui(lambda x: x.get("idle") == "0", "did not wake")
        return u

    def clear(self):
        """Close menus, chats, cards and text entry (^) until a plain screen shows."""
        for _ in range(6):
            u = self.wake()
            busy = (u["menu"] != "0" or u["card"] != "0" or u["thread"] != "-" or u["entry"] != "0"
                    or u["share"] != "0" or u["lib"] != "0" or (u["screen"] == "Settings" and u["page"] != "0"))
            if not busy:
                return u
            self.key("BACK", 0.6)
        raise Fail("%s: could not get back to a plain screen" % self.label())

    def go(self, letter, screen):
        """M (menu), then the screen's letter."""
        self.clear()
        self.key("M", 0.6)
        self.key(letter, 0.8)
        return self.wait_ui(lambda u: u["screen"] == screen and u["menu"] == "0", "did not open %s" % screen)

    def select(self, target):
        u = self.ui()
        sel = int(u["sel"])
        for _ in range(abs(target - sel)):
            self.key("V" if target > sel else "T", 0.25)
        u = self.wait_ui(lambda x: int(x["sel"]) == target, "selection did not reach row %d" % target, 10)
        return u

    def identity_page(self):
        """Settings (M S), then 7 = Identity."""
        self.go("S", "Settings")
        self.key("7", 0.8)
        return self.wait_ui(lambda u: any("Share all contacts" in s for s in u["srowlist"]),
                            "7 did not open Identity (S.7)")

    def ident(self):
        m = self.dev.command("id", r"\[ID\] dest=([0-9a-f]{32}) name=(.*)$", 15)
        if not m:
            raise Fail("radio %d: no [ID] answer" % self.idx)
        self.hash, self.name = m.group(1), m.group(2).strip()

    def announce_keys(self):
        """M H N (Home: Announce ID) or M P R (Peers: announce)."""
        start = self.dev.mark()
        if self.mode == 0:
            self.dev.send("a")    # Minimal has no Home / Peers: the console (not a UI path)
            how = "console a"
        else:
            how = self.rnd.choice(["M H N", "M P R", "S.7.4"])
            if how == "M H N":
                self.go("H", "Home")
                self.key("N", 1.0)
            elif how == "M P R":
                self.go("P", "Peers")
                self.key("R", 1.0)
            else:
                self.identity_page()
                self.key("4", 1.0)                             # Announce ID
        i, _ = self.dev.wait_for(r"\[ANN\] sent", 20, start)
        if i is None:
            raise Fail("%s: announce (%s) not sent" % (self.label(), how))
        return how


# ---- setup wizard ----------------------------------------------------------------------
def walk_wizard(r, names_left, allow_minimal):
    u = r.wait_ui(lambda x: x["wiz"] == "0", "setup wizard not on screen after a new start", 60)
    plan = r.rnd.choices(["defaults", "custom", "skip"], [35, 45, 20])[0]
    steps = []
    if plan == "skip":
        r.press("0", 1.0)
        r.wait_ui(lambda x: x["wiz"] == "-1" and x["wizdone"] == "1", "0 did not skip setup")
        nm = names_left.pop()
        if int(r.ui()["mode"]) == 0:          # Minimal shows no Identity page
            r.dev.command("name %s" % nm, r"\[NAME\] this device is now", 10)
            steps.append("skip, name by console")
        else:                                  # S.7, 2 = Display name
            r.identity_page()
            r.press("2", 0.8)
            r.wait_ui(lambda x: x["entry"] == str(ENTRY_NAME), "S.7.2 did not open the name entry")
            for _ in range(24):
                r.key("BKSP", 0.08)
            r.type(nm)
            r.press("ENTER", 0.8)
            r.wait_ui(lambda x: x["entry"] == "0", "name entry did not close")
            steps.append("skip, name in S.7.2")
        r.want_name = nm
    elif plan == "defaults":
        for s in range(WIZ_DONE):
            r.press("ENTER", 0.6)
            r.wait_ui(lambda x, s=s: x["wiz"] == str(s + 1), "Enter did not go to step %d" % (s + 2))
        steps.append("Enter x9")
    else:
        r.press("ENTER", 0.6)                                    # Welcome
        r.wait_ui(lambda x: x["wiz"] == "1", "no Name step")
        if r.rnd.random() < 0.7:                               # 1 = type a name
            nm = names_left.pop()
            r.press("1", 0.8)
            r.wait_ui(lambda x: x["entry"] == str(ENTRY_NAME), "1 did not open the name entry")
            for _ in range(24):
                r.key("BKSP", 0.08)
            r.type(nm)
            r.press("ENTER", 0.8)
            r.wait_ui(lambda x: x["entry"] == "0", "name entry did not close")
            steps.append("name " + nm)
            r.want_name = nm
        r.press("ENTER", 0.6)
        r.wait_ui(lambda x: x["wiz"] == "2", "no Time zone step")
        for _ in range(r.rnd.randint(0, 2)):
            r.press("1", 0.5)
        r.press("ENTER", 0.6)
        r.wait_ui(lambda x: x["wiz"] == "3", "no Clock step")
        r.press(r.rnd.choice("12"), 0.6)                         # 24 / 12 h
        r.wait_ui(lambda x: x["wiz"] == "4", "clock pick did not go on")
        pm = r.rnd.choice("1234")
        r.press(pm, 0.6)                                         # Full / Balanced / Eco / Saver
        r.wait_ui(lambda x: x["wiz"] == "5", "energy pick did not go on")
        r.press(r.rnd.choice("123"), 0.6)                        # idle screen
        r.wait_ui(lambda x: x["wiz"] == "6", "idle pick did not go on")
        mode = r.rnd.choices("123", [15 if allow_minimal else 0, 50, 35])[0]
        r.press(mode, 0.6)
        r.wait_ui(lambda x: x["wiz"] == "7", "mode pick did not go on")
        r.press("1", 0.6)                                        # no password
        r.wait_ui(lambda x: x["wiz"] == "8", "lock pick did not go on")
        r.press("ENTER", 0.6)                                    # Import: nothing
        r.wait_ui(lambda x: x["wiz"] == str(WIZ_DONE), "no Done step")
        steps.append("energy %s mode %s" % (pm, mode))
    r.plan = plan + " (" + ", ".join(steps) + ")"
    return plan


def finish_wizard(r):
    r.wake()
    start = r.dev.mark()
    r.key("ENTER", 1.0)
    r.wait_ui(lambda x: x["wiz"] == "-1" and x["wizdone"] == "1", "Enter on Done did not finish setup")
    if r.dev.wait_for(r"\[ANN\] sent", 20, start)[0] is None:
        raise Fail("%s: finishing setup sent no announce" % r.label())


# ---- contacts --------------------------------------------------------------------------
def peers_heard(r):
    """Hashes -> names on the Peers list (read with the console, any mode)."""
    r.dev.send("p")
    time.sleep(0.8)
    u = r.ui()
    return {row["hash"]: row["name"] for row in u["rowlist"] if not row["self"]}


def by_rename(x, y, nick):
    u = x.go("P", "Peers")
    rows = [row for row in u["rowlist"] if row["hash"] == y.hash]
    if not rows:
        raise Fail("%s: %s not on Peers" % (x.label(), y.name))
    x.select(rows[0]["i"])
    x.key("ENTER", 0.8)
    x.wait_ui(lambda v: v["card"] == "1", "Enter on a peer did not open its card")
    x.key("2", 0.8)                                            # Rename
    x.wait_ui(lambda v: v["entry"] == str(ENTRY_NAME), "2 did not open Rename")
    if nick:
        for _ in range(24):
            x.key("BKSP", 0.08)
        x.type(nick)
    x.key("ENTER", 0.8)
    x.wait_ui(lambda v: v["entry"] == "0", "Rename did not save (Enter)")


def by_card(x, y):
    """y sends its own card to x; x saves it from the chat."""
    u = y.go("C", "Contacts")
    me = [row for row in u["rowlist"] if row["self"]]
    if not me:
        raise Fail("%s: no MyID row on Contacts" % y.label())
    y.select(me[0]["i"])
    y.key("ENTER", 0.8)
    y.wait_ui(lambda v: v["card"] == "1", "Enter on MyID did not open the card")
    y.key("3", 0.8)                                            # Share
    u = y.wait_ui(lambda v: v["share"] == "1", "3 did not start Share")
    rows = [row for row in u["rowlist"] if row["hash"] == x.hash]
    how = "Contacts"
    if not rows:                                               # not a contact yet: P = Peers
        y.key("P", 0.8)
        u = y.wait_ui(lambda v: v["screen"] == "Peers" and v["share"] == "1", "P did not show Peers to share to")
        rows = [row for row in u["rowlist"] if row["hash"] == x.hash]
        how = "Peers"
        if not rows:
            raise Fail("%s: %s not offered to share to" % (y.label(), x.name))
    y.select(rows[0]["i"])
    start = y.dev.mark()
    y.key("ENTER", 1.0)
    if y.dev.wait_for(r"\[CARD\] sent", 20, start)[0] is None:
        raise Fail("%s: card to %s not sent" % (y.label(), x.name))
    # x: the card arrives as a message; Inbox, the chat, Enter on the empty line.
    end = time.time() + 120
    while True:
        u = x.go("I", "Inbox")
        rows = [row for row in u["rowlist"] if row["hash"] == y.hash]
        if rows:
            x.select(rows[0]["i"])
            x.key("ENTER", 1.0)
            v = x.wait_ui(lambda v: v["thread"] == y.hash, "Enter did not open the chat")
            if v["tcard"] == "1":
                break
            x.key("BACK", 0.6)
        if time.time() > end:
            raise Fail("%s: card from %s never showed in the chat" % (x.label(), y.name))
        time.sleep(3)
    start = x.dev.mark()
    x.key("ENTER", 1.0)
    if x.dev.wait_for(r"\[CARD\] saved", 15, start)[0] is None:
        raise Fail("%s: Enter did not save the card from %s" % (x.label(), y.name))
    x.key("BACK", 0.6)
    return how


def contacts(r):
    start = r.dev.mark()
    r.dev.send("contacts")
    i, m = r.dev.wait_for(r"\[CONTACT\] (\d+) saved", 15, start)
    if i is None:
        raise Fail("%s: no contacts dump" % r.label())
    with r.dev.lock:
        lines = r.dev.lines[start:i]
    out = {}
    for ln in lines:
        mm = re.match(r"\[CONTACT\] ([0-9a-f]{32}) (.*?)( \(self\))?$", ln)
        if mm:
            out.setdefault(mm.group(1), []).append((mm.group(2), bool(mm.group(3))))
    return out


# ---- radios: simulator or units -------------------------------------------------------
def start_sim(i, a, out):
    data = os.path.join(out, "r%d" % i)
    lib = os.path.join(data, "sd", "ratdeckpro", "library")
    os.makedirs(lib)
    for f in glob.glob(os.path.join(LIB_SRC, "*")):            # the SD card, as after an upload
        shutil.copy(f, lib)
    os.environ.pop("RS_SIM_WAKE", None)
    return SimDevice("ff%d" % i, a.tcp + i, data, os.path.join(out, "r%d.log" % i), 0, a.air)


def start_unit(port, i, out):
    py = sys.executable
    r = subprocess.run([py, os.path.join(ROOT, "tools", "flash_new_device.py"), "--erase", port],
                       capture_output=True, text=True)
    open(os.path.join(out, "flash_%s.log" % port), "w").write(r.stdout + r.stderr)
    if r.returncode:
        raise Fail("%s: flashing failed (see flash_%s.log)" % (port, port))
    files = sorted(glob.glob(os.path.join(LIB_SRC, "*")))
    for _ in range(5):                                          # the unit must be up first
        r = subprocess.run([py, os.path.join(ROOT, "tools", "sdcard", "push_files.py"), "--port", port] + files,
                           capture_output=True, text=True)
        if r.returncode == 0:
            break
        time.sleep(10)
    open(os.path.join(out, "push_%s.log" % port), "w").write(r.stdout + r.stderr)
    if r.returncode:
        raise Fail("%s: SD upload failed (see push_%s.log)" % (port, port))
    return SerialDevice("u%d" % i, port, os.path.join(out, "r%d.log" % i))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--radios", type=int, default=4)
    ap.add_argument("--ports", nargs="+", help="real units (ERASED, then set up) instead of simulators")
    ap.add_argument("--seed", type=int, default=int(time.time()))
    ap.add_argument("--out", default=OUT_DEFAULT)
    ap.add_argument("--tcp", type=int, default=5861)
    ap.add_argument("--air", type=int, default=48471)
    ap.add_argument("--no-joiner", action="store_true", help="no late radio importing contacts in its wizard")
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    shutil.rmtree(a.out, ignore_errors=True)
    os.makedirs(a.out)
    joiner_on = not a.no_joiner and (len(a.ports) >= 3 if a.ports else True)
    n = (len(a.ports) - (1 if joiner_on else 0)) if a.ports else a.radios
    joiner_dev = None
    if joiner_on:   # one more radio, started after the others (on units: the last port)
        joiner_dev = (lambda: start_unit(a.ports[-1], n, a.out)) if a.ports else (lambda: start_sim(n, a, a.out))
    notes, fails, radios = [], [], []
    t0 = time.time()

    def note(s):
        notes.append(s)
        print(s, flush=True)

    note("seed %d, %d %s%s" % (a.seed, n, "units " + " ".join(a.ports) if a.ports else "simulated radios",
                                ", + 1 late radio" if joiner_on else ""))
    try:
        for i in range(n):
            dev = start_unit(a.ports[i], i, a.out) if a.ports else start_sim(i, a, a.out)
            radios.append(Radio(i, dev, random.Random(rnd.random()), note))
        for r in radios:
            if r.dev.wait_for(r"\[BOOT\] ready", 180)[0] is None and not r.dev.command("pw", r"\[POWER\] mode=", 15):
                raise Fail("radio %d did not start" % r.idx)

        # 1. setup
        names_left = ["CAMP NODE%s" % c for c in "ABCDEFGHJKLMNPQRSTUVWXYZ"]
        rnd.shuffle(names_left)
        minimal_left = max(1, n // 3)
        plans = {}
        for r in radios:
            plans[r.idx] = walk_wizard(r, names_left, allow_minimal=minimal_left > 0)
            r.mode = int(r.ui()["mode"])
            if r.mode == 0:
                minimal_left -= 1
        for r in radios:                         # all up and listening: now finish / announce
            if plans[r.idx] == "skip":
                r.plan += ", announce " + r.announce_keys()
            else:
                finish_wizard(r)
            r.ident()
            if r.want_name and r.name != r.want_name:
                fails.append("%s: typed the name %s, the radio says %s" % (r.label(), r.want_name, r.name))
            r.mode = int(r.ui()["mode"])
            time.sleep(2)
        for r in radios:
            note("radio %d %s %s mode %s: %s" % (r.idx, r.name, r.hash[:8], "MTE"[r.mode], r.plan))

        # 2. everyone hears everyone, by name
        for r in radios:
            want = {o.hash: o.name for o in radios if o is not r}
            end, tries = time.time() + 90, 0
            while True:
                heard = peers_heard(r)
                missing = [h for h in want if h not in heard]
                if not missing:
                    break
                if time.time() > end:
                    if tries >= 2:
                        raise Fail("%s never heard %s" % (r.label(), [want[h] for h in missing]))
                    tries += 1
                    for o in radios:
                        if o.hash in missing:
                            o.dev.send("a")
                    note("%s: re-announce asked of %s" % (r.label(), [want[h] for h in missing]))
                    end = time.time() + 60
                time.sleep(3)
            for h, nm in want.items():
                if heard[h] != nm:
                    fails.append("%s shows %s as '%s' on Peers (announced name '%s')" % (r.label(), h[:8], heard[h], nm))
            r.clear()

        # 3. contacts, each direction by a random path
        expect = {r.idx: {} for r in radios}
        pairs = [(x, y) for x in radios for y in radios if x is not y]
        rnd.shuffle(pairs)
        skipped = []
        for x, y in pairs:                               # x saves y
            ways = []
            if x.mode != 0:
                ways += ["rename", "rename-nick"]
            if y.mode != 0 or x.hash in expect[y.idx]:     # Minimal sends only to its contacts
                ways += ["card"]
            if not ways:
                skipped.append("%s -> %s" % (x.name, y.name))
                continue
            how = x.rnd.choice(ways)
            if how == "rename":
                by_rename(x, y, None)
                expect[x.idx][y.hash] = y.name
                detail = "Peers, rename, kept the name"
            elif how == "rename-nick":
                nick = "PAL%d%d" % (y.idx, x.rnd.randint(10, 99))
                by_rename(x, y, nick)
                expect[x.idx][y.hash] = nick
                detail = "Peers, rename to " + nick
            else:
                via = by_card(x, y)
                expect[x.idx][y.hash] = y.name
                detail = "card from %s (picked on %s), saved from the chat" % (y.name, via)
            note("%s saves %s: %s" % (x.name, y.name, detail))
        for s in skipped:
            note("no path (both Minimal, not contacts): " + s)

        # 4. a late radio imports contacts in its setup wizard: Import, 1 = listen (it announces
        #    then), and a radio that knows everyone sends S.7 "Share all contacts" to it.
        if joiner_dev:
            j = Radio(len(radios), joiner_dev(), random.Random(rnd.random()), note)
            if j.dev.wait_for(r"\[BOOT\] ready", 180)[0] is None and not j.dev.command("pw", r"\[POWER\] mode=", 15):
                raise Fail("late radio did not start")
            j.wait_ui(lambda x: x["wiz"] == "0", "late radio: no setup wizard", 60)
            for s_ in range(8):                                # Welcome .. Lock: defaults
                j.press("ENTER", 0.6)
                j.wait_ui(lambda x, s_=s_: x["wiz"] == str(s_ + 1), "late radio: Enter did not go on")
            start = j.dev.mark()
            j.press("1", 1.0)                                    # Listen over the radio
            if j.dev.wait_for(r"\[WIZ\] listen on", 10, start)[0] is None:
                raise Fail("late radio: 1 did not start listening")
            if j.dev.wait_for(r"\[ANN\] sent", 15, start)[0] is None:
                fails.append("late radio: listening did not announce (nobody can pick it to send to)")
            j.ident()
            z = rnd.choice([r for r in radios if r.mode != 0])
            end = time.time() + 90
            while j.hash not in peers_heard(z):
                if time.time() > end:
                    raise Fail("%s never heard the late radio %s" % (z.label(), j.name))
                time.sleep(3)
            z.identity_page()
            z.key("7", 0.8)                                    # Share all contacts
            u = z.wait_ui(lambda v: v["share"] == "1", "S.7.7 did not start Share all")
            if not [row for row in u["rowlist"] if row["hash"] == j.hash]:
                z.key("P", 0.8)
                u = z.wait_ui(lambda v: v["screen"] == "Peers" and v["share"] == "1", "P did not show Peers")
            rows = [row for row in u["rowlist"] if row["hash"] == j.hash]
            if not rows:
                raise Fail("%s: late radio not offered for Share all" % z.label())
            z.select(rows[0]["i"])
            start = z.dev.mark()
            z.key("ENTER", 1.0)
            if z.dev.wait_for(r"\[CARD\] shareall", 20, start)[0] is None:
                raise Fail("%s: Share all not sent" % z.label())
            want = dict(expect[z.idx])
            want[z.hash] = z.name                              # z's own card goes too
            want.pop(j.hash, None)
            end = time.time() + 60 + 15 * len(want)
            while True:
                with j.dev.lock:
                    got = sum(1 for ln in j.dev.lines if "[WIZ] contact saved" in ln)
                if got >= len(want) or time.time() > end:
                    break
                time.sleep(2)
            j.press("ENTER", 0.8)                                # Import -> Done
            j.wait_ui(lambda x: x["wiz"] == str(WIZ_DONE), "late radio: no Done step")
            finish_wizard(j)
            j.ident()
            j.mode = int(j.ui()["mode"])
            j.plan = "late, wizard import from %s (%d cards)" % (z.name, got)
            expect[j.idx] = want
            radios.append(j)
            note("radio %d %s %s: %s, expected %d" % (j.idx, j.name, j.hash[:8], j.plan, len(want)))

        # 5. checks
        lib_files = sorted(os.path.basename(f) for f in glob.glob(os.path.join(LIB_SRC, "*")))
        for r in radios:
            got = contacts(r)
            for h, entries in got.items():
                if len(entries) > 1:
                    fails.append("%s: %s saved %d times" % (r.label(), h[:8], len(entries)))
                if h == r.hash or entries[0][1]:
                    fails.append("%s lists itself as a contact" % r.label())
            for h, nm in expect[r.idx].items():
                if h not in got:
                    fails.append("%s: contact %s missing" % (r.label(), nm))
                elif got[h][0][0] != nm:
                    fails.append("%s: contact %s named '%s', expected '%s'" % (r.label(), h[:8], got[h][0][0], nm))
            for h in got:
                if h not in expect[r.idx] and h != r.hash:
                    fails.append("%s: unexpected contact %s %s" % (r.label(), h[:8], got[h][0][0]))
            u = r.ui()
            if u["wizdone"] != "1" or u["wiz"] != "-1":
                fails.append("%s: setup not finished" % r.label())
            start = r.dev.mark()
            r.dev.send("sdlsa /ratdeckpro/library")
            i, _ = r.dev.wait_for(r"\[SDLS\] (\d+ entries|cannot open)", 30, start)
            with r.dev.lock:
                listed = " ".join(r.dev.lines[start:(i or start) + 1])
            missing = [f for f in lib_files if f not in listed]
            if missing:
                fails.append("%s: Library missing %s" % (r.label(), missing))
            with r.dev.lock:
                bad = [ln for ln in r.dev.lines if re.search(r"Guru|abort\(\)|panic|Backtrace|\[MAIN\] setup", ln)]
            if len([b for b in bad if "[MAIN] setup" in b]) > 1 or any("[MAIN] setup" not in b for b in bad):
                fails.append("%s: crash or restart: %s" % (r.label(), bad[:3]))
    except Fail as e:
        fails.append(str(e))
    except Exception as e:  # noqa: BLE001 - the harness itself: report, do not hide
        fails.append("harness error: %r" % (e,))
    finally:
        for r in radios:
            try:
                r.dev.close()
            except Exception:
                pass
    summary = "fresh_fleet seed %d: %d radios, %s in %.0f s" % (
        a.seed, len(radios) or n, "PASS" if not fails else "FAIL (%d)" % len(fails), time.time() - t0)
    with open(os.path.join(a.out, "summary.txt"), "w", encoding="utf-8") as f:
        f.write(summary + "\n" + "\n".join("FAIL " + x for x in fails) + "\n\n" + "\n".join(notes) + "\n")
    for x in fails:
        print("FAIL " + x)
    print(summary)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
