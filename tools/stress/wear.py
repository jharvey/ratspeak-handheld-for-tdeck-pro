#!/usr/bin/env python3
"""Flash wear estimate from the simulator's write counters (<data>/wear.txt).

The simulator counts every LittleFS file write (bytes per open..close), every rename /
remove / mkdir, and every NVS value that changed (ESP-IDF skips unchanged values). This
tool takes two runs:
  --idle  wear.txt of a device left idle (periodic writers: clock, logs, announces...);
          --idle-base = a copy of it taken after boot, so boot-time writes are left out
  --busy  wear.txt of a device in a ping-pong run, with --msgs = messages it sent + received
and works out the cost per message and per idle hour, then the erase cycles per day and
years to wear-out for a usage profile (--per-day messages, default 500).

Model (ESP32-S3, 16 MB flash, settings from the Arduino core's sdkconfig):
  LittleFS: 4 KB blocks, 2016 in the 0x7E0000 partition; a file of <= 512 B is stored inline
  in its directory's metadata; every change is one metadata commit, padded to the 128 B
  program size; a metadata block is erased when it fills (compaction), assume half of each
  block is free after a compaction (2 KB of commits per erase). Larger files are copied on
  write: one erase per 4 KB block written. Directories hop to fresh blocks every 512 erases
  (block_cycles), and the allocator walks all free blocks, so erases spread over the free
  blocks. NOR flash: 100,000 erase cycles per block.
  NVS: 0x5000 = 5 pages of 126 x 32-byte entries; a number takes 1 entry, a string / blob
  1 + ceil(n/32); a page is erased once its entries are used up, pages rotate; live
  entries are copied forward (write amplification 1 / (1 - live share)).
Rough by design (a factor of 2 either way): the answer that matters is decades vs. years.

  python tools/stress/wear.py --idle <dir>/wear.txt --busy <dir>/wear.txt --msgs 900
"""
import argparse
import math
import os
import sys

BLOCK = 4096
FS_BLOCKS = 0x7E0000 // BLOCK
INLINE_MAX = 512
PROG = 128
COMMIT_OVERHEAD = 40          # tags + CRC per commit (name, struct, mtime attr)
META_PER_ERASE = 2048
CYCLES = 100000
NVS_PAGES = 5
NVS_ENTRIES = 126


def load(path):
    rows, up = {}, 0
    for line in open(path, encoding="utf-8"):
        f = line.rstrip("\n").split("\t")
        if f[0] == "uptime_s":
            up = int(f[1])
            continue
        kind, key, ops, nbytes = f[0], f[1], int(f[2]), int(f[3])
        entries = int(f[4]) if len(f) > 4 else 0
        rows[(kind, key)] = (ops, nbytes, entries)
    return rows, up


def cost(kind, key, ops, nbytes, entries):
    """(LittleFS erases, NVS entries) for this many operations."""
    if kind == "nvs":
        return 0.0, float(entries)
    if kind == "meta":
        commits = ops * PROG                        # a rename / remove / mkdir is one small commit
        erases = commits / META_PER_ERASE
        if key.startswith("mkdir"):
            erases += 2 * ops                         # a new directory = a fresh metadata pair
        return erases, 0.0
    avg = nbytes / ops if ops else 0
    if avg <= INLINE_MAX:                             # inline: data rides in the commit
        per = math.ceil((avg + COMMIT_OVERHEAD) / PROG) * PROG
        return ops * per / META_PER_ERASE, 0.0
    per_data = math.ceil(avg / BLOCK)                 # copy-on-write data blocks + a commit
    return ops * (per_data + PROG / META_PER_ERASE), 0.0


def nvs_live_entries(nvs_dir):
    live = 0
    if not os.path.isdir(nvs_dir):
        return 0
    for name in os.listdir(nvs_dir):
        if not name.endswith(".nvs"):
            continue
        live += 1                                     # namespace entry
        for line in open(os.path.join(nvs_dir, name), encoding="utf-8"):
            f = line.rstrip("\n").split("\t")
            if len(f) < 3:
                continue
            t, n = int(f[1]), len(f[2]) // 2
            live += 1 + (n + 31) // 32 if t >= 8 else 1    # PT_STR = 8, PT_BLOB = 9
    return live


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--idle", required=True)
    ap.add_argument("--idle-base", help="earlier copy of the idle wear.txt (after boot): idle rates are "
                    "taken from the difference, so one-off boot writes do not count as periodic")
    ap.add_argument("--idle-seconds", type=float, help="idle run length (default: uptime in wear.txt)")
    ap.add_argument("--busy", required=True)
    ap.add_argument("--msgs", type=float, required=True, help="messages the busy device sent + received")
    ap.add_argument("--per-day", type=float, default=500, help="messages per day for the estimate")
    ap.add_argument("--free-share", type=float, default=0.15,
                    help="share of LittleFS blocks free (worst case: flash 85%% full of messages)")
    args = ap.parse_args()

    idle, idle_up = load(args.idle)
    if args.idle_seconds:
        idle_up = args.idle_seconds
    if args.idle_base:
        base, base_up = load(args.idle_base)
        idle = {k: tuple(a - b for a, b in zip(v, base.get(k, (0, 0, 0)))) for k, v in idle.items()}
        idle_up -= base_up
    busy, busy_up = load(args.busy)
    if not idle_up or not busy_up:
        sys.exit("uptime missing in wear.txt")
    keys = set(idle) | set(busy)
    per_hour, per_msg = {}, {}
    for k in keys:
        i = idle.get(k, (0, 0, 0))
        b = busy.get(k, (0, 0, 0))
        ih = tuple(x * 3600.0 / idle_up for x in i)
        per_hour[k] = ih
        extra = tuple(max(0.0, bb - x * busy_up / 3600.0) for bb, x in zip(b, ih))
        per_msg[k] = tuple(x / args.msgs for x in extra)

    day = {}
    for k in keys:
        d = tuple(h * 24 + m * args.per_day for h, m in zip(per_hour[k], per_msg[k]))
        if d[0] > 0:
            day[k] = (d, cost(k[0], k[1], *[x for x in d]))
    fs_erases = sum(c[0] for _, c in day.values())
    nvs_entries = sum(c[1] for _, c in day.values())

    live = nvs_live_entries(os.path.join(os.path.dirname(os.path.abspath(args.idle)), "nvs"))
    cap = NVS_PAGES * NVS_ENTRIES
    amp = 1.0 / max(0.05, 1.0 - live / float(cap))
    nvs_page_cycles = nvs_entries * amp / NVS_ENTRIES / NVS_PAGES

    print("Flash wear estimate: %g messages/day + 24 h running" % args.per_day)
    print("  idle run %.1f h, busy run %.1f h with %g messages" % (idle_up / 3600.0, busy_up / 3600.0, args.msgs))
    print()
    print("Top writers per day (ops/day, bytes/day, est. LittleFS erases or NVS entries per day):")
    ranked = sorted(day.items(), key=lambda kv: -(kv[1][1][0] * 1.0 + kv[1][1][1] / 40.0))
    for (kind, key), (d, c) in ranked[:25]:
        what = "%.1f erases" % c[0] if kind != "nvs" else "%.0f entries" % c[1]
        print("  %-5s %-48s %9.0f ops %10.0f B  %s" % (kind, key[:48], d[0], d[1], what))
    print()
    for label, share in (("typical (flash mostly free)", 0.9), ("worst case (flash %d%% full)" % round(100 - args.free_share * 100), args.free_share)):
        blocks = FS_BLOCKS * share
        per_block = fs_erases / blocks
        years = CYCLES / per_block / 365.0 if per_block else float("inf")
        print("LittleFS %-30s %8.0f erases/day over %4.0f blocks = %.2f cycles/block/day -> %s years"
              % (label + ":", fs_erases, blocks, per_block, "%.0f" % years if years < 1e6 else ">1e6"))
    years = CYCLES / nvs_page_cycles / 365.0 if nvs_page_cycles else float("inf")
    print("NVS: %.0f entries/day, %d of %d entries live (x%.2f copying) = %.2f cycles/page/day -> %s years"
          % (nvs_entries, live, cap, amp, nvs_page_cycles, "%.0f" % years if years < 1e6 else ">1e6"))


if __name__ == "__main__":
    main()
