# INA226 battery-current logger (board floor rig)

The T-Deck Pro's BQ27220 gauge cannot resolve currents below a few mA (one unit reads
0 mA while its battery voltage falls), so the ~1–5 mA board floor (guide §6.6) needs a
meter in series with the battery. This folder has both halves of that rig:

- `src/main.cpp`: firmware for a **separate** small ESP32 (default ESP32-C3 SuperMini)
  that reads an INA226 over I2C and streams one summary line per period over USB.
- `log.py`: the PC logger. It writes a CSV and prints window and overall averages, charge,
  wake bursts, and the battery life that average gives.

Nothing here runs on the T-Deck itself.

## Parts (~$15)

- INA226 breakout with a 0.1 Ω (R100) shunt (CJMCU-226 or similar). 2.5 µV/LSB →
  **25 µA steps**, range ±819 mA (enough for awake + LoRa TX at full power).
- ESP32-C3 SuperMini (or any ESP32 devkit, env `esp32`).
- One JST-PH 2.0 mm 2-pin extension lead (male/female), cut in the red wire.

## Wiring

```
 T-Deck battery  red ──► INA226 IN+        INA226 IN− ──► red ──► T-Deck board connector
 T-Deck battery  black ─────────────────────────────────► black (uncut)
 INA226 VBUS ── IN− (some boards link it already)   INA226 GND ── battery black
 INA226 VCC ── C3 3V3   GND ── C3 GND   SDA ── C3 GPIO 8   SCL ── C3 GPIO 9
```

- IN+ on the **battery** side: discharge reads positive, charging reads negative.
- The INA226 GND must be tied to the battery black wire; the C3 is powered by the PC.
- Unplug the T-Deck's USB for a true floor reading. With USB attached, the charger feeds
  the board and the battery lead shows charge current, unless you run `bench <s>` on the
  T-Deck (charger input off, guide §11).
- Address 0x40–0x4F is auto-detected (A0/A1 straps).

## Build and flash

```
cd tools/inalogger
pio run                                  # env c3 (default) or: pio run -e esp32
pio run -t upload --upload-port COMx
```

## Use

```
python log.py COMx --zero                # once: battery unplugged (no current), stores the offset
python log.py COMx --out floor.csv       # log until Ctrl+C, summary every 60 s
python log.py COMx --out floor.csv --minutes 60 --cmd "p 50"
python log.py --summary floor.csv        # analyse later
python log.py --selftest
```

Typical floor run: on the T-Deck run `tools/sleepcheck` `floor d 8 300` (ESP32 in deep
sleep, rails off), unplug its USB, log for 40 min. For the real firmware, log one Balanced
or Saver hour, then compare with the energy monitor (S.4.9).

Summary fields: average and charge since start, the life estimate (`--capacity`, default
1450 mAh), wake bursts (rows at or above `--burst-ma`, default 2 mA): count, mean length,
peak, share of the charge, median spacing, and the average between bursts.

## Logger console (115200)

| Command | Meaning |
|---|---|
| `s` | status (address, shunt, offset, timing, charge) |
| `c` | clear charge and time |
| `p <ms>` | report period, 5–60000 (default 100) |
| `avg <0..7>` | INA averaging ×1, 4, 16, 64, 128, 256, 512, 1024 (default ×1) |
| `ct <0..7>` / `bt <0..7>` | shunt / bus conversion time 140 µs … 8.2 ms (default 1.1 ms / 140 µs) |
| `r <mΩ>` | shunt value (default 100) |
| `z [n]` / `z0` | zero offset over n conversions (no current must flow) / clear it |
| `q` / `g` | stop / resume data lines |

Settings live in NVS. The defaults convert every ~1.24 ms with the shunt sampled ~89 % of
the time, so millisecond wake bursts are caught. Each period's charge uses the whole
elapsed time.

Data line: `D,<ms>,<n>,<avg_uA>,<min_uA>,<max_uA>,<bus_mV>,<uAh>`. Lines starting with `#`
are replies.

## Status

Firmware builds (c3, esp32) and `log.py --selftest` passes. **Not yet run on hardware**:
no INA226 or C3 is on the bench yet.
