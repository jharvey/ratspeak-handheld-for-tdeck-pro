#!/usr/bin/env python3
"""PC side of the INA226 battery-current logger (tools/inalogger, guide section 6.6).

Reads the "D,..." lines from the logger firmware, writes them to a CSV with a PC
timestamp, and prints a summary every --every seconds: average current over the last
window and since the start, charge, wake bursts, and the battery life that average gives.

    python log.py COM7 --out floor.csv                    log until Ctrl+C
    python log.py COM7 --out floor.csv --minutes 60       log for one hour
    python log.py COM7 --cmd "p 50" --cmd c               send logger commands first
    python log.py COM7 --zero                             store the zero offset (no current!)
    python log.py --summary floor.csv                     analyse a CSV written earlier
    python log.py --selftest                              check the analysis on fake data

Positive current = the battery is discharging into the board (IN+ battery, IN- board).
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import statistics
import sys
import time
from dataclasses import dataclass, field

CSV_FIELDS = ["pc_time", "dev_ms", "n", "avg_uA", "min_uA", "max_uA", "bus_mV", "uAh"]


@dataclass
class Burst:
    start_ms: int
    end_ms: int = 0
    peak_uA: float = 0.0
    charge_uAs: float = 0.0   # uA * s


@dataclass
class Analyzer:
    """Sums D lines into windows, totals and bursts. Times are device milliseconds."""

    burst_uA: float = 2000.0
    total_s: float = 0.0
    total_uAs: float = 0.0
    win_s: float = 0.0
    win_uAs: float = 0.0
    win_min: float = float("inf")
    win_max: float = float("-inf")
    rows: int = 0
    last_ms: int | None = None
    last_bus: float = 0.0
    bursts: list = field(default_factory=list)
    _open: Burst | None = None
    _periods: list = field(default_factory=list)

    def feed(self, dev_ms: int, avg_uA: float, min_uA: float, max_uA: float, bus_mV: float) -> None:
        if self.last_ms is None or dev_ms <= self.last_ms:
            # First line, or the logger restarted: assume one typical period.
            dt_s = (statistics.median(self._periods) if self._periods else 0.1)
            if self.last_ms is not None and dev_ms <= self.last_ms:
                self._close_burst(self.last_ms)
        else:
            dt_s = (dev_ms - self.last_ms) / 1000.0
            if len(self._periods) < 50:
                self._periods.append(dt_s)
        self.last_ms = dev_ms
        self.last_bus = bus_mV
        self.rows += 1
        self.total_s += dt_s
        self.total_uAs += avg_uA * dt_s
        self.win_s += dt_s
        self.win_uAs += avg_uA * dt_s
        self.win_min = min(self.win_min, min_uA)
        self.win_max = max(self.win_max, max_uA)
        if avg_uA >= self.burst_uA:
            if self._open is None:
                self._open = Burst(start_ms=dev_ms - int(dt_s * 1000))
            self._open.peak_uA = max(self._open.peak_uA, max_uA)
            self._open.charge_uAs += avg_uA * dt_s
            self._open.end_ms = dev_ms
        else:
            self._close_burst(dev_ms)

    def _close_burst(self, _ms: int) -> None:
        if self._open is not None:
            self.bursts.append(self._open)
            self._open = None

    def finish(self) -> None:
        self._close_burst(self.last_ms or 0)

    def take_window(self):
        w = (self.win_s, self.win_uAs / self.win_s if self.win_s else 0.0, self.win_min, self.win_max)
        self.win_s = 0.0
        self.win_uAs = 0.0
        self.win_min = float("inf")
        self.win_max = float("-inf")
        return w

    @property
    def avg_uA(self) -> float:
        return self.total_uAs / self.total_s if self.total_s else 0.0

    def report(self, capacity_mAh: float) -> str:
        bursts = self.bursts + ([self._open] if self._open else [])
        avg_mA = self.avg_uA / 1000.0
        lines = [
            f"duration {self.total_s / 60:.1f} min, {self.rows} rows",
            f"average {avg_mA:.3f} mA, charge {self.total_uAs / 3.6e6:.3f} mAh",
        ]
        if avg_mA > 0:
            hours = capacity_mAh / avg_mA
            lines.append(f"battery life at this average: {hours:.0f} h = {hours / 24:.1f} days ({capacity_mAh:.0f} mAh)")
        if bursts:
            burst_uAs = sum(b.charge_uAs for b in bursts)
            durs = [max(b.end_ms - b.start_ms, 0) for b in bursts]
            starts = [b.start_ms for b in bursts]
            gaps = [b - a for a, b in zip(starts, starts[1:])]
            share = 100.0 * burst_uAs / self.total_uAs if self.total_uAs else 0.0
            lines.append(
                f"bursts >= {self.burst_uA / 1000:.1f} mA: {len(bursts)}, mean {statistics.mean(durs):.0f} ms, "
                f"peak {max(b.peak_uA for b in bursts) / 1000:.1f} mA, {share:.1f}% of the charge"
                + (f", every {statistics.median(gaps) / 1000:.1f} s (median)" if gaps else ""))
            base_s = self.total_s - sum(durs) / 1000.0
            if base_s > 0:
                lines.append(f"between bursts: {(self.total_uAs - burst_uAs) / base_s / 1000:.3f} mA")
        else:
            lines.append(f"no bursts >= {self.burst_uA / 1000:.1f} mA")
        return "\n".join(lines)


def parse_d(line: str):
    """'D,ms,n,avg,min,max,bus,uAh' -> tuple, or None."""
    if not line.startswith("D,"):
        return None
    parts = line.split(",")
    if len(parts) != 8:
        return None
    try:
        return (int(parts[1]), int(parts[2]), float(parts[3]), float(parts[4]), float(parts[5]),
                float(parts[6]), float(parts[7]))
    except ValueError:
        return None


def open_port(port: str):
    import serial  # pyserial

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    # Set the lines before opening: toggling DTR/RTS afterwards resets ESP32 boards.
    s.dtr = True
    s.rts = False
    s.open()
    return s


def run_live(args) -> int:
    s = open_port(args.port)
    time.sleep(0.3)
    cmds = list(args.cmd or [])
    if args.zero:
        cmds.append(f"z {args.zero_n}")
    for c in cmds:
        s.write((c + "\r\n").encode())
        time.sleep(0.15)
    s.write(b"s\r\n")

    out = None
    writer = None
    if args.out:
        out = open(args.out, "a", newline="", encoding="utf-8")
        writer = csv.writer(out)
        if out.tell() == 0:
            writer.writerow(CSV_FIELDS)
    an = Analyzer(burst_uA=args.burst_ma * 1000.0)
    start = time.monotonic()
    next_sum = start + args.every
    end = start + args.minutes * 60 if args.minutes else None
    zero_wait = args.zero
    buf = b""
    try:
        while True:
            buf += s.read(4096)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode(errors="replace").strip()
                d = parse_d(line)
                if d is None:
                    if line:
                        print(line, flush=True)
                        if zero_wait and line.startswith("# OK offset_raw"):
                            zero_wait = False
                    continue
                dev_ms, n, avg, mn, mx, bus, uah = d
                an.feed(dev_ms, avg, mn, mx, bus)
                if writer:
                    writer.writerow([dt.datetime.now().isoformat(timespec="milliseconds"), dev_ms, n,
                                     avg, mn, mx, bus, uah])
            now = time.monotonic()
            if args.zero and not zero_wait:
                return 0
            if now >= next_sum:
                next_sum += args.every
                w_s, w_avg, w_min, w_max = an.take_window()
                if w_s:
                    print(f"[{(now - start) / 60:6.1f} min] last {w_s:.0f} s: {w_avg / 1000:.3f} mA "
                          f"(min {w_min / 1000:.3f}, max {w_max / 1000:.1f})  overall {an.avg_uA / 1000:.3f} mA  "
                          f"{an.total_uAs / 3.6e6:.3f} mAh  bursts {len(an.bursts)}  bus {an.last_bus / 1000:.3f} V",
                          flush=True)
                else:
                    print(f"[{(now - start) / 60:6.1f} min] no data", flush=True)
                if out:
                    out.flush()
            if end and now >= end:
                break
    except KeyboardInterrupt:
        pass
    finally:
        s.close()
        if out:
            out.close()
    an.finish()
    print("---- summary")
    print(an.report(args.capacity))
    return 0


def run_summary(path: str, args) -> int:
    an = Analyzer(burst_uA=args.burst_ma * 1000.0)
    with open(path, newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            try:
                an.feed(int(row["dev_ms"]), float(row["avg_uA"]), float(row["min_uA"]), float(row["max_uA"]),
                        float(row["bus_mV"]))
            except (KeyError, ValueError):
                continue
    an.finish()
    print(an.report(args.capacity))
    return 0


def selftest() -> int:
    """Fake floor: 3 mA base, a 50 ms 40 mA wake every 1 s, 100 ms rows for 10 min."""
    an = Analyzer(burst_uA=10000.0)
    ms = 0
    for i in range(6000):
        ms += 100
        in_burst = (i % 10) == 0
        avg = 3000.0 + (20000.0 if in_burst else 0.0)   # 50 ms of 40 mA averaged into a 100 ms row
        an.feed(ms, avg, 2900.0, 43000.0 if in_burst else 3100.0, 4100.0)
    an.finish()
    expect = 3000.0 + 20000.0 / 10
    ok = abs(an.avg_uA - expect) < 1.0 and len(an.bursts) == 600 and abs(an.total_s - 600.0) < 0.01
    assert parse_d("D,1000,45,3012.5,2900.0,3100.0,4100.0,0.084") == (1000, 45, 3012.5, 2900.0, 3100.0, 4100.0, 0.084)
    assert parse_d("# INA226 at 0x40") is None and parse_d("D,1,2") is None
    print(an.report(1450.0))
    print("SELFTEST", "PASS" if ok else f"FAIL avg={an.avg_uA} bursts={len(an.bursts)} s={an.total_s}")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="logger serial port, e.g. COM7")
    ap.add_argument("--out", help="append rows to this CSV")
    ap.add_argument("--minutes", type=float, default=0, help="stop after this long (0 = until Ctrl+C)")
    ap.add_argument("--every", type=float, default=60, help="summary interval in seconds")
    ap.add_argument("--burst-ma", type=float, default=2.0,
                    help="a row at or above this average counts as a wake burst (mA, default 2)")
    ap.add_argument("--capacity", type=float, default=1450, help="battery capacity for the life estimate (mAh)")
    ap.add_argument("--cmd", action="append", help="logger command to send first (repeatable)")
    ap.add_argument("--zero", action="store_true", help="store the zero offset, then exit (no current must flow)")
    ap.add_argument("--zero-n", type=int, default=2000, help="conversions to average for --zero")
    ap.add_argument("--summary", metavar="CSV", help="analyse a CSV written earlier")
    ap.add_argument("--selftest", action="store_true", help="check the analysis on synthetic data")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.summary:
        return run_summary(args.summary, args)
    if not args.port:
        ap.error("port required (or --summary / --selftest)")
    return run_live(args)


if __name__ == "__main__":
    sys.exit(main())
