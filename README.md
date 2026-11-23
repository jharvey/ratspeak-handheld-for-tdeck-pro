<div align="center">

# [Ratspeak Handheld](https://ratspeak.org/)

**Standalone messaging and RNode firmware for Reticulum handhelds.**

[![Status](https://img.shields.io/badge/status-beta-yellow.svg)](#devices)
[![License](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue.svg)](LICENSE)

[Ratspeak](https://github.com/ratspeak/Ratspeak) |
[Docs](https://docs.ratspeak.org/docs/hardware/handheld-guide) |
[Downloads](https://github.com/ratspeak/ratspeak-handheld/releases)

</div>

---

## This repository: RatSpeak for the LilyGO T-Deck Pro

This fork adds the **LilyGO T-Deck Pro** (ESP32-S3, 3.1" e-ink, keyboard, SX1262 LoRa,
GPS) as a standalone messenger. The rest of this page is the upstream Ratspeak text.

What the T-Deck Pro firmware does (state on 23 Nov 2026, beta):

- **Messages and contacts** over LoRa (and WiFi mesh): chats, delivery status, peers,
  contact cards, an Inbox.
- **Groups**: one message to up to 8 contacts; a group can be shared so members have it too.
- **Status sharing**: send your position and battery % to a contact or a group, once or
  automatically (off by default).
- **Markers (ATAK-style items of interest)**: mark a hazard, camp, water, meet-up ... with a
  note, by distance and direction or at the map cross; send it to a contact or group. The
  receiver sees "hazard 640 m NE of you" and a triangle on the map.
- **ATAK feed**: on WiFi, sends this unit, its contacts' positions and the markers as
  Cursor-on-Target to 239.2.3.1:6969, so phones running ATAK see them (not yet tried with a
  real ATAK phone).
- **Atlas**: offline topo maps from the SD card, places, waypoints, breadcrumb track and
  track back, navigate to a contact, arrival alerts.
- **Power**: energy modes from Full to Check-in, standby, battery gauge and low-battery
  notice; the idle screen (splash, If found, or the map) with one top banner.
- **Also**: password lock, walkie-talkie and voice messages over WiFi, Home Assistant
  voice, a library and games on the SD card, SD backup / restore, firmware update from SD,
  range test.

Documents:

| Document | What |
|---|---|
| [docs/HowTo.md](docs/HowTo.md) | Step-by-step guides, each with a video ([docs/videos](docs/videos)) |
| [RatSpeak_TDeck-Pro_Description.md](src/boards/tdeck_Pro/RatSpeak_TDeck-Pro_Description.md) | The full description: every screen, key and setting |
| [docs/sharing_atak_report.md](docs/sharing_atak_report.md) | Status sharing, markers, the ATAK feed: design, tests, open questions |
| [docs/groups_report.md](docs/groups_report.md) | Groups |
| [docs/standby_mode_report.md](docs/standby_mode_report.md) | Standby mode |
| [Stress_test_plan.md](Stress_test_plan.md) | How the firmware is stress tested |
| [sim/README.md](sim/README.md) | The desktop simulator (runs the real firmware) |
| [docs/videos/Video_Descriptions.txt](docs/videos/Video_Descriptions.txt) | Scripts of the how-to videos |
| [Future_steps.txt](Future_steps.txt) | Planned work |

Build, flash and test (T-Deck Pro):

- Firmware: PlatformIO environment `tdeck_pro_standalone`
  (`pio run -e tdeck_pro_standalone`), or `python tools/vm_build.py --commit` on the build VM.
  The current image is committed as `firmware/tdeck_pro_standalone.bin` (its `.txt` names
  the source commit and sha256).
- Flash a unit that already runs RatSpeak: `python tools/flash_app.py COM7`. A new or
  wiped unit: `python tools/flash_new_device.py COM7` (`--erase` blanks settings, identity and
  messages first).
- Simulator: see [sim/README.md](sim/README.md). Test scripts are in `sim/scripts`
  (screen scripts `*.txt`, multi-radio tests `*_net.py`, e.g. `marks_net.py`, `cot_net.py`,
  `groups_net.py`, `status_net.py`); how-to videos: `tools/howto/make_videos.ps1`.

---

Send encrypted LXMF messages over LoRa or WiFi, directly from your handheld.
No account or phone required. Keep contacts, discover peers, and carry your
conversations with you. LoRa messaging works without an internet connection.

This is the shared home for our handheld firmware, bringing rsDeck, rsPager
and rsCardputer into one codebase. The protocol core uses rsReticulumLite and
rsLXMFLite, with device support built around the same messaging services.

## Devices

| Device | Support |
| --- | --- |
| LilyGO T-Deck Plus | Standalone and RNode |
| LilyGO T-Pager (SX1262) | Standalone and RNode |
| M5Stack Cardputer Adv | Standalone and RNode; Cap LoRa-1262 required for LoRa |
| Elecrow ThinkNode M9 | Standalone; RNode coming soon |

An SD card is optional for normal messaging. Downloads are available for all
four devices.

This is beta firmware. If something isn't working, open an issue with your
device, firmware version, and the steps to reproduce it.

## Install

Download the ZIP for your device from [Releases](https://github.com/ratspeak/ratspeak-handheld/releases):

- **Full** — Standalone and RNode, with a launcher to choose between them at startup.
- **Standalone** — the on-device messenger, using LoRa or WiFi.
- **RNode** — use the handheld as a radio for Ratspeak, Sideband, or another
  Reticulum client over USB or BLE.

Files are named by device: `tdeck-full.zip`, `pager-standalone.zip`,
`cardputer-rnode.zip`, and `m9-standalone.zip`. Open the
[Ratspeak web flasher](https://ratspeak.org/download.html), select your device,
and choose **Flash in browser**. To install a downloaded ZIP, choose **Flash**
under **Build your own** and upload it.

For M9, extract `m9-standalone.zip` and flash its enclosed factory image with
esptool: `python3 -m esptool --chip esp32s3 --port PORT --baud 115200 write-flash 0x0 m9-standalone.bin`.
Replace `PORT` with your device's serial port.

ZIPs install a complete firmware layout. Back up your identity and messages
before flashing. The matching `.bin` files contain only the application and
must be written to the correct slot for your installed layout. See the
[installation guide](https://docs.ratspeak.org/docs/hardware/flashing-firmware)
for backups and updates, and the [handheld guide](https://docs.ratspeak.org/docs/hardware/handheld-guide)
for controls and setup.

## Build From Source

On Linux or macOS, install Git, Make, Python 3.12 and Arduino CLI 1.4.1, then:

```bash
git clone https://github.com/ratspeak/ratspeak-handheld
cd ratspeak-handheld
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install -r requirements-build.txt
make setup DEVICE=tdeck
make doctor DEVICE=tdeck
make package DEVICE=tdeck
```

Use `DEVICE=tpager`, `DEVICE=cardputer` or `DEVICE=m9` for the other devices. Packages
are written to `dist/`. Normal builds use the included Rust libraries; a Rust
toolchain is not required.

M9 builds Standalone only and does not require Arduino CLI.

## License

GNU Affero General Public License v3.0 or later. See [LICENSE](LICENSE).
Bundled RNode firmware retains its [GPLv3 license](vendor/rnode_firmware/LICENSE).
See [Third-party notices](THIRD_PARTY_NOTICES.md).
