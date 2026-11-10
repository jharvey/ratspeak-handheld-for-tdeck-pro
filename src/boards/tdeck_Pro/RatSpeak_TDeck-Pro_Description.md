# RatSpeak T-Deck Pro — Guide

**Audience:** operators holding the device, and developers working on the firmware,
the desktop simulator or the hardware bring-up.  
**Input priority:** **keyboard first**; touch is secondary.  
**Radio priority:** **LoRa mesh / LXMF first**. WiFi (station) is optional.  
**Stable hotkeys:** **I**, **C**, **M**, **P** (do not reassign lightly).

Related code: `StandaloneApp.cpp` (the whole app), `hal/Keyboard.cpp`, `power/`
(energy modes, light sleep), `remote/ScreenMirror.*` (USB screen mirror),
`security/SecretVault.*`, `sim/` (desktop simulator), `tools/gpscheck/` (GPS bench check).

**Figures:** `images/screen-*.png`, the RatSpeak UI on the LilyGo T-Deck Pro bezel and
keyboard. Selection = bold outline; MENU underline under **M** only; bottom underlines
under **C / P / I**.

Status words used below: **Works** (in the firmware and checked on hardware),
**Built** (in the firmware, checked in the simulator or partly on hardware),
**Planned** (designed, not written yet).

---

## 1. How to

### 1.1 Send a message

1. Wake if idle → any key.  
2. **I** → Inbox *(skip if already on Inbox or in the right thread)*.  
3. Select the contact by **touch** or **number**, then **Enter**.  
4. Type the message (**Alt** for case). Nav underlines go off while typing.  
5. **Enter** to send.  
6. Optional **^** back to the list.

```text
I  →  select  →  type  →  Enter  →  ^ (optional)
```

### 1.2 Add / name a contact

**Discover first (two radios):** on each device, Home → **Announce ID**, then open
**Peers** (**P**) and confirm the other appears.

1. **C** → Contacts *(or **P** Peers if they are not saved yet)*.  
2. Select the device (touch, **V**/**G**, or digit).  
3. **Enter** to open name edit, type the name (**Alt** for case), **Enter** to save.

Row **0** is always **this** device (**MyID**). Rename it the same way to set the
local display name (default **MyRatSpeakPro**).

### 1.3 First-boot setup

| Step | What | How |
|------|------|-----|
| 1 | **Time** | **M → S → 1** (Time & Location) → Set RTC (**S.1.1**), Time zone (**S.1.2**) |
| 2 | **Theme** | **M → S → 3** (Screen & I/O) → **1** Theme: **White paper** / **Black ink** |
| 3 | **Power mode** | **M → S → 4** (Power) → Full / Balanced / Saver / Custom (§6) |
| 4 | **Name this device** | **C** → row **0 MyID** → type name → **Enter** |
| 5 | **WiFi** (optional) | **M → S → 2** — scan, join, saved networks, auto-connect |
| 6 | **Meet the other radio** | Announce ID on both → **P** → name contact (§1.2) → test send (§1.1) |

A **second LoRa RatSpeak device** in range is needed to check messaging.

### 1.4 Install on a T-Deck Pro

1. Plug in USB. Build env **`tdeck_pro_standalone`** in PlatformIO.  
2. Upload. On these boards the esptool "stub" often fails ("Unable to verify flash
   chip connection"); the ROM loader works and needs **no buttons**:

   ```bash
   ~/.platformio/penv/Scripts/python.exe ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port COM8 --baud 115200 --before default_reset --after hard_reset --no-stub write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x10000 .pio/build/tdeck_pro_standalone/firmware.bin
   ```

   If a board will not enter the loader by itself: hold the **top** button, press and
   release the **bottom** button, then release the top one.  
3. Serial console: **115200** baud (§11).  
4. Continue with §1.3.

**Bench build without LoRa** (antenna disconnected — never transmit without one):
`RS_NO_RADIO=1` keeps the SX1262 unpowered (`LORA_EN` low), so it can never transmit.

```bash
PLATFORMIO_BUILD_FLAGS="-DRS_NO_RADIO=1" PLATFORMIO_BUILD_DIR=".pio/build_noradio" pio run -e tdeck_pro_standalone
```

### 1.5 See the device's screen on a PC (USB remote)

`sim\run-live.bat COM8` opens the desktop simulator window in **live mode**: it shows
the real device's e-ink contents and sends the PC keyboard / mouse to the device (§10.3).

### 1.6 Make the battery last longer

**M → S → 4** (Power) and pick **Balanced** (a day-to-day choice) or **Saver** (longest
runtime). Keys and incoming messages stay immediate in every mode; see §6 for what
changes. While USB power is plugged in the device runs **Full** anyway (row 5).

---

## 2. Physical controls

| Key | Browse mode | Entry mode |
|-----|-------------|------------|
| **I** | Inbox | types letter |
| **C** | Contacts | types letter |
| **P** | Peers | types letter |
| **M** | MENU overlay (**H**ome · **S**ettings · **C**ontacts · **P**eers · **I**nbox) | does **not** open the menu |
| **H** | Home | — |
| **G** / **V** | Up / down | — |
| **1**–**9**, **0** | Lists: select row. Settings: select **and** activate row 1–9, **0** = row 10 | digits (numeric fields) |
| **Enter** | Activate / open | Send / save |
| **Alt** | — | Case shift while typing |
| **SYM** | — | Digits / symbols |
| **Shift** (tap alone) **= ^** | Back | Back / cancel |
| **MIC** | Full e-ink refresh (clears ghosting) | — |

There is **no S hotkey**: Settings is **M → S**. Touch is secondary (MENU tab, bottom
tabs, list rows, settings rows). Idle wake is **key only**.

The keyboard boots in caps lock (letters type upper case; **Alt+letter** flips case).
The TCA8418 holds only 10 events, so keys are also read while the panel is refreshing
and queued (48 events) — nothing typed during a refresh is lost.

---

## 3. Navigation model

**Browse:** hotkeys and underlines active; **G/V** move; digits select (Settings rows
activate).  
**Entry:** a text field or the compose line is focused — **M/C/P/I** do not navigate.  
**MENU overlay:** **M** opens a column under MENU; top bar stays visible.  
**Settings:** sub-pages form a stack; **^** returns to the exact row you came from.
Row text is `n : Kind : Name` with the value underneath. Kinds: **Menu** (opens a page),
**Toggle**, **Pick** (Enter steps to the next value), **Entry** (type a value),
**Action**, **Info**.  
**Scroll:** **v more** / **^ more** appear only when more rows exist.

---

## 4. Screen map & figures

### 4.1 Idle splash
![Idle splash](images/screen-idle.png)

Logo, date/time, battery %, plug status, unread line, uptime, device name. Updates at
most once a minute. **Press a key to wake** (touch does not wake).

### 4.2 Home
![Home screen](images/screen-home.png)

Status tiles, **Announce ID**, device name. Bottom **Contact | Peers | Inbox**.

### 4.3 Home with MENU overlay
![Home with MENU overlay](images/screen-home-menu.png)

### 4.4 Contacts
![Contacts](images/screen-contacts.png)

**0: MyID** = this radio.

### 4.5 Inbox (list)
![Inbox list](images/screen-inbox.png)

Per-row **unrd ##** when unread.

### 4.6 Inbox thread (typing)
![Inbox compose](images/screen-inbox-compose.png)

### 4.7 Settings root
![Settings](images/screen-settings.png)

### 4.8 Settings S.3 Screen & I/O
![Settings S.3](images/screen-settings-s3.png)

### 4.9 Settings S.4 Power and S.4.4 Custom
![Settings S.4 Power](images/screen-settings-power.png)
![Settings S.4.4 Custom](images/screen-settings-power-custom.png)

Simulator screenshots (`sim/scripts/power.txt`): Saver selected; Custom with CPU set to 240 MHz.

---

## 5. Settings tree (`S.*`)

```
S     Settings                      ← M then S
├── S.1   Time & Location
│   ├── S.1.1  Set RTC              hour, minute, month, day, year → Apply
│   ├── S.1.2  Time zone            18 zones (EST … UTC+9)
│   ├── S.1.3  Enable GPS           shows heard/in-view satellites, "backup cell low", or "resting"
│   ├── S.1.4  Manual location      latitude / longitude / clear
│   ├── S.1.5  Sync GPS now         needs a fix
│   └── S.1.6  Auto GPS sync        boot + daily
├── S.2   WiFi                      status, IP
│   ├── S.2.3  Scan networks        pick → saved: join; new locked: type password
│   │   └── …  Join WiFi            Password · Auto-connect · Join (saves on success)
│   ├── S.2.4  Saved networks       up to 8; passwords sealed (§12)
│   │   └── …  Saved network        Connect now · Auto-connect · Password · Forget
│   └── S.2.5  Disconnect           radio off, auto-connect paused until a manual join
├── S.3   Screen & I/O
│   ├── S.3.1  Theme                White paper / Black ink
│   ├── S.3.2  Idle splash          Off / On (no password) / On + password (planned) / timeout
│   └── S.3.3  Key backlight        (a power mode may hold it off)
├── S.4   Power                     energy modes (§6)
│   ├── S.4.1  Full power
│   ├── S.4.2  Balanced
│   ├── S.4.3  Saver
│   ├── S.4.4  Custom               → S.4.4.1–8 knobs (§6.2)
│   ├── S.4.5  Full power on USB    ON (default) / OFF
│   ├── S.4.6  Now                  CPU MHz and % of time asleep
│   └── S.4.7  Battery draw         mA and estimated time left (gauge)
├── S.5   Battery                   volts, %, current, charger status, capacity
│   ├── S.5.8  Battery log          internal RAM ring / SD CSV, dump to USB
│   └── S.5.10 Reset calibration
├── S.6   Identity                  destination, display name, LXMF address, Announce
├── S.7   LoRa                      915 MHz; link preset (Long Fast / Long Slow / Medium / Fast Short)
├── S.8   Alerts                    beep, vibrate, volume, test
├── S.9   Diagnostics               paths/links, LXMF queue, peers, uptime
└── S.10  Storage  (key 0)          heap, PSRAM, flash, messages, SD card (+ S.10.5 SD), secrets key
```

---

## 6. Power & energy modes (S.4) — Built (light sleep off for now)

> **Status 2026-11-09:** light sleep resets this board. On COM8, on battery with no USB,
> every first light sleep ended in an immediate watchdog reset (reset reason 7), so the
> device boot-looped every ~16 s. The same happens with a PC attached. Until it is
> solved, light sleep is compiled out (`RS_LIGHT_SLEEP` 0): the modes still apply CPU
> clock, LoRa, GPS schedule, WiFi and key-light, and the sleep knobs are kept but have no
> effect. Build with `-DRS_LIGHT_SLEEP=1` to test sleep again.

The T-Deck Pro's largest steady loads are the ESP32-S3 itself (awake at 240 MHz), the
GPS receiver, WiFi and the key backlight. The e-ink panel costs nothing between
refreshes, and the LoRa receiver is small. The energy modes trade background
responsiveness for runtime. **Keys and incoming LoRa messages are immediate in every
mode**, because they wake the processor directly.

### 6.1 The four modes

| | **Full power** | **Balanced** | **Saver** | **Custom** |
|---|---|---|---|---|
| CPU clock (awake) | 240 MHz | 160 MHz | 80 MHz | your choice |
| Light sleep between wake-ups | never | wake every **1 s** | wake every **60 s** (on the minute) | your choice |
| Stay awake after a key | — | 15 s | 5 s | your choice |
| LoRa receiver | on | on | on | on / off |
| GPS | always on | every 15 min | every hour | your choice |
| WiFi | allowed | allowed | **off** | allowed / off |
| Key backlight | allowed | allowed | **off** | allowed / off |
| Feels like | today's behaviour | same, minus constant GPS | clock, battery and GPS update slowly | — |

**Full power on USB** (S.4.5, default ON): whenever USB power is present the device
runs **Full**, whatever mode is chosen, and goes back to the chosen mode when unplugged.
The Power row on the Settings root then reads e.g. `Saver (USB: Full)`.

Modes are stored in NVS namespace `rs_power` and survive reboots.

### 6.2 Custom knobs (S.4.4)

Choosing **Custom** the first time copies the mode you were in, then each row changes
one knob (Enter steps to the next value):

| Row | Knob | Values |
|---|---|---|
| 1 | CPU speed | 240 · 160 · 80 MHz |
| 2 | Sleep, wake every | never · 1 s · 5 s · 10 s · 30 s · 1 min · 5 min |
| 3 | Awake after key | 3 · 5 · 10 · 15 · 30 · 60 s |
| 4 | LoRa radio | ON (messages) / OFF (no messages in or out — the radio chip sleeps) |
| 5 | GPS | Always on · Every 15 min · Every hour · Off |
| 6 | WiFi | allowed / OFF |
| 7 | Key backlight | allowed / OFF |
| 8 | Reset to Balanced | |

Example: GPS off but snappy UI → Custom, CPU 240, sleep "never", GPS Off.

### 6.3 How it works underneath

- **CPU clock:** `setCpuFrequencyMhz()`. 80 MHz is the lowest clock that keeps the PLL,
  USB console and peripherals running normally.
- **Light sleep** (`power/LightSleep.cpp`): the CPU clocks stop but RAM, PSRAM and every
  pin level are kept, so the e-ink image stays, the LoRa radio keeps listening and
  nothing re-initialises. Wake-up takes about a millisecond and the program carries on.
  Wake sources:
  - **timer** — the mode's wake interval (Saver lands on the minute so the clock stays right);
  - **key** — TCA8418 INT line (GPIO 15, low). The chip's interrupt is acknowledged before
    each sleep so the line is high again;
  - **touch** — CST328 INT (GPIO 12, low), not while the idle splash shows;
  - **LoRa packet** — SX1262 DIO1 (GPIO 5, high = RX done). After the wake the radio's
    edge interrupt is re-armed and the packet is read at once.
- **When the CPU stays awake anyway** (the "blocker", shown by console `pw`): within the
  stay-awake time after a key; the first 15 s after boot; a redraw, queued key or LoRa
  packet pending; LXMF messages waiting to be sent; LoRa transmitting; **WiFi on**;
  **GPS powered** (its serial data would be lost while asleep); screen mirror active;
  **a USB host attached**. On this ESP32-S3 / Arduino 2.x build, light sleep with the
  USB-Serial-JTAG console connected to a PC resets the chip (watchdog reset, reason 7,
  seen on COM8), so the device never sleeps while plugged into a PC. With a plain USB
  charger the "Full power on USB" rule normally keeps it awake anyway.
- **LoRa off:** the interface stops (queued frames are dropped) and the SX1262 goes to
  warm sleep (~1 µA); turning it on restarts receive.
- **GPS schedule:** a "visit" powers the receiver (GPS_EN, GPIO 39) until a fix has held
  for 20 s (which also syncs the clock if Auto GPS sync is on) or 3 minutes pass, then
  powers it off until the next interval. The receiver's backup cell keeps satellite
  data, so later visits are usually hot starts of a few seconds. S.1.3 shows
  `resting (Every hour)` between visits. Switching modes starts a visit at once.
- **WiFi off:** the radio is turned off and auto-connect / scans are refused with a
  toast ("WiFi off in Saver mode"). Allowed again → auto-connect resumes in 2 s.
- **Key backlight:** lit only if S.3.3 is ON **and** the mode allows it.
- **Battery estimate:** S.4.7 shows the gauge's average current and its TimeToEmpty
  (BQ27220 register 0x16).

### 6.4 Measuring

- Console `pw`: mode, knobs, percent of time asleep, wake counts by cause, and what is
  keeping the CPU awake right now.
- Console `bench <s>` (max 1800): turns the charger's input off (BQ25896 EN_HIZ) so the
  board runs from its battery while USB stays connected for the console; the gauge then
  shows the real draw. It prints a `[BENCH]` line every second and turns the charger back
  on when time is up or the battery drops below 3.55 V. Refused below 3.70 V or with no
  battery. Light sleep is still blocked by the USB host, so this measures awake draw.
  `pset <knob> <value>` changes one Custom knob from the console (`pset` alone lists them).
- Light-sleep draw: console `blog 1` (or S.5.8 Internal log), unplug USB for 10+
  minutes, plug back in, then `pw` (sleep statistics; uptime shows there was no reset)
  and `d` (the log, one sample per wake-up).

Measured with `bench` on COM8 (battery 4.14–4.16 V, USB attached so no light sleep;
the gauge's average current, settled for 60 s; COM8's GPS receiver is in its
backup-fault state, so a working receiver adds its own ~20–30 mA while powered):

| Configuration (awake, never sleeping) | Battery draw |
|---|---|
| 240 MHz, LoRa on, GPS off | ~99 mA |
| 80 MHz, LoRa on, GPS off | ~87 mA (CPU clock: −12 mA) |
| 80 MHz, LoRa off, GPS off | ~79 mA (LoRa receiver: −8 mA) |
| Full / Balanced / Saver as presets, awake | ~90 / ~82 / ~76 mA |

So with the CPU awake about 75 mA is board baseline that no knob reaches (USB PHY while
plugged in, e-ink/touch/keyboard/sensor rails, regulators). **Light sleep is where the
large saving is** (the ESP32-S3 drops from tens of mA to ~1–2 mA); its measured figure
was to go here, but the unplugged test found the reset described at the top of §6.

### 6.5 Planned

- **Fix light sleep** (first priority). Facts so far: reset reason 7 within ~1 s of
  `esp_light_sleep_start()`, also with a timer-only 0.5 s sleep and no wake pins; arming
  the RTC watchdog ourselves (paused in sleep) did not help. Next suspects: QSPI PSRAM /
  flash power-down settings in the prebuilt Arduino 2.x libraries, the super-watchdog,
  and the other core's tasks. `sleeptest <ms> [guard]` and the `rl` RTC log are there
  for this.
- **Find the ~75 mA baseline:** measure with each rail off in turn (BOARD_1V8_EN 38 /
  touch / keyboard / sensor hub), and check whether the modem enable (GPIO 41) or the
  BHI260 sensor hub draws while unused.
- **Deep sleep "storage" mode:** everything off, wake on a key, reboot on wake (~3 s).
  For a device left in a drawer; LoRa messages are not received.
- **LoRa receive duty cycle** (SX1262 RX duty-cycle with preamble detection) for a
  further cut in Saver, once tested against RNode-style 18-symbol preambles.
- **GPS power-save mode** on the u-blox M10 (cyclic tracking) instead of full off/on.
- A "Power" tile on Home and a battery-runtime figure on the idle splash.

---

## 7. Idle & notifications

| Topic | Behaviour |
|-------|-----------|
| Timeout | **S.3.2** (10–99999 s, default 30 s); never while a draft is unsent |
| Wake | Any **key** |
| Unread on splash | Updates on receive without leaving idle |
| Beep / vibe | **S.8** when enabled |
| With energy modes | The splash and light sleep are independent; in Balanced/Saver the device usually sleeps behind the splash |

---

## 8. GPS — Works (receiver-dependent)

The T-Deck Pro has a u-blox **MIA-M10Q** (UART1, 38400 baud, RX 44 / TX 43, power
GPS_EN 39) with a ceramic patch antenna, and a small rechargeable **MS621FE** backup cell
(J6, charged from 3.3 V through D8 + R15) on its V_BCKP pin.

- The firmware listens to NMEA (GGA, RMC, GSV), counts satellites heard per
  constellation, and logs the receiver's `$GxTXT` notices.
- **Link recovery:** if no valid sentence arrives for 3 s it cycles baud rates and sends
  u-blox "clear saved config + load defaults" (like LilyGo's `GPS_Recovery()`), then
  re-enables NMEA in RAM.
- **Backup-supply fault:** a receiver that reports `CANNOT USE BBR -> NO GNSS OPERATION`
  refuses to navigate. The firmware shows "backup cell low" and power-cycles the
  receiver every minute (every 5 min after 10 tries). On the unit at COM8 this fault is
  permanent under RatSpeak, LilyGo's own test and factory firmware; the unit at COM9
  navigates normally, as do three more units checked on 2026-11-09 (MACs …0E:34,
  …14:48, …17:CC). Suspected cause: the J6 cell or its D8 / R15 charge path on COM8.
- **Bench check of any unit:** `tools/gpscheck/check.sh COMx` flashes a tiny check app
  (LoRa kept off), listens 70 s and prints `VERDICT: GPS OK / GPS FAULT / NO DATA`.
- Console: `g` (status dump), `gp` (power-cycle the receiver), `gr` (hardware reset).

---

## 9. Hardware notes (T-DeckPro V1.1, 25-09-15)

| Function | Part | Pins / bus |
|---|---|---|
| MCU | ESP32-S3, 16 MB flash, 8 MB QSPI PSRAM | USB-Serial-JTAG console |
| Display | 3.1" e-ink GDEQ031T10, 240 × 320 | SPI CS 34, DC 35, BUSY 37, RST 16 |
| Keyboard | TCA8418, 4 × 10 matrix | I²C 0x34, INT 15, backlight 42 |
| Touch | CST328 | I²C 0x1A, INT 12, RST 45 |
| LoRa | SX1262, TCXO, DIO2 RF switch | SPI CS 3, IRQ 5, RST 4, BUSY 6, enable 46 |
| GPS | MIA-M10Q | UART 44/43, enable 39 |
| Gauge / charger | BQ27220 / BQ25896 | I²C 0x55 / 0x6B |
| Haptics | DRV2605 | I²C 0x5A |
| Audio | I²S amp | BCK 7, DOUT 8, WS 9 |
| SD card | | SPI CS 48 |

The battery gauge often stops at ~85% when full (the charger terminates before the
gauge sees the taper), so each device learns its own full point at a charge-done and
scales by it (S.5.9 "Full-charge cal").

---

## 10. Desktop simulator (`sim/`) — Works

A Qt app that runs **the real firmware** (`StandaloneApp.cpp`, keyboard driver, LVGL,
storage, the Rust protocol) on simulated hardware, in a window drawn like the device:
light frosted case, black keys with their symbol legends, the e-ink screen.
Full instructions: `sim/README.md`.

### 10.1 What it is for

- Try firmware changes without flashing.
- Run several simulated devices that talk to each other over a simulated LoRa "air"
  (UDP multicast — also across PCs on the same LAN).
- Scripted checks (exit code 0 = PASS) and screenshots for guides like this one.

### 10.2 Running

```bat
sim\build-sim.bat
sim\run-sim.bat alice
sim\run-script.bat sim\scripts\smoke.txt
```

Each name keeps its own identity, settings, messages and SD card under
`sim\build\simdata\<name>\`. The side panel sets battery %, USB / charge-done, GPS fix,
real e-ink timing, WiFi access points, and has a USB serial console box.

In the simulator the energy modes run their full logic (profiles, GPS schedule, sleep
decisions, statistics); "light sleep" is a 20 ms pause, since there is no hardware to
power down.

### 10.3 Live-device mode

```bat
sim\run-live.bat COM8
```

No firmware runs on the PC: the window shows the **real device's screen** over USB
(like VNC) and sends keys, clicks and console lines to it. The device mirrors only while
the PC's 2 s heartbeat arrives and stops 6 s after the last one. Scripts work here too,
so tutorials can be driven on real hardware. Protocol (`remote/ScreenMirror.h`): `@M`,
`@K`, `@T` in; `@HELLO`, `@FB <y> <rows> <crc16> <base64>`, `@ST` out. While mirroring
the device does not light-sleep.

---

## 11. USB serial console (115200 baud)

| Command | Does |
|---|---|
| `t:<text>` | send `<text>` in the open thread |
| `a` | announce |
| `h` `i` `c` `p` `s` | go to Home / Inbox / Contacts / Peers / Settings |
| `d` | dump the battery log |
| `g` / `gp` / `gr` | GPS status / power-cycle / hardware reset |
| `pw` | power status: mode, knobs, % asleep, wakes, blocker, battery |
| `pm0` … `pm3` | set mode: Full, Balanced, Saver, Custom |
| `bench <s>` | run from battery with USB attached (charger input off) for `<s>` s; `bench 0` stops |
| `pset <knob> <v>` | set a Custom knob and switch to Custom: `cpu 80\|160\|240`, `wake <s>`, `awake <s>`, `lora 0\|1`, `gps 0-3`, `wifi 0\|1`, `kbl 0\|1` |
| `blog 0\|1` | internal battery log off / on (RAM ring, 120 samples) |
| `rl` / `rl 0` | dump / clear the RTC power log (survives resets: time, uptime, mV, mA, USB, sleeps; a BOOT line with the reset reason at every boot) |
| `sleeptest <ms> [guard]` | diagnostic: one timer-only light sleep (resets the board today, see §6) |
| `L` | toggle key backlight |
| `FORMAT` | erase and format the data partition |
| `@…` | screen-mirror protocol (§10.3) |

Log prefixes: `[BOOT] [CFG] [POWER] [BENCH] [GPS] [LORA] [WIFI] [EINK] [BAT] [UI] [PROTO]`.

---

## 12. Secrets at rest (SecretVault)

Saved secrets (today: WiFi passwords) are never written to flash as plaintext.
`security/SecretVault` derives a key with PBKDF2-HMAC-SHA256 (10 000 rounds) from
**password + chip MAC + a random per-device salt** (NVS `rs_vault`), then seals each
secret with AES-256-GCM (random nonce, SSID as associated data).

- **Now:** the password is a built-in default, applied automatically at boot. This stops
  a flash dump or copied NVS from being read on a PC, but not someone holding the device.
- **Planned:** the idle-screen password (S.3.2 "On + password") becomes the vault
  password; at boot the vault stays locked until it is typed, so WiFi auto-connect waits.
- **Strongest option (not enabled):** ESP32-S3 flash + NVS encryption. Burns eFuses and
  can't be undone, so it is a deliberate per-device decision.

---

## 13. Stable hotkey policy & implementer checklist

**I / C / M / P** must stay. Settings is reached via **M → S** only. **Alt** = case in
entry mode.

1. Browse vs entry gates **I/C/M/P**.  
2. Underlines only when the action works.  
3. Settings rows are built by one function per page; count, numbering, digit jumps and
   touch hits all come from it.  
4. Input handlers request a redraw; only `loop()` renders.  
5. Idle: key wake only.  
6. Announce never implies private-key export.  
7. Hardware changes for power go through `powerApply()` only.

---

## 14. Roadmap (near term)

| Item | Status |
|---|---|
| Energy modes Full / Balanced / Saver / Custom, GPS schedule | Built |
| Light sleep | Off: resets the board; under investigation (§6.5) |
| Power numbers in §6.4 | Awake measured |
| GPS check of all five units (`tools/gpscheck`): only COM8 has the backup fault; COM3, COM5, COM6, COM9 OK | Done — report to LilyGo next |
| Idle password + vault unlock | Planned |
| Deep-sleep storage mode, LoRa RX duty cycle, GPS power-save | Planned |
| Simulator: show power state / sleep % in the side panel | Planned |

---

## 15. Document history

| Ver | Notes |
|-----|--------|
| 0.1 | Initial draft: how-to, device figures, settings tree, LoRa-first bring-up |
| 0.2 | Energy modes (S.4 Power), renumbered settings (Battery S.5 … Storage S.10, key 0), GPS diagnosis, hardware table, simulator + live mode, console reference, roadmap |
