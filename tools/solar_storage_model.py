#!/usr/bin/env python3
"""Storage + solar energy budget for a radio that is switched off most of the time.

    python tools/solar_storage_model.py                 (prints the tables, Markdown)
    python tools/solar_storage_model.py --off-ma 0.25   (once the draw while off is measured)
    python tools/solar_storage_model.py --use-ma 60 --usable 1200

The case: the radio is charged, switched off (Settings > Power > Switch off) and carried for
months; then it is used about an hour a day and switched off again, with a small USB solar
panel to keep it going.

A MODEL, NOT A MEASUREMENT. What is measured and what is assumed:
  usable battery   ~1150 mAh   battery run 1 (2026-11-24): ~90 mA for ~13 h down to the 3.40 V switch-off
  draw while on    ~70 mA      bench: awake 240 MHz + LoRa ~48 mA, GPS ~19 mA more, no WiFi
  start-up         ~1.5 mAh    about a minute awake before it is usable (assumed)
  draw while off   0.1 - 1 mA  NOT MEASURED (deep sleep, all rails off; SD card and gauge stay powered)
  self-discharge   ~2 % / month of the cell itself (typical Li-ion, assumed)
  solar panel      gives ~60 % of its rating in direct sun, ~10 % behind glass or in haze (assumed)
  charger          ~88 % from USB to battery, at most 1 A into the battery (assumed)
  "sun hours"      hours of full direct sun on the panel per day (1 = a bright window sill or a
                   winter day, 3 = a fair day outside, 5 = a clear summer day)
"""
import argparse


def charge_mah_per_sun_hour(panel_w, derate=0.60, eff=0.88, vbat=3.9, max_ma=1000.0):
    return min(panel_w * derate * eff / vbat * 1000.0, max_ma)


def storage_days(usable, off_ma, selfdis_pct_month):
    """Days switched off, from full, until the battery is empty (no use, no sun)."""
    per_day = off_ma * 24.0 + usable * selfdis_pct_month / 100.0 / 30.0
    return usable / per_day


def daily_use_mah(use_h, use_ma, boot_mah, off_ma):
    return use_h * use_ma + boot_mah + (24.0 - use_h) * off_ma


def simulate(usable, start_frac, use_h, use_ma, boot_mah, off_ma, selfdis, panel_w, sun_h, cloudy_run=0,
             days=365):
    """Day by day. cloudy_run: every 10 days, this many days in a row give 10 % of the sun.
    Returns (first day it was empty or None, lowest charge in % of usable)."""
    q = usable * start_frac
    low = q
    per_h = charge_mah_per_sun_hour(panel_w) if panel_w else 0.0
    for d in range(days):
        cloudy = cloudy_run and (d % 10) < cloudy_run
        q += per_h * sun_h * (0.1 if cloudy else 1.0)
        q = min(q, usable)
        q -= daily_use_mah(use_h, use_ma, boot_mah, off_ma) + usable * selfdis / 100.0 / 30.0
        low = min(low, q)
        if q <= 0:
            return d + 1, 0.0
    return None, 100.0 * low / usable


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--usable", type=float, default=1150.0, help="usable battery, mAh")
    ap.add_argument("--use-ma", type=float, default=70.0, help="draw while on, mA")
    ap.add_argument("--boot-mah", type=float, default=1.5)
    ap.add_argument("--off-ma", type=float, default=None, help="measured draw while off, mA")
    ap.add_argument("--selfdis", type=float, default=2.0, help="cell self-discharge, %% per month")
    a = ap.parse_args()
    offs = [a.off_ma] if a.off_ma else [0.1, 0.3, 1.0]

    print("## 1. Stored, switched off, no charging: time until empty\n")
    print("| Draw while off | Full battery lasts | Half-full battery lasts |")
    print("|---|---|---|")
    for o in offs:
        d = storage_days(a.usable, o, a.selfdis)
        print("| %.2f mA | %.0f days (%.1f months) | %.0f days |" % (o, d, d / 30.4, d / 2))

    print("\n## 2. In use, no sun: days of use from a full battery\n")
    print("| Use per day | " + " | ".join("off %.2f mA" % o for o in offs) + " |")
    print("|---|" + "---|" * len(offs))
    for h in (0.5, 1.0, 2.0):
        row = []
        for o in offs:
            row.append("%.0f days" % (a.usable / (daily_use_mah(h, a.use_ma, a.boot_mah, o)
                                                   + a.usable * a.selfdis / 100 / 30)))
        print("| %.1f h (%.0f mAh) | " % (h, h * a.use_ma + a.boot_mah) + " | ".join(row) + " |")

    print("\n## 3. Sun needed to stay level (1 h of use a day)\n")
    print("| Panel (rated) | Charge per sun hour | Sun hours needed per day | Behind glass / haze |")
    print("|---|---|---|---|")
    o = offs[len(offs) // 2]
    need = daily_use_mah(1.0, a.use_ma, a.boot_mah, o) + a.usable * a.selfdis / 100 / 30
    for w in (1, 2, 5, 10):
        c = charge_mah_per_sun_hour(w)
        print("| %d W | %.0f mAh | %.1f h | %.1f h |" % (w, c, need / c, need / (c / 6.0)))

    print("\n## 4. A year of 1 h a day with a panel: does it run out?\n")
    print("(start at 60 % charge; every 10 days some days in a row with almost no sun)\n")
    print("| Panel | Sun hours / day | No bad days | 3 bad days in 10 | 6 bad days in 10 |")
    print("|---|---|---|---|---|")
    for w in (1, 2, 5):
        for sun in (1, 3, 5):
            cells = []
            for bad in (0, 3, 6):
                dead, low = simulate(a.usable, 0.6, 1.0, a.use_ma, a.boot_mah, o, a.selfdis, w, sun, bad)
                cells.append("empty on day %d" % dead if dead else "OK (lowest %.0f %%)" % low)
            print("| %d W | %d | " % (w, sun) + " | ".join(cells) + " |")
    print("\nAssumptions: usable %.0f mAh, on %.0f mA, off %s mA, self-discharge %.0f %%/month."
          % (a.usable, a.use_ma, "/".join("%.2f" % x for x in offs), a.selfdis))


if __name__ == "__main__":
    main()
