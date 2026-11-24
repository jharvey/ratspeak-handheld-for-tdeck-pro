# RatSpeak T-Deck Pro: stress test plan

Draft, 2026-11-12. Goal: before wider use, show that the handheld keeps working for days
under heavy, messy use: no crashes or restarts, no slow memory loss, no lost or stuck
messages, and a sensible outcome whenever something fills up or goes away.

**Small limits first, then normal limits.** Every limit (flash full, SD full, outbox,
contacts) is first tested with a tiny limit so it is hit within minutes, not days. Only
when the short runs pass do the long runs use the normal limits. Two ways to get there fast:

- **Shrink the limit:** `flashcap <n>` (console, RAM only, a restart clears it) makes the
  message upkeep act as if the flash fills after about *n* more messages. In the simulator
  `--flashkb <KB>` shrinks the whole flash. `flashcap 100` + 300 messages: removal started
  after 65 messages (new conversation folders also take space), 227 removed, 0 failed saves
  (`sim/scripts/flashcap.txt`).
- **Preload:** `msgfill <n> [conversations] [bytes]` stores *n* fake messages through the
  normal save path in seconds, so a unit can start a test already near full and overflow
  after the next ~100 real messages.

Three layers, cheapest first. Most bugs should be found in the simulator, so the units
are only needed for what the simulator cannot model (radio, real flash timing, power).

---

## 1. What "pass" means

| Check | Pass |
|---|---|
| Restarts | None during the run (`rl` shows no new BOOT lines; reset reason logged if one happens) |
| Memory | Free heap and the largest free block level out after the first hour; the lowest-ever free heap stays above a floor (to set from the first runs, e.g. 40 KB internal) |
| Messages | Every sent message ends **delivered** or visibly **failed**; none stuck as queued/sending for more than its retry window; nothing received twice in a thread |
| Outbox | Empty again within a few minutes after the other unit comes back in range |
| Storage | Flash and SD fill without failed writes to identity, ratchet or contact files (§8.3 of the description) |
| UI | Keys still answer within ~1 s after hours of use; the screen is not stuck on an old frame |
| Power | Sleep still works at the end (sleep % and current like at the start) |

## 2. Layer 1: simulator soak (no hardware, run overnight)

The simulator runs the real firmware. Two simulators share a simulated radio (UDP
multicast), so they can message each other.

| Test | How | Looks for |
|---|---|---|
| **Message ping-pong** | Two sims (`alice`, `bob`); a script sends `msg <peer> <text>` both ways every few seconds, with a counter in the text, for 8+ hours | Every counter arrives once and is shown delivered; outbox drains; no restart |
| **Random keys ("monkey")** | Script presses random keys with weights: open/close the map (L), zoom (Z/X), pan (WASD), menus, settings, compose, back. Avoids only factory reset / identity erase | Crashes, hangs, screen stuck, slow growth in frame time |
| **Map tour** | Script walks the map: every zoom level, pans across tile edges, missing tiles, SD removed and re-inserted (`sd out` / `sd in`) | Crash on missing/damaged tiles; tile cache leaks |
| **Small storage** | Ping-pong and monkey with `--flashkb 1024` (or `flashcap 100`) and a nearly full simulated SD card | Full-flash path every few minutes: removal, refusal, recovery after deleting conversations |
| **Restart in the middle** | Kill and restart a sim at random points while it saves | Boot always works; no broken records; outbox recovered |
| **Memory-check build** | Build the simulator with AddressSanitizer / UndefinedBehaviorSanitizer and run all of the above | Buffer overruns, use-after-free, leaks that do not crash every time |

Tools:
- DONE: `hb <s>` heartbeat line: uptime, free heap / internal / lowest-ever / largest block,
  PSRAM, outbox, unread, conversations, `inc` (new incoming messages saved since boot),
  flash %, messages removed for space; in the simulator also `proc` (the process's real
  memory in KB, since the simulated heap numbers are fixed).
- DONE: `msgstat <peer>`: a conversation's records by direction and status, plus the
  numbered test messages received (count, duplicates, gaps).
- DONE: `tools/stress/pingpong.py`: two simulators (own TCP ports and own radio channel via
  `RS_SIM_AIR_PORT`, so other simulators on the PC do not interfere) or two units
  (`--ports COM5 COM9`); numbered messages both ways, waits for the outboxes to empty,
  checks with `msgstat` + `hb`, PASS/FAIL in `summary.txt`. `--flashcap N` / `--flashkb`
  for the full-storage path.
- DONE: `tools/stress/powercut.py`: kills the simulator at random moments (during saves,
  sometimes during boot), restarts, checks boot, identity, no saved message lost
  (records >= before + saves the heartbeat confirmed), every record readable (`msgstat`
  bad=0); deletes the test conversations above 1,000 records so the firmware's own
  removal at 85 % does not count as a loss. `--flashkb` for the full-flash path.
- DONE: checked simulator build: `sim\build-sim.bat checked` -> `sim\build-checked`
  (undefined-behaviour + array-bounds traps, libstdc++ assertions, stack protector,
  _FORTIFY_SOURCE). MinGW GCC has no AddressSanitizer, so heap use-after-free is not
  caught. Runners use it with `set RS_SIM_EXE=...\sim\build-checked\ratspeak-sim.exe`.
- DONE: `tools/stress/monkey.py`: rounds of random key taps (letters, digits, Enter, Back,
  Backspace, Space, SYM / ALT combos, short pauses, now and then GPS / USB / SD / battery
  changes) on a fresh simulator; FAIL on an exit without PASS, a trap / assert line, or a
  heartbeat gap of 60 s. A failed round keeps its script (replay with `run-script.bat`).
- DONE: flash wear counters in the simulator (`sim/shim/src/wear.cpp` -> `<data>/wear.txt`:
  LittleFS writes per path pattern, renames / removes / mkdirs, changed NVS values) and
  `tools/stress/wear.py` (idle run + ping-pong run -> erases per day, years to wear-out).

## 3. Layer 2: real units (24-72 hours)

Two or three units on the bench, each on USB to the PC. A PC script talks to every
console at once and logs everything to one file per unit.

- **Traffic:** `msg <peer> <text>` between units on a timer (e.g. every 30-90 s, random),
  in every energy mode the units use (Balanced, Saver, Check-in). Counters in the text,
  as in the simulator.
- **Map:** key presses sent over USB (the live-mode `@K` key command) to open, zoom and
  pan the map with the real SD card and real tiles.
- **Health:** the `heap` heartbeat every minute, `pw` every 10 min, `rl` (power/RTC log
  with reset reasons) at the end. Battery level and current from the gauge.
- **Crashes:** the board has a core-dump area in flash; after any restart the script
  saves it so the crash location can be decoded.
- **TX power:** keep every unit whose antenna status is unknown at -9 dBm (see
  AI-guide.txt for which units).
- **Short first:** a 1-2 h run with `flashcap 100` (or a `msgfill` preload) so the flash
  limit is crossed many times; then the long runs at normal limits.
- **Pass:** as in §1, over at least 24 h; then one 72 h run before a release.

## 4. Layer 3: nasty cases (once each, by hand or scripted)

| Case | What should happen |
|---|---|
| Full flash on a unit (`msgfill`) | Oldest messages removed from 85 %, refusal at 92 %, toasts shown; time one removal pass and the usage check on real LittleFS |
| Full SD card (archive on, map tiles) | Archive writes fail quietly, flash keeps working, map still opens |
| SD removed while the map or archive is in use | No crash; "no tiles" / archive off until the card is back |
| Power pulled during a save (battery out, or `gr` style reset loop) | Boots; at most the message being saved is missing; no broken files |
| Other unit out of range for an hour, then back | Messages queue, then deliver; nothing duplicated |
| 100+ other nodes announcing (simulated crowd) | Contacts list and memory stay bounded (announce table caps) |
| Long message (max size), many short ones in a burst | No truncation; queue limits respected |
| Check-in mode for a week | Windows still on time (RTC drift ~2.9 s/day); outbox flushes |
| Password lock + idle for days | Unlock still works; messages received while locked are shown after unlock |
| Low battery / brown-out | Clean shutdown at the cut-off; boots normally when charged |
| Flash wear (later, see Future_steps.txt) | Write counter during soak runs: no file or NVS key rewritten often enough to wear the flash out within years of heavy use |

## 4a. Power cuts: pick up where it left off

Goal: after a power blip, reset or crash, the unit boots and carries on with at most the
one message being saved at that moment missing, and nothing half-written or stuck.

What is designed for this today (to prove, not assume):
- Message records are written to a temporary file, then renamed; a `.bak` copy covers a
  cut during a rewrite; leftovers are cleaned at boot (atomic write + recovery).
- Queued outgoing messages are found again at boot and resent (outgoing recovery); the
  Check-in outbox is in NVS.
- An incoming message's delivery proof is only sent after it is saved, so a cut before
  the save makes the sender retry instead of losing it.
- Settings, TX power, identity: NVS / atomic files.
- Map copies and downloads on the PC side resume from the NAS (`M:\ratspeak-maps\STATUS.txt`).

Tests:
| Test | How | Pass |
|---|---|---|
| **Reset loop while saving** (automated) | PC script runs `msgfill` (or ping-pong traffic) and resets the unit at a random moment through the USB serial lines (the same RTS/DTR reset as opening the port), several hundred times | Every boot succeeds; message count only ever grows by what was confirmed; no broken records; queued messages go out after boot |
| **Real power cut** | Battery-only unit, pull the battery during saves / map copy / settings change (10-20 times by hand) | Same as above; settings unchanged |
| **Simulator kill** | Kill the simulator process at random points during saves, restart, repeat | Same as above, fast enough for hundreds of runs overnight |
| **Brown-out** | Run the battery down under load | Clean stop; normal boot when charged |

## 4b. Battery life: normal use, unplugged, until empty (planned, week of 2026-11-16)

Goal: real runtime numbers for each energy mode, measured on units used the way a person
would use them, as the basis for the time estimates in Settings > Power (S.4.12) and the
guide. Start only after the current development is done (user, 2026-11-13).

**Traffic pattern (normal use):** once an hour, unit A sends a message, B answers, A
answers, B answers: 4 messages per hour, 96 per day, each 40-80 characters, with a counter
and A's battery % / B's battery % in the text. Between exchanges the units sit idle with
the screen on the idle splash (lock off), as in a pocket.

**Unplugged = no PC.** The units run on battery only, so the traffic must come from the
firmware itself:
- New console command (set over USB before unplugging, kept in NVS): `bench chat <peer>
  every 60 [lead|answer]` - the lead unit starts an exchange every 60 min at a fixed
  minute; the answer unit replies to bench messages after a short random delay (5-30 s),
  up to the 4th message. `bench off` stops it. Normal messages are not touched.
- Every exchange and every 10 min: one line to `<SD>/ratdeckpro/logs/battery.csv`
  (the S.5.8 battery log): time, uptime, %, mV, mA (gauge), power mode, messages sent /
  received / failed, delivery time of each message. Also in the RTC power log (`rl`),
  which survives the final brown-out.
- At the end the last CSV line shows the cut-off point; the % reading vs. real time left
  also checks the gauge (the 85 % "full" calibration, S.5.10).
- Learned capacity (Future_steps 15): before each run `batcap` (and after the charge, once
  unplugged: "start counting at 100 % (full charge)"); the `[BAT] capacity:` lines in the
  log show each rest reading and every learned value. Compare the learned mAh with the
  total mAh the run actually drew (battery.csv / energy monitor) to the cut-off.

**Runs (each until the unit shuts down, then charge to full and start the next):**

| Run | Units | Mode | Expected (rough, to be replaced by the result) |
|---|---|---|---|
| 1 | COM5 + COM9 | Balanced (default) | 2-4 days |
| 2 | COM3 + COM6 | Saver | 5-8 days (~7 mA asleep) |
| 3 | COM5 + COM9 | Full | under 1 day |
| 4 | COM3 + COM6 | Check-in (windows every 60 min, exchange inside the window) | longest |

Runs 1 + 2 can go at the same time (two pairs, different modes); 3 + 4 after. A pair
holds the same mode so both ends behave alike. Same TX power on both (22 dBm with
antennas). Full charge first (charge-done on the charger), same room temperature,
units 2-5 m apart; GPS left at the mode's default.

**Before the real runs:**
1. Simulator: the `bench chat` command with two simulators (scripted, fast clock not
   needed: 1-minute interval for the test), battery drain from the simulated gauge.
2. A short 2-hour run on one pair on USB with the PC logging the console, to check the
   CSV lines and that 4 messages per hour really go out and arrive.

**Results:** hours to shutdown per mode, mAh per day (gauge), messages delivered / lost,
average delivery time, gauge % vs. real time left. They feed the S.4.12 "estimated time
left" and the table in the guide's §6.

## 5. Order of work

1. Add the `heap` heartbeat line (firmware) and the PC logger (also used for the units).
2. Simulator: ping-pong runner, then the `monkey` command, then the sanitizer build.
3. Overnight simulator runs; fix what they find.
4. Power-cut tests (§4a): simulator kill loop first, then the USB reset loop on one unit.
5. Units (after the map copies finish): full-flash timing on one unit, then a 24 h
   two-unit run, then the nasty cases.
6. One 72 h run with all three units before calling it stable.

Results go in a table at the end of this file (date, build, layer, hours, result, issues).

## 6. Results

| Date | Build | Test | Hours | Result | Notes |
|---|---|---|---|---|---|
| 2026-11-12 | 1dcbd88 | Full flash, simulator (`fullflash*.txt`) | - | PASS | 1,500 msgs in 2 MB: 1,142 oldest removed, no failed writes; refusal at 93 % with nothing removable |
| 2026-11-12 | (flashcap) | Small limit, simulator (`flashcap.txt`, 8 MB flash, `flashcap 100`) | - | PASS | 300 msgs of 50 chars: removal from message 65, 227 removed in 32 passes of 25-100 ms, 0 failed |
| 2026-11-12 | (ping-pong) | Ping-pong, simulator, every ~8 s each way | 0.05 | PASS | 20 + 22 msgs, all arrived once, all delivered, no restart |
| 2026-11-12 | (ping-pong) | Ping-pong overload, every ~3 s each way (more than SF11 airtime allows) | 0.07 | PASS | 41 + 40 msgs all delivered; queues drained in ~210 s; 14-16 sends per side refused by the 3 s send spacing; one send took 16 s to be accepted (console/UI blocked meanwhile) |
| 2026-11-12 | (ping-pong) | Ping-pong overload + `flashcap 30` | 0.07 | PASS after 2 fixes | Found: (1) removal took outgoing "sent" (no proof yet) records the engine may still retry -> now only delivered/failed outgoing are removed; (2) the new-message beep keyed on a rising unread count, so a message whose save also removed old unread ones gave no beep -> now keyed on saved incoming messages. After: 40 + 41 arrived, 0 dup, no restart |

| 2026-11-12 | (no-freeze) | Ping-pong overload after the send fix | 0.07 | PASS | 48 + 48 msgs all delivered; slowest send answer 1.1 s (was 16-24 s); busy / 3 s spacing now go to the outbox; 24-25 per side refused only when 8 already waited ("outbox full") |

Fixed findings:
- A send blocked the console and UI for up to ~16-24 s when the radio was busy
  (sendMessage retried "busy" 12 x ~1.2 s inside the main loop). Now one attempt; busy or
  the 3 s spacing puts the message in the outbox (shown "queued", retried in the
  background every ~3 s, kept in NVS); "outbox full" (8) keeps the draft.

| 2026-11-12 | (checked) | Ping-pong 6 min + `flashcap 40`, checked build | 0.1 | PASS | 59 + 60 msgs all arrived, no trap / assertion, no restart |
| 2026-11-12 | (checked) | Power cuts, checked build | 40 cycles | PASS | 6 cuts in boot, 34 during saves; 1,323 records, none lost or unreadable, identity kept |
| 2026-11-12 | (powercut) | Power cuts, tiny flash (`--flashkb 1024`, removal active) | 40 cycles | PASS | 9 cuts in boot, 31 during saves; no unreadable record, always boots |
| 2026-11-12 | (powercut) | Power cuts, normal flash | 120 cycles | PASS | 13 cuts in boot, 107 during saves (bursts of 40-200 saves, 20-600 byte messages); no saved message lost, none unreadable, identity kept, boot 5.5-16 s. (A first run reported "lost" records above ~1,400: that was the firmware's own removal at 85 %, not a loss; the runner now clears the test conversations at 1,000.) |
| 2026-11-12 | (index) | Boot with 1,400 messages, saved record index | 2 boots | PASS | record opens at boot 1,400 -> 8, message start 1.5 s -> 0.2 s; `msgidx check` 0 mismatches |
| 2026-11-12 | (index) | Power cuts with record index, normal flash | 60 cycles | PASS | 9 cuts in boot, 36 during saves, 15 while idle (around the 30 s index save); 14 boots from the saved index; index matched flash every cycle |
| 2026-11-12 | (index) | Power cuts with record index, tiny flash (`--flashkb 1024`) | 30 cycles | PASS | removal active; 7 boots from the saved index; index matched flash every cycle |
| 2026-11-12 | (index) | Ping-pong 8 min with record index | 0.13 | PASS | 51 + 50 msgs, all delivered; index matched flash on both sides |
| 2026-11-12 | dad0b2f | UI branch merged into main: 21 sim scripts, ping-pong 4 min, power cuts | 25 cycles | PASS | 11 boots from the saved index; map / map_menu with test tiles |
| 2026-11-12 | dad0b2f | Boot from the saved index on a unit (COM3, 212 messages) | 2 boots | PASS | message start 11.3 s (212 opens, 9.9 s opening) -> 0.58 s (4 opens); `msgidx check` 0 mismatches |
| 2026-11-13 | (wear) | Flash wear: idle sim 1 h + ping-pong 30 min normal rate (115 msgs) + 60 min overload (705 msgs) | - | PASS | 500 msgs/day: LittleFS ~450 erases/day = 185 years with flash 85 % full (1,100 years mostly empty); NVS 114 years (busiest: clock save once a minute). Overload all day (outbox nearly full): NVS ~10 years (outbox rewrite) - see findings |
| 2026-11-13 | (ping-pong) | Ping-pong 60 min overload (every ~8 s each way) | 0.13 | PASS | 352 + 353 msgs, all arrived once, all delivered, no restart, queues drained in 292 s |
| 2026-11-13 | (0.15) | Outbox `q2` + boot recovery wait: 18 sim scripts, old-format outbox moved, 30 power cuts, ping-pong 15 min overload (every ~4 s) | 30 cycles | PASS | sim boot 5.4-6.2 s -> 1.0-3.0 s (recovery done after ~0.4 s); 108 + 108 msgs all delivered once |
| 2026-11-13 | 2396882 | Units COM3 <-> COM6, ping-pong, every ~45 s each way | 2 | PASS (runner said FAIL) | 116 + 117 msgs, all arrived once, no gap, no restart, index matched flash. The FAIL was one old outgoing "sent" record already in COM3's history (327 outgoing). COM3 flash writes take 1-1.7 s each (COM5/COM9 0.2-1.2 s) |
| 2026-11-14 | 2396882 | Units COM5 <-> COM9 side by side, 22 dBm, every ~45 s each way | 1.7 | FAIL (radio) | 120/127 and 123/126 arrived; no dup, no restart, heap flat, index matched flash. Lost ones ended "no proof" after the retries. Received at -7..-9 dBm with SNR only 5-8 dB and 8 CRC failures: receiver overload at ~0 m plus collisions. Re-run with the units apart (several metres / a wall) before reading anything into it |
| 2026-11-14 | 2396882 | Simulator ping-pong, every ~20 s each way | 4 | FAIL -> fixed | 680 + 699 msgs, no restart, memory flat, index ok; 1 duplicate: a 55 s flash write (PC busy) made `sendMessage` give up after ~8 s ("timeout" = failed) although the engine had the message; the runner (as the outbox or a user would) sent it again. See findings |

Open findings:
- FIXED 2026-11-14: a send whose save took over ~8 s was reported "timeout" (failed) although
  the engine had accepted it and still saved and sent it: the outbox kept and resent it, the
  share-card / compose path showed "Not sent" (user resends) -> duplicate; and the ticket was
  never acknowledged, holding one of the engine's 20 slots for good (about 20 such stalls and
  nothing could be sent until restart). Now it counts as sent ("saving") and the main loop
  finishes the ticket (`txLateTick`, "[TX] late save: ..."). Simulator hook
  `RS_SIM_SLOW_OUT_MS=<ms>` slows every outgoing record save to test it. Ping-pong 8 min
  with 12 s saves: 21 late saves, all "saved", no duplicate, nothing reported failed; 2 of 14
  A->B ended "no proof" because the receiver's store was saturated (each message costs
  ~36 s of saves every ~20 s), so the packets were heard but not taken in. Control with 6 s
  saves: 17 + 18 all delivered once.
- Under store saturation an incoming message that is heard on the radio is not taken in, and
  nothing is logged on the receiver (the sender ends "no proof"). Worth a "[MSG] dropped:
  store busy" line.
- FIXED 2026-11-13: flash wear (simulator counters, `tools/stress/wear.py`): nothing near the
  limit in normal use. The outbox was rewritten whole (up to 8 x 177 B, ~41 NVS entries) on
  every add / send (NVS ~10 years with the radio overloaded all day). Now saved without
  padding (key `q2`): overload ping-pong 173 B / 6.8 entries per save instead of 1,266 B / 41.
- FIXED 2026-11-12: boot time grew with stored messages (the startup scan opened
  every record: ~36 ms each on COM5, 376 records = ~16 s; near the 85 % mark it would have
  passed a minute). A saved record index now replaces the scan when it matches the file
  names (Description §8.4). On COM3 (212 messages) message start went 11.3 s -> 0.58 s.
- Under load, retried messages arrive late and the thread shows them in arrival order
  (e.g. "8" below "40"). Consider ordering threads by the sender's timestamp.

| 2026-11-16 | 70f9d47 | 4 units, group chat (`tools/stress/chat4.py`, COM3/5/6/9), half typed with keys, UI tours | 2.5 | PASS | 621 msgs, 620 arrived, 1 failed after its link request was lost (shown failed, not stuck); 305 typed messages all exact; 213 screen checks ok; no restart, crash or hang; internal heap -1.5..2.5 KB over 2.5 h (levelling; 24 h run to confirm) |

Fixed findings (2026-11-15/16, 4-unit soak):
- Mark read of a 100+ message backlog was one store job (~1 s per record): the store was held
  for minutes, every send froze 8-12 s and threads opened empty. Now 2 records per job.
- An open thread reloaded its history and marked read every 2.5 s (each mark-read bumped the
  store revision, which reloaded the thread): ~330 ms of loop time every 2.5 s. Now mark read
  only when a new message came in.
- The idle timer could fire while typed keys were still queued; the first key became the wake
  key and was swallowed (a lost letter). No idle now while keys wait.
Open: processKeys / watchBattery sometimes take exactly 1000 ms with no key pressed (an I2C
wait; COM5 71 times in 2.5 h). Logging of slow keyboard / gauge reads is in the next build.
A send holds the loop ~2.5-5 s while the record is saved.

### 2026-11-23: markers and the ATAK feed on real units

| Date | Build | Test | Hours | Result | Notes |
|---|---|---|---|---|---|
| 2026-11-23 | 04c172f | Simulator: `marks_net.py` (3 radios), `cot_net.py` (2 radios), full regression (status, groups, lock, standby, idle, wifi, groups_net, standby_pair, status_net, monkey x2) | - | PASS | 8/8, 4/4, all PASS |
| 2026-11-23 | 04c172f | Units COM9 -> COM6: one marker over LoRa | - | PASS | stored and listed on COM6 |
| 2026-11-23 | 04c172f | Units COM3, COM5, COM8 (USB, WiFi) + COM6, COM9 (air): markers every 45 s to a unit, to COM6 / COM9 and to a five-unit group; ATAK feed every 10 s; PC on 239.2.3.1:6969 (`M:
atspeak-soaktak_stress.py`) | 0.4 | NOT CONCLUSIVE | 5 of 46 markers arrived: COM3 (94 %) and COM5 (92 %) refuse new messages (flash full from the soak), outboxes full. Feed sent 1,528 / 1,410 events; the PC got none, also not by unicast (PC on a WiFi network marked Public in Windows). Heap steady (COM8 lowest: 22 KB, internal 16 KB), no restarts. COM8's feed stopped after 9 events and its console stalled up to 16 s |

Findings to follow up: COM8's stalled console and stopped feed; a store that is full refuses
markers and status updates as well as chat (by design today, but those small updates could
be kept apart); re-run with stores that are not full and a receiver that can take UDP.

### 2026-11-23: full store that could not clean itself; timed random tasks

Found on COM3 / COM5 (soak units, stores 92-94 % full): the flash upkeep only removed
finished outgoing messages (delivered, failed), so messages that ended "unconfirmed" (6) or
stayed "sent" (3) to radios that had gone never left; every save started a cleanup pass that
freed nothing, and new messages were refused. Fixed: unconfirmed messages and sent messages
more than an hour old are removable too (the engine only resends queued / sending ones).
Tests: `sim/scripts/fullflash_unconfirmed.txt` (300 old unconfirmed on a 1 MB flash: all
saved) and `fullflash_sent_young.txt` (fresh sent messages stay pinned: the store fills and
refuses, nothing lost). Console `flash` now says "busy" instead of "0 of 0 KB" while the
storage task holds the flash.

`tools/stress/tasks.py`: random realistic tasks (Atlas pan / zoom, Settings, Contacts, chat,
markers, layers, Library, Peers) with a time budget per step (key to finished screen: 2 s,
4 s for screens that read storage), cycling energy modes; simulator with real e-ink timing or
a real unit (`--port`). First runs (simulator): a step is one e-ink partial refresh, about
660 ms; opening the Atlas and returning to it after a sub-page are full refreshes, about
1.6 s; heap flat.

### 2026-11-23 afternoon: after the clean-up fix (image 73780f7)

| Date | Build | Test | Hours | Result | Notes |
|---|---|---|---|---|---|
| 2026-11-23 | 73780f7 | Markers + ATAK feed rerun, COM3 / COM5 / COM8 (stores cleaned by the fix: 76 % / 75 %) | 0.4 | PASS (markers) | 46 of 46 marker deliveries to the USB units; over the air to COM6 / COM9 all delivered, 0 failed; no restarts. ATAK feed: still 0 on the PC (Windows network set to Public). COM8 lowest internal heap 5.9 KB with its feed left on all day (40,961 events): to watch |
| 2026-11-23 | 73780f7 | tools/stress/tasks.py on COM3 and COM5, 8 min each, Full / Balanced / Saver | 0.3 | timing | a normal step ~850 ms (one partial refresh); Atlas open and some pan / zoom 2.4-2.8 s (full refresh ~2.5 s on the panel); back from the marker list up to 3.0 s; one Library page 6.5 s. Heap flat (COM3 ~35 KB, COM5 ~38 KB). Back to the map now uses a partial refresh (2495741) |
| 2026-11-23 | a7dfadc | Full regression (all screen scripts incl. fullflash_*, all network tests incl. relay_net, monkey x2) | - | PASS | |


### 2026-11-23 evening: SOS, ATAK memory full, PSRAM threshold

| Date | Build | Test | Hours | Result | Notes |
|---|---|---|---|---|---|
| 2026-11-23 | 330f0a4+ | Simulator: `sos_net.py` (3 radios), `sos_checkin.txt`, `lock_button*.txt`, full regression incl. monkey x2 | - | PASS | 8/8; Check-in: LoRa on for the SOS, back to Check-in after "SOS cancelled" is out |
| 2026-11-23 | 330f0a4 | `atak_full.txt`, checked build: 40 ATAK units, 12 own + 30 ATAK markers, 4,000 broken CoT events (cut, noise, bad numbers, huge callsign, no uid) | - | PASS after 2 fixes | Tables held at 16 units / 24 markers, no crash. Found: ATAK markers could push out every other marker, and each was a flash write (a flood = one NVS write per event). Now ATAK gets at most 12 of the 24, saved at most every 30 s; 4,000 events 2.2 s -> 0.5 s |
| 2026-11-23 | 71101eb + psramth 1024 | COM8 under the marker / ATAK stress + timed tasks | 0.7 | PASS | lowest internal free 33.9 KB (COM3 17 KB, COM5 22 KB at the old 4096; COM8 5.9 KB before); UI steps unchanged (~840 ms). 1024 is now the default |
| 2026-11-23 | 25fba8a | Real radios, `M:\ratspeak-soak\sos_hw.py` COM3 -> group (COM5, COM8 + COM6, COM9 on the air) | 0.3 | 4/7 -> fixed | First SOS reached COM5 5 min late, after an update; updates queued before a cancel arrived after it; first SOS sent twice. Fixed: SOS first in the outbox, newer position replaces a queued one, cancel drops queued SOS texts, duplicate removed |
| 2026-11-23 | 8cc1e32 | Same, rerun | 0.3 | PASS 7/7 | countdown cancel 400 ms; first SOS 149 s to both (a group message needs a link on LoRa: link setup ~2 min at this bitrate); update 1-7 s, cancel 4-7 s, quick reply OK |

Open: an SOS to a group sets up a link first (~2 min on LoRa); to saved contacts it goes as
one packet. Consider sending the SOS to each member as a plain message (no group fields) so it
fits one packet. Battery-life runs (4b) are still not done.
| 2026-11-23 | d88ee78 | Real radios, group SOS as plain messages: COM8 -> group (COM6, COM9); ATAK feed COM8 -> PC | 0.3 | PASS (6/7, see notes) | COM6 alarm after 10 s (was ~150 s as a group message); COM9 164 s: its first packet failed CRC (units side by side, RSSI -22..-35 dBm, SNR 1-4: receiver overload) and the retry got through; update 73 s, cancel 2-16 s, quick reply OK. The failed check is the test's order (COM6's 60 s alarm ended while it waited for COM9). ATAK: after the PC allowed Python on the network, the PC got 201 events, all valid CoT (9 units, 192 markers) |

### 2026-11-23 17:15: battery run 1 started (4b)

COM3 + COM5, firmware 4b38139, Balanced (`pm1`), both 100 % (4.13 V) on USB before unplugging.
Traffic: the on-device soak (`soak on 30 <other>`: a message about every 30 min, most answered),
logged to each unit's SD card (`ratdeckpro/soak.log`, COM3's card re-seated and working).
Status reports ("PS ... mV pct ma heap ...") every 5 min, and "dark" just before the
switch-off, to the listener COM8 on USB (`M:\ratspeak-soak\2026-11-17\COM8_listener.log`);
`batcap` started the learned-capacity count. Units apart, unplugged by the owner.
**Result (2026-11-24): Balanced + WiFi joined + ATAK feed on (worst case).**

| | COM3 | COM5 |
|---|---|---|
| Unplugged | ~17:52 (first "dsg" report 17:54) | ~17:52 |
| Switched itself off ("dark") | 06:29 | 07:23 |
| Runtime on battery | **12 h 37 m** | **13 h 31 m** |
| Average draw | 85-100 mA, mostly ~90 | 85-103 mA, mostly ~92 |
| Used (draw x time) | ~1150 mAh | ~1250 mAh |
| Gauge at 6 h / 12 h / off | 50 % at 6.1 h, 25 % at 12.2 h, **22 % at off** | 50 % at ~6.5 h, **19 % at off** |
| Voltage at off | 3.40 V (kBatEmptyV) | 3.40 V |
| Soak messages | ~61 sent, 0 send failures | ~56 sent, 0 send failures |
| Restarts / heap | none (one boot) / internal 33-34 KB, min 28 KB, steady | none / 34 KB, min 29 KB |

- About **1.9 Ah/day** at this draw: a full battery lasts **~13 h** with WiFi joined and the ATAK feed on,
  in any mode (WiFi keeps the CPU awake). The firmware's new Power-page figure for this case
  (Balanced + WiFi, 89 mA, "~16-19 h" on the nominal capacity) is somewhat optimistic: the
  usable capacity to the 3.40 V cut-off is ~1.2 Ah, not 1.45.
- The gauge reads ~20 % at the cut-off: it tracked well down to ~25 %, then the cell reached
  3.40 V under ~95 mA. Either the cut-off could go a little lower (3.30 V would give maybe
  20-40 min more; brownout margin to check) or the % shown should be scaled so 3.40 V reads 0 %.
  Not changed yet: owner's call.
- Every 5-min status report and every "dark" report arrived at the listener; the reports
  repeated "dark" 2-3 times while the voltage hovered at 3.40 V (fine).
- Next: run 2 with WiFi off (plain Balanced), then Eco, to measure the modes the docs still
  mark "not measured yet".

20:30 check: both still draw ~87 mA (100 → 86 % in 3 h, about 22 h projected). Cause: both were
still joined to the WiFi with the ATAK feed on (left from the ATAK test), and WiFi keeps the CPU
awake. So run 1 measures **Balanced + WiFi + ATAK feed** (the worst case); a run with WiFi off
is still needed for the plain Balanced figure.

### 2026-11-23 night: ATAK feed over Bluetooth; big buffers to PSRAM

- New: ATAK link to phone = Bluetooth (BLE, NimBLE, paired with a passkey, encrypted) plus the
  Android bridge app (`tools/android/atak-bridge`, `firmware/RatSpeak-ATAK-bridge.apk`).
- COM9 found: with Bluetooth on, WiFi starting next to it left **288 bytes** of internal RAM,
  and Bluetooth alone left 6 KB (NimBLE + controller take ~38 KB internal). Fixes: WiFi stays
  off while the feed uses Bluetooth; the largest scratch buffers (console tile bench 8 KB,
  track 8 KB, Home Assistant line 4.5 KB, Library 7 KB, Bluetooth receive 3 KB, others) moved
  to PSRAM. Static internal RAM 234 KB → 196 KB; free internal RAM before Bluetooth 46 → 84 KB,
  with Bluetooth advertising 44.7 KB (more than the WiFi link had before). The WiFi link gains
  the same 38 KB.
- Simulator: `atak_ble.py` 5/5 (whole valid events to the "phone", ATAK unit + marker in,
  broken / NUL / oversized input dropped without blocking the next event, back to WiFi turns
  Bluetooth off); full regression with it: all pass.
- Not yet: pairing and traffic with a real Android phone and ATAK; battery with Bluetooth on
  (it blocks light sleep like WiFi: the SDK's controller has no modem sleep).
