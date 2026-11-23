# Status sharing and ATAK-style markers: report

State on 2026-11-23: Part 1 (status sharing) passes every simulator test and is on all five
units (image `92c596b`). Part 2 (markers) and part 3 (ATAK feed over WiFi) are built and
pass their simulator tests; not tried on real radios or with a real ATAK phone.

## Part 1: share position and battery with a contact or a group

### What was built

- One-time share of position, battery % or both, to a contact or to a group.
- Automatic sharing per contact or group, with the same choice. It is off by default. When
  switched on it starts with position + battery.
- The receiving radio keeps each contact's newest position and battery % with their age.
  The position is on the Atlas. The battery % and its age are on the contact page (row 1)
  and in the share menu (row 4). The position's age is on the contact page (row 4).
- An automatic update makes no sound on the receiver. In a contact's chat the updates fold
  into one line (the newest). In a group chat they are left out. When nothing else is
  unread, the update is marked read at once so the new-message count stays right.
- A "sharing on" mark on the sender: the chat's top line shows `[loc+bat 5m]`, `[loc 5m]` or
  `[bat 5m]`, and the clock box has the small pin as before.

### How to use it

1. Open a chat, press **MIC**. Row 1 sends once. Row 2 turns automatic sharing on (5, 15,
   30, 60 minutes, off). Row 3 sets for how long.
2. **B** in that menu changes what is shared: position + battery, position, battery.
3. In a group chat: **MIC**, then **L**. Rows 1 to 3 and **B** work the same.
4. Stop everything: **M**, **S**, **2**, row 8.

Keys 1 to 5 of the menu mean what they meant before, so the existing videos still match.

### Choices made

- **When an automatic update goes out.** One is due every interval. It is sent when the
  radio moved more than 50 m or the battery changed by 5 points or more. With no change it
  is sent at every second interval.
- **No GPS fix.** In standby, in a power mode without GPS, or with GPS off, the battery goes
  alone. The receiver keeps the last position and shows its age. With a fix expected but not
  found in 3 minutes, the battery also goes alone. "Position only" then sends nothing.
- **Check-in mode.** Updates wait for the check-in window, as location shares did before.
- **Sleeping modes.** The radio sends when it is awake. In standby it wakes every 5 minutes,
  so an update can be up to one sleep period late.
- **Message format.** Text any LXMF client can read:
  `My location: 43.64012, -70.92011 (+-10 m, 14:32). Battery 84%`, `My battery: 84% (14:32)`,
  and for automatic updates `Loc: 43.64012, -70.92011 14:32 Bat 84%` or `Bat: 84% 14:32`.
- **Sideband.** A one-time share also carries Sideband's telemetry field (LXMF field 0x02:
  time, location, battery). The bytes were decoded with Sideband's own unpack rules and gave
  back the right values. It was not tried with a real Sideband.
- **Storage.** Saved automatic shares and contact positions from older firmware still load.

### Limits

- Since 23 Nov 2026 this radio also reads Sideband's telemetry field (FIELD_TELEMETRY 0x02:
  position, battery) on any incoming message, so Sideband users show on the Atlas and the
  contact page without position text (`sim/scripts/telemetry_net.py`, bench `telesend`). Not
  yet tried with a real Sideband.
- Each automatic update is still a stored message on both radios. The message store cannot
  delete one message.
- A copy that waits in the outbox goes without the telemetry field. The text is the same.
- At most 8 automatic shares.

### Tests

Simulator build `C:\Users\MyPC\simrun\simb` of `8a5e56d` (with the three late edits), 2026-11-23.

| Test | Result |
|---|---|
| `sim\scripts\status.txt` (menu, B choice, automatic on, the mark, group menu) | PASS |
| `sim\scripts\status_after.txt` (a restart keeps both automatic shares) | PASS |
| `sim\scripts\status_net.py` steps 1 to 11 (three radios: off by default; one-time position, battery, both; automatic; change threshold; battery only; position only; standby; group; restart; receiver's data) | PASS (11 of 11) |
| groups, groups_after, lock, lock_boot, lock_button, lock_button_boot, standby, standby_after, idle_banner, idle_map, `groups_net.py`, `standby_pair.py`, `monkey.py` 2 rounds | PASS |
| `loc_pair.py` | NOT RUN: it only runs `sim\build`, an older build |
| Firmware build on VM100 | see the firmware commit after this one; no unit flashed |

Step 8 showed one real fault in the first run: in standby only one share was served per
wake, so a second contact waited 5 more minutes. The fix (all due shares in one pass) now
passes step 8.

**Three small edits made after the first simulator build, now compiled and tested:** the fix above; a battery-only chat row showing `[bat] 61%` and not the raw text;
one comment.

## Part 2: markers (items of interest)

Built 2026-11-23 from the part 2 draft. Only two draft files survived (the marker code and
its patch script, now kept in `backups/atak_p2_draft`); the map triangle and the three
tests were lost with the old notes folder and were written again.

### What was built

- A marker is a place with a type (hazard, person, vehicle, camp, water, meet-up, other) and
  a note of up to 23 characters. 24 are kept (NVS `rs_mark`); when full, a new one replaces
  the oldest received one, or the oldest when all are your own.
- Atlas **J**: the marker list, nearest first, with distance, direction, note, maker and age.
  The last row makes a new one. **Alt+J**: a new marker at the map cross.
- The form: **1** type, **2** where (map cross, or from my position), **3** distance,
  **4** direction (N, NE, E ...), **5** note, **6** save.
- One marker: **1** show it on the map, **2** send it to a contact or a group (groups
  first), **3** twice within 3 s removes it.
- On the map a marker is a black triangle in a white halo. Selected: hollow with a ring, a
  dashed line from you, and the bottom line `water 299 m E spring`.
- **J** in the list: the contacts who shared a position, nearest first. **Enter** shows one
  on the map.
- Sent as text any LXMF client reads: `Marker: hazard at 43.64012, -70.92011 (bridge out)`.
  The receiver keeps it and draws it. The chat shows
  `[marker] hazard 640 m NE of you: bridge out`, measured from the receiver's own position.
  **Enter** on the empty draft opens the map on it.
- The same marker sent again (same maker, same type, within 15 m) updates the note and
  adds no copy.
- The Atlas keys card now reads `P Alt+P wpt, add` and `J Alt+J marker, add`.
- Console: `mark`, `mark new <type> <lat> <lon> [note]`, `mark rel <type> <metres>
  <degrees> [note]`, `mark send <n> <hex32|g<n>>`, `mark del <n|all>`, `mark list`,
  `mark near`.

### Tests (simulator build of this commit, 2026-11-23)

| Test | Result |
|---|---|
| `sim\scripts\marks.txt` (Atlas J, one marker, show on map, Alt+J form, save, nearby, 8 screenshots) | PASS |
| `sim\scripts\marks_after.txt` (a restart keeps the markers) | PASS |
| `sim\scripts\marks_net.py` (three radios: make, send to a contact, chat row, update, send to a group, the pages, restart, full) | PASS (8 of 8) |
| Real radios, image `04c172f` (2026-11-23 02:50): COM9 made a marker and sent it to COM6 over LoRa | PASS: COM6 stored it (`[MARK] #1 hazard ... from 3bb4b09700e7`); COM6 had no GPS fix, so it showed the coordinates instead of a distance |

Seen on COM9 during that test, not caused by markers but worth a look: free heap 18 KB
(lowest 6.9 KB), `announce-order persist/verify FAILED` on every announce (flash 81% full),
and after each flash the first boot spends 98 s on a full message scan
(`no saved record index`: the index is only written 30 s after a message change).
Opening the USB serial port also restarts these units, so a console query during that scan
starts it again.

## Part 3: ATAK feed (Cursor-on-Target over WiFi)

Built 2026-11-23. Phones running ATAK (also WinTAK, iTAK) see the units without a TAK
server.

- **S, 3 (WiFi), last row "ATAK feed (CoT)"**: off by default. When it is on and WiFi is up,
  every 30 s the unit sends CoT XML events over UDP to ATAK's default "SA" multicast group,
  239.2.3.1:6969. ATAK on the same WiFi listens there out of the box.
- What goes out:
  - the unit itself, as a friendly ground unit (`a-f-G-U-C`) with its name and battery;
  - each contact's last shared position, until it is an hour old (same type, their
    battery when known);
  - each marker, as a spot on ATAK's map (`b-m-p-s-m`), named `hazard: bridge out` and
    coloured by type (hazard red, person cyan, vehicle blue, camp green, water light blue,
    meet-up yellow, other white).
- Each item keeps a stable uid (`RatSpeak-<first 16 of its address>`, `RatSpeak-mk-<hash>`),
  so ATAK moves the same icon and does not add a new one.
- Console: `cot`, `cot on|off`, `cot to <a.b.c.d> [port]` (a phone's own address),
  `cot to default`, `cot every <s>`, `cot now`, `cot listen <port|0>` (bench: print the events
  that arrive). `wifi join <ssid> [password]` was added for tests.
- Code: `src/boards/tdeck_Pro/CotFeed.cpp/.h` (XML and the UDP socket, kept apart because
  the simulator's socket shim defines `bind` and `close` as macros), the rest in
  `StandaloneApp.cpp` ("ATAK feed").

### Tests

| Test | Result |
|---|---|
| `sim\scripts\cot_net.py` (two radios on the simulated WiFi: off by default; `cot now` sends self, contact and marker as well-formed XML with the right type, callsign, point, battery, colour and escaping; on = every interval, off = stops; restart keeps the settings) | PASS (4 of 4) |

The simulated WiFi only delivers to one unit's address, so the test sends to the listener's
address. Multicast to 239.2.3.1 was not tried (it needs real WiFi). **Not tried with a real
ATAK phone.**

### Real units, 23 Nov 2026 (stress run, all five on image `04c172f`)

COM3, COM5, COM8 on USB and WiFi, COM6 and COM9 on the air; 20 rounds of 45 s: each USB
unit made a marker and sent it to the next unit, to COM6 / COM9, or to a group of all five;
the ATAK feed every 10 s; the PC listened on 239.2.3.1:6969 (`M:
atspeak-soaktak_stress.py`,
logs in `M:
atspeak-soak6-11-23_atak`).

| What | Result |
|---|---|
| Markers received on USB units | 5 of 46. Not a marker fault: COM3's message store is 94 % full and COM5's 92 % (soak test data), so they refuse every new message, sent or received ("flash full: new message refused"), and their outboxes filled ("outbox full", "radio busy"). COM8 (83 %) sent all 19 of its markers; COM3 did not take them in |
| ATAK feed sent | COM3 1528 events, COM5 1410 (sendto succeeded) |
| ATAK feed on the PC | 0 events, also 0 when COM3 sent straight to the PC's address. The PC is on WiFi with the network set to Public in Windows, which blocks incoming UDP for programs without a rule. Not a feed fault as far as this shows; not yet seen by any receiver on real WiFi |
| Heap (heartbeat every 30 s) | COM3 38.5-44.8 KB (lowest internal 33 KB), COM5 37.4-42.6 KB (28 KB), COM8 22.4-30.1 KB (16 KB). No restarts |
| COM8 | Its feed sent 9 events and then none; its console was blocked up to 16 s at a time (`[SLOW] pollSerial 16450 ms`): to look into |

To run it again usefully: free the message stores on COM3 and COM5 (or use units that are not
full), and let the PC receive UDP 6969 (network set to Private plus a rule for Python, or
listen on a phone running ATAK instead).

### Receive (added 23 Nov 2026)

While the feed is on (or the bench listener `cot listen <port>`), the unit also listens on
UDP 6969 and joins 239.2.3.1 where the network stack has multicast (not in the simulator).

- A phone's own position (CoT type `a-...`, not hostile) is kept per uid, 16 at most, and
  drawn on the Atlas as a hollow square; it goes when its stale time passes or after an
  hour. Console `cot atak` lists them.
- Its markers (`b-m-p...`, `u-d-p`, hostile `a-h-...`) become markers "from ATAK": the type
  from a callsign like "camp: river camp" (else hazard for hostile, other for the rest), the
  rest as the note. The same marker again (same uid, type, note, within 15 m) changes
  nothing, so a feed repeating every few seconds does not wear the flash.
- Events from RatSpeak units (uid `RatSpeak-...`) are skipped, and markers that came from
  ATAK are not sent back out on the feed.
- Bench: `cot fake <ip|local> <u|m> <callsign> <lat> <lon>` sends (or feeds locally) an event
  the way a phone running ATAK does. Tests: `cot_net.py` step 4, `sim/scripts/atak_map.txt`.

### Not done

- A TAK server connection (TCP / TLS, port 8087 / 8089).
- A TAK server connection (TCP / TLS, port 8087 / 8089).
- The feed over LoRa (only WiFi; a unit without WiFi is seen through a unit that has it,
  because contacts' positions are passed on).

## Next steps

1. Flash COM3 and COM5 with `04c172f` (COM6, COM8, COM9 have it).
2. Try the ATAK feed with a phone running ATAK on the same WiFi (multicast 239.2.3.1:6969).
3. Receive from ATAK: phones' positions and their markers onto the Atlas.
4. Videos for the share menu, markers and the ATAK feed (videos are on hold).

## Decisions for the owner

- Keep "send at every second interval when nothing changed", or send less often?
- Is 50 m and 5 battery points the right threshold?
- Should a one-time share default to position + battery (as built) or position only?
- Markers: are the seven types right? 24 markers and a 23-character note enough?
- ATAK feed: every 30 s to the multicast group (as built), or another interval?

Recommendation for the open sharing choices (kept as built while the owner was away): keep
the keep-alive at every second interval, 50 m and 5 points, and position + battery as the
default; they passed the tests and cost one message per hour at most when nothing changes
on a 30-minute interval.
