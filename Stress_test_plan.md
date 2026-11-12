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
- TODO: a random-key script command (`monkey <minutes> <seed>`), repeatable by seed.
- TODO: a sanitizer build option for the simulator.

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
- Map copies and downloads on the PC side resume from the NAS (`M:atspeak-maps\STATUS.txt`).

Tests:
| Test | How | Pass |
|---|---|---|
| **Reset loop while saving** (automated) | PC script runs `msgfill` (or ping-pong traffic) and resets the unit at a random moment through the USB serial lines (the same RTS/DTR reset as opening the port), several hundred times | Every boot succeeds; message count only ever grows by what was confirmed; no broken records; queued messages go out after boot |
| **Real power cut** | Battery-only unit, pull the battery during saves / map copy / settings change (10-20 times by hand) | Same as above; settings unchanged |
| **Simulator kill** | Kill the simulator process at random points during saves, restart, repeat | Same as above, fast enough for hundreds of runs overnight |
| **Brown-out** | Run the battery down under load | Clean stop; normal boot when charged |

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

Open findings:
- A send can block the console (and the UI) for up to ~16-24 s when the radio is busy
  (sendMessage retries "busy" inside the main loop). Look at doing the retry in the
  background with a "sending..." state.
- Under load, retried messages arrive late and the thread shows them in arrival order
  (e.g. "8" below "40"). Consider ordering threads by the sender's timestamp.
