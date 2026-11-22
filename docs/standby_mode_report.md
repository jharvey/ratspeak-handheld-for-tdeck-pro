# Standby when idle, lock button, boot to idle: report

Unattended run of 2026-11-22. No unit was flashed. No serial port was opened.
Both jobs are committed and pushed to main. The firmware image in `firmware/` was built on VM100.

## 1. Standby when idle

### What was built

A power option, **Standby when idle**. It is off by default.

- While someone uses the radio it runs in the chosen power mode, as before.
- After a set time without a key or touch it goes into **standby**.
- Standby stays until someone presses a key. Then the chosen mode is back at once and the timer starts again.

### How to turn it on and off

- On the radio: **MENU, S, 5** (Settings > Power). Go down to row **11 Standby when idle** and press **Enter**. That is the on / off switch.
- Row **12 Standby after** steps the time: 1, 3, 6, 12 or 24 hours. The default is 6.
- Console: `standby on`, `standby off`, `standby after 6`.
- The setting is saved (NVS `rs_power`, keys `sby` and `sbyH`) and survives a restart. Older saved settings load unchanged.

### How to see the state

- Power page, row 11: `ON: GPS off after 6 h idle` or `OFF`.
- Power page, row 13 "Now": `STANDBY for N min, GPS off` while in standby.
- Idle screen, bottom line: `STANDBY | press a KEY` (locked: `LOCKED | standby | key`).
- Console: `standby` or `pw` print option, hours, state, idle time and time to go.

### What each state switches off

| | Normal (chosen mode) | Standby |
|---|---|---|
| GPS | as the mode has it | off |
| WiFi | as the mode has it | off |
| Key backlight | as the mode has it | off |
| Touch | on | off until a key |
| CPU | as the mode has it | asleep, wakes every 5 min |
| LoRa receive | as the mode has it | unchanged: still listening (duty-cycled) |
| LoRa TX power | unchanged | unchanged |
| Screen | in use | idle screen, held by the e-ink for free |

From Full, Balanced or Saver the standby profile is the same as Ultra-low.
On demand and Check-in keep their own LoRa schedule in standby.

### Design choices (points 1 to 6)

1. **Messages still arrive.** Standby reuses the Ultra-low behaviour: LoRa on, duty-cycled receive (`rxdc`), CPU light sleep. No new radio scheme. TX power and `txp` are not touched.
2. **A message is not use.** It is received and notified as usual. The unit stays in standby.
3. **Off in standby:** GPS, WiFi, key backlight. The idle screen is shown. The last GPS fix stays on the map and in Time & Location with its age. The GPS line reads `GPS off (standby)`.
4. **The console is not use.** Console commands, console keys (`k`) and the soak test do not end standby and do not reset the timer. Only a real key or touch does. Test commands: `standby now`, `standby wake`, and `standby test <seconds>` (a timeout in seconds, not saved).
5. **Off by default.** The calibration units do not change behaviour. To make it the default: change `standby = false` to `true` in `power/PowerModes.h` and the `getBool("sby", false)` default in `PowerModes.cpp`. Units that never saved the option would then take the new default.
6. **Motion sensor.** The firmware does not drive the board's motion sensor (BHI260) at all. So "picked up" cannot wake it today. It would need a new driver. Not done in this run.

Further choices I made:

- **Standby also applies on USB power.** It overrides "Full power on USB". Otherwise bench units on USB would never reach it. On USB the CPU still stays awake while a USB host is attached (existing rule), so the saving there is GPS and WiFi only.
- **Touch does not wake it.** Touch is already off on every idle screen. A key wakes it.
- **Entering standby with "idle lock" on locks the unit**, as going idle does.
- Standby is not entered during a walkie-talkie call, a voice message, Home Assistant voice or USB drive mode.
- The UI rows are 11 and 12 on the Power page. Digits only reach rows 1 to 10, so they are reached by moving down. The "Now" and "Battery draw" lines moved from 11 / 12 to 13 / 14.

### Energy saved

Not measured. The firmware's own estimate table (`power::estimateMa`, from the bench readings of 2026-11-09/10) gives:

- Balanced with the GPS always on: about 67 mA. About 22 hours on the nominal 1450 mAh.
- Standby (same as Ultra-low): about 7 mA. About 8.6 days.

To measure it: put a unit on battery, type `standby now`, and read the gauge after 20 minutes or more (`pw`, or Settings > Power > Energy monitor, or the `rl` power log). The `bench` command gives the same reading with USB plugged in.

### Tests

| Test | Result |
|---|---|
| `sim/scripts/standby.txt` (option off never triggers; on in the UI; timeout reached; GPS off; console key does not end it; real key restores mode and GPS; `standby now / wake / off`) | PASS |
| `sim/scripts/standby_after.txt` (option and hours survive a restart) | PASS |
| `sim/scripts/standby_pair.py` (two simulators: a real message arrives in standby, the unit stays in standby) | PASS |
| `power.txt`, `checkin.txt` | PASS |
| `idle_screens.txt` | PASS, on a device I seeded with two library files. On a fresh device it fails before and after my change (it needs library files). |
| `tools/stress/monkey.py`, 2 rounds, 5965 keys | PASS |

Stated plainly: `standby.txt` alone does not prove the message case. The simulator script language cannot make a message arrive, so that part is in `standby_pair.py`. The real timeout in hours was not waited out; the test uses `standby test 8`.

Not tested on hardware: the current in standby, and waking from light sleep on a real unit.

## 2. Lock button and boot to idle

### What the top button is

- It is the **BOOT key on GPIO0**, low while pressed. Source: `BOARD_BOOT_PIN 0` in `src/boards/tdeck_Pro/config/BoardConfig.h` and in LilyGo's `examples/factory/utilities.h`. LilyGo's factory code uses it as a wake key.
- **Before:** the firmware did not read it at all.
- GPIO0 is a boot strap. Reading it at run time is fine. Holding it during a reset still enters the USB download mode. That is unchanged.
- The bottom button is the chip reset. Unchanged.

### What it does now

- **Short press, password set:** locks at once and shows the idle screen in its locked form. It uses the existing lock (`lockNow`), no second lock.
- **Short press, no password:** shows the idle screen. Nothing is locked.
- Works from every screen. A voice recording is stopped (nothing sent) and a walkie-talkie call is hung up first. A draft message stays in its buffer, as with the idle lock.
- Debounced (30 ms). It acts on the press; the release does nothing. Held through a boot it is not a press.
- It wakes the CPU from light sleep (new wake pin).
- At the password prompt it goes back to the idle screen.
- There was no long-press function, so there is no conflict.
- **Boot:** every start now ends on the idle screen, locked if a password is set. Before, it showed Home (or Inbox in Minimal mode), or the password prompt.
- Console: `btn` presses the top button. It is allowed while locked. In the simulator the menu item Device > Hold BOOT drives the real pin (`menu Device > Hold BOOT` in a script, the same line again to let go).

### Choices taken

- No password: the button shows the idle screen (as asked).
- **Standby and the button:** with a password the press locks and the unit stays in standby. Without a password the unit ends up unlocked, so the press counts as use and standby ends.
- **Ignored** during the first-run setup wizard and in USB drive mode. Those screens stay.
- **Boot exceptions:** a new unit shows the setup wizard. After a backup restore the normal screen shows so the result message is seen. The battery "plug in USB" state is untouched.
- Help text: Settings > Screen > Idle splash, row "Lock now" now reads "Enter, or the top side button". `docs/HowTo.md`, the product description (section 1.2c, console table, version row 0.17) and `sim/README.md` have one entry each.

### Tests

| Test | Result |
|---|---|
| `sim/scripts/lock_button.txt` (no password: idle, not locked; password: locks from a menu, the Inbox and the map; wrong password refused, right one accepted; real pin and console; in standby) | PASS |
| `sim/scripts/lock_button_boot.txt` (restart with a password: locked on the idle screen) | PASS |
| `sim/scripts/lock_button_boot2.txt` (restart without one: idle screen, not locked) | PASS |
| Every script in `sim/scripts/*.txt`: 59 before, 64 after (5 new) | 35 passed before, 41 pass after (the 5 new ones and `idle_screens`, which got its library files). No script that passed before fails now. |
| `tools/stress/monkey.py`, 2 rounds, 5969 keys | PASS (run on the build just before the one-line help text change) |

The other 23 scripts fail **before and after** in my run. They need prepared devices (map tiles, library files, a second simulator, a backup on the card) that a fresh device does not have: `checkin_migrate, contacts_map, coords, gps_map, hwmenu, hwreset, library, loc_a, loc_b, map, map_keys, map_menu, packs, places, rangetest_screen, sdtools, sdtools_after, sdtools_check, share_a, share_b, track_nav, waypoints, wifi`. I could not confirm these on their real devices.

**Scripts changed because of the boot change** (a `key BACK` added to leave the idle screen; it does nothing on Home). Broken by the change and now passing again: `identity_after, lock_boot, sdtools_restore, wizard_existing`. Changed the same way, but not confirmed on their prepared devices: `announce_a, announce_b, contacts_map, coords, devshot, energy, geofence, gps_map, idle_map, library, live_tcp, loc_b, map, map_keys, map_menu, more_box, packs, places, rangetest_screen, sdtools_check, share_b, smoke, track_nav, waypoints, wifi`.

`monkey.py` needed no change. It starts on a fresh device, where the wizard shows.

Not tested on hardware: the real button, its debounce, and the wake from light sleep. The light-sleep code for the button compiles on VM100 but only runs on a unit.

### How-to scripts and document lines to update (not edited: another job owns them)

- `sim/scripts/howto/*`: all 33 start from fresh devices and use `serial wiz skip`, so the boot change does not break them. None restarts a unit mid-script. No edit is needed for them to run.
- Wanted additions, for the owner of those files:
  - `Password_lock_and_unlock`: show the top button as the quick way to lock.
  - `docs/videos/Video_Descriptions.txt` lines 29 to 31 ("a unit wakes to the screen used last ... in Minimal mode it always wakes to the Inbox"): add that a restart ends on the idle screen, and that the first wake after a restart goes to Home (Inbox in Minimal mode).
  - Lines 237, 285 and 323 ("wakes to the Inbox"): still true for waking; no change needed unless the video shows a restart.

## 3. What the owner needs to decide

1. Flash the new firmware or not. The calibration and soak units were left alone.
2. Make Standby when idle the default or keep it off.
3. Is "standby also on USB power" right, or should USB keep the unit in Full?
4. Rows 11 and 12 need scrolling. A digit shortcut would mean moving another Power row.
5. Should the top button with no password count as use (it does now)?
6. Measure the standby current on a unit before quoting battery life.
7. C: free space fell to 9.9 GB during the run. My test output there was 19 MB; the rest came from elsewhere. Worth a look.

## 4. Commits

- `071c48c` Standby when idle. Firmware: `ec8bb62`.
- `d582a50` Lock button, boot to idle, this report. Firmware: `8cff149` (2,348,080 bytes, sha256 starts `654d7cd0cbd9598a`). The build tool labelled it "+ uncommitted changes": that was another job's edit to `tools/howto/make_videos.ps1`, not firmware source.
