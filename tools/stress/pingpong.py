#!/usr/bin/env python3
"""Two-device message ping-pong stress test.

Simulator (default): starts two simulators headless, each with its firmware USB serial on a
local TCP port, lets them announce to each other, then sends numbered messages both ways
("PP <tag> <n>") for --minutes. At the end it waits for the queues to drain and checks
every conversation with the firmware's `msgstat` command.

Units: --ports COM5 COM9 talks to two real T-Decks over USB serial instead (needs pyserial).
Opening a port resets the unit, so the script waits for boot first.

PASS when, for each direction: every message sent arrived exactly once (no duplicates, no
gaps), every outgoing record ended "delivered", no unit restarted, and the outbox drained.
The firmware heartbeat (`hb`) is logged every --hb seconds; the summary shows the first and
last values so slow memory growth stands out.

Examples:
  python tools/stress/pingpong.py --minutes 10
  python tools/stress/pingpong.py --minutes 480 --interval 20 --flashkb 1024
  python tools/stress/pingpong.py --ports COM5 COM9 --minutes 60 --interval 60
Logs: <out>/a.log, <out>/b.log, <out>/summary.txt
"""
import argparse
import os
import random
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SIM = os.path.join(ROOT, "sim", "build", "ratspeak-sim.exe")


class Device:
    """One firmware console: a simulator over TCP or a unit over a serial port."""

    def __init__(self, name, log_path):
        self.name = name
        self.lines = []          # every line received
        self.lock = threading.Lock()
        self.log = open(log_path, "w", encoding="utf-8", errors="replace")
        self.proc = None
        self.alive = True
        self.boots = 0

    def _reader(self, read_chunk):
        buf = b""
        while self.alive:
            try:
                chunk = read_chunk()
            except Exception:
                break
            if not chunk:
                if self.proc is not None:  # TCP closed
                    break
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r")
                stamp = time.strftime("%H:%M:%S")
                with self.lock:
                    self.lines.append(line)
                    if "[MAIN] setup" in line:
                        self.boots += 1
                self.log.write("%s %s\n" % (stamp, line))
                self.log.flush()

    def start_reader(self, read_chunk):
        threading.Thread(target=self._reader, args=(read_chunk,), daemon=True).start()

    def wait_for(self, pattern, timeout, start=0):
        """Index + match of the first line from `start` matching regex `pattern`."""
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                for i in range(start, len(self.lines)):
                    m = rx.search(self.lines[i])
                    if m:
                        return i, m
            time.sleep(0.2)
        return None, None

    def mark(self):
        with self.lock:
            return len(self.lines)

    def command(self, text, pattern=None, timeout=10):
        start = self.mark()
        self.send(text)
        if pattern:
            return self.wait_for(pattern, timeout, start)[1]
        return None


class SimDevice(Device):
    def __init__(self, name, port, data_dir, log_path, flashkb, air):
        super().__init__(name, log_path)
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen", RS_SIM_AIR_PORT=str(air),QT_QPA_FONTDIR=os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts"))
        args = [SIM, "--name", name, "--data", data_dir, "--fast", "--serial-tcp", str(port)]
        if flashkb:
            args += ["--flashkb", str(flashkb)]
        self.proc = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.sock = None
        for _ in range(100):
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                time.sleep(0.2)
        if not self.sock:
            raise RuntimeError("%s: no serial on TCP %d" % (name, port))
        self.sock.settimeout(None)
        self.start_reader(lambda: self.sock.recv(4096))

    def send(self, text):
        try:
            self.sock.sendall((text + "\n").encode())
        except OSError as e:
            code = self.proc.poll()
            raise RuntimeError("%s: serial lost (%s); simulator %s" % (
                self.name, e, "still running" if code is None else "exited with code %d (0x%X)" % (code, code & 0xFFFFFFFF)))

    def close(self):
        self.alive = False
        try:
            self.sock.close()
        except Exception:
            pass
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except Exception:
                self.proc.kill()
        self.log.close()


class SerialDevice(Device):
    def __init__(self, name, port, log_path):
        super().__init__(name, log_path)
        import serial  # pyserial
        self.ser = serial.Serial(port, 115200, timeout=0.5)
        self.start_reader(lambda: self.ser.read(4096))

    def send(self, text):
        self.ser.write((text + "\n").encode())

    def close(self):
        self.alive = False
        try:
            self.ser.close()
        except Exception:
            pass
        self.log.close()


def parse_kv(line):
    return {k: v for k, v in re.findall(r"(\w+)=(\S+)", line)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--minutes", type=float, default=10)
    ap.add_argument("--interval", type=float, default=8, help="seconds between sends (random 0.5x-1.5x), each side")
    ap.add_argument("--hb", type=int, default=60, help="heartbeat period, seconds")
    ap.add_argument("--drain", type=float, default=600, help="max seconds to wait for both outboxes to empty")
    ap.add_argument("--flashkb", type=int, default=0, help="simulator flash size (small = full-flash path)")
    ap.add_argument("--flashcap", type=int, default=0, help="send 'flashcap N' first (removal after ~N msgs)")
    ap.add_argument("--ports", nargs=2, metavar=("A", "B"), help="real units (COMx COMy) instead of simulators")
    ap.add_argument("--out", default=os.path.join(ROOT, "sim", "build", "pingpong"))
    ap.add_argument("--seed", type=int, default=int(time.time()))
    ap.add_argument("--tcp", type=int, default=5701, help="first of two local TCP ports for the simulators' serial")
    ap.add_argument("--air", type=int, default=48101, help="simulated radio channel (UDP port), apart from other sims")
    a = ap.parse_args()
    random.seed(a.seed)

    os.makedirs(a.out, exist_ok=True)
    devs = []
    try:
        if a.ports:
            devs = [SerialDevice("a", a.ports[0], os.path.join(a.out, "a.log")),
                    SerialDevice("b", a.ports[1], os.path.join(a.out, "b.log"))]
            boot_wait = 40
        else:
            if not os.path.exists(SIM):
                sys.exit("build the simulator first: sim\\build-sim.bat")
            for n, port in (("ppa", a.tcp), ("ppb", a.tcp + 1)):
                d = os.path.join(a.out, "data", n)
                shutil.rmtree(d, ignore_errors=True)
                devs.append(SimDevice(n, port, d, os.path.join(a.out, "%s.log" % n[-1]), a.flashkb, a.air))
            boot_wait = 30
        A, B = devs
        for d in devs:
            if d.wait_for(r"\[BOOT\] ready", boot_wait)[0] is None:
                print("%s: no [BOOT] ready line yet, continuing" % d.name)
        time.sleep(3)
        boots0 = [d.boots for d in devs]
        ids = []
        for d in devs:
            m = d.command("id", r"\[ID\] dest=([0-9a-f]{32})", 10)
            if not m:
                sys.exit("%s: no id" % d.name)
            ids.append(m.group(1))
            d.send("hb %d" % a.hb)
            if a.flashcap:
                d.send("flashcap %d" % a.flashcap)
        print("A %s  B %s  seed %d" % (ids[0], ids[1], a.seed))
        # Announce both ways until each side knows the other (a message to an unknown key is dropped).
        for _ in range(3):
            A.send("a")
            time.sleep(4)
            B.send("a")
            time.sleep(6)
        sent = [0, 0]        # accepted by the firmware (numbers 1..sent stay contiguous)
        slowest = [0.0, 0.0]  # longest wait for the firmware's answer to "msg"
        refused = [0, 0]     # "send failed" (e.g. the 3 s send spacing, outbox full): number reused
        tag = ["A2B", "B2A"]
        end = time.time() + a.minutes * 60
        nxt = [time.time() + random.uniform(0, a.interval) for _ in devs]
        last_print = 0
        while time.time() < end:
            now = time.time()
            for i, d in enumerate(devs):
                if now >= nxt[i]:
                    t_cmd = time.time()
                    m = d.command("msg %s PP %s %d" % (ids[1 - i], tag[i], sent[i] + 1),
                                  r"\[MSG\] (send queued|send failed|outbox)", 60)
                    slowest[i] = max(slowest[i], time.time() - t_cmd)
                    if not m:
                        sys.exit("%s: no answer to msg within 60 s" % d.name)
                    if m.group(1) != "send failed":
                        sent[i] += 1
                    else:
                        refused[i] += 1
                    nxt[i] = time.time() + a.interval * random.uniform(0.5, 1.5)
            if now - last_print > 60:
                last_print = now
                print("%s  sent A->B %d  B->A %d  (refused %d / %d)" % (
                    time.strftime("%H:%M:%S"), sent[0], sent[1], refused[0], refused[1]))
            time.sleep(0.2)
        # Drain: wait until both outboxes are empty (heartbeat q=0), up to --drain seconds.
        print("sending done: A->B %d, B->A %d (refused %d / %d); draining up to %ds" % (
            sent[0], sent[1], refused[0], refused[1], a.drain))
        t0 = time.time()
        while time.time() - t0 < a.drain:
            qs = []
            for d in devs:
                m = d.command("hb", r"\[HB\].*", 10)
                qs.append(int(parse_kv(m.group(0)).get("q", "1")) if m else 1)
            if all(q == 0 for q in qs):
                break
            time.sleep(10)
        drained = time.time() - t0
        time.sleep(15)  # last proofs

        # ---- verdict ----
        ok = True
        report = ["ping-pong %s, %.1f min, interval %.0fs, seed %d%s%s; queues drained in %.0f s" % (
            "units %s %s" % tuple(a.ports) if a.ports else "simulator", a.minutes, a.interval, a.seed,
            ", flashkb %d" % a.flashkb if a.flashkb else "", ", flashcap %d" % a.flashcap if a.flashcap else "",
            drained),
            "refused by the firmware (number reused): A %d, B %d; slowest send answer A %.1f s, B %.1f s" % (
                refused[0], refused[1], slowest[0], slowest[1])]
        for i, d in enumerate(devs):
            o = 1 - i
            rx = devs[o].command("msgstat %s" % ids[i], r"\[STAT\].*", 120)
            tx = d.command("msgstat %s" % ids[o], r"\[STAT\].*", 120)
            hb = d.command("hb", r"\[HB\].*", 10)
            if not rx or not tx:
                report.append("%s: msgstat missing" % d.name)
                ok = False
                continue
            r, t = parse_kv(rx.group(0)), parse_kv(tx.group(0))
            got, dup, gap = int(r["pp"]), int(r["dup"]), int(r["gap"])
            delivered = int(t["delivered"])
            restarts = d.boots - boots0[i]
            # New incoming messages the receiver saved since boot (heartbeat inc=; duplicates not counted).
            rhb = devs[o].command("hb", r"\[HB\].*", 10)
            arrived = int(parse_kv(rhb.group(0)).get("inc", "-1")) if rhb else -1
            line = ("%s->%s: sent %d, arrived %d, kept %d (dup %d, gap %d), sender shows delivered %d / sent %d / "
                    "failed %d / queued %d; restarts %d" % (
                        d.name, devs[o].name, sent[i], arrived, got, dup, gap, delivered, int(t["sent"]),
                        int(t["failed"]), int(t["queued"]) + int(t["sending"]), restarts))
            good = (arrived == sent[i] and got == sent[i] and dup == 0 and gap == 0 and delivered == sent[i]
                    and restarts == 0)
            if a.flashkb or a.flashcap:  # old messages are removed on purpose: arrivals + newest kept
                good = arrived == sent[i] and dup == 0 and restarts == 0 and int(r["ppmax"]) == sent[i]
            report.append(("OK   " if good else "FAIL ") + line)
            ok &= good
            hbs = [l for l in d.lines if l.startswith("[HB]")]
            if hbs:
                report.append("     first %s" % hbs[0])
                report.append("     last  %s" % (hb.group(0) if hb else hbs[-1]))
        report.append("RESULT: %s" % ("PASS" if ok else "FAIL"))
        text = "\n".join(report)
        print(text)
        with open(os.path.join(a.out, "summary.txt"), "w") as f:
            f.write(text + "\n")
        return 0 if ok else 1
    finally:
        for d in devs:
            d.close()


if __name__ == "__main__":
    sys.exit(main())
