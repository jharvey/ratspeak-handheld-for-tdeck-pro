# Status sharing and ATAK-style markers: report

State on 2026-11-23: Part 1 (status sharing) passes every simulator test, including the
three late edits; firmware built on VM100, no unit flashed yet. Part 2 (markers) is
drafted but not in the tree.

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

- This radio does not read Sideband's telemetry field. It reads the text.
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

## Part 2: markers (items of interest): NOT in the tree

Drafted, never compiled. The draft is outside the repo, in the sessions' shared notes
folder, subfolder `sharing-atak-part2-draft`:

- `marks_block.cpp`: the marker code (data, send, receive, pages, console).
- `p2.py`: a patch script that puts the block and its hooks into `StandaloneApp.cpp`.
- `mapview_p2.patch`: the triangle marker for `MapView.cpp` and `MapView.h` (`git apply`).
- `marks.txt`, `marks_after.txt`, `marks_net.py`: the tests, to go into `sim\scripts`.

Design: Atlas **J** lists markers nearest first, **Alt+J** makes one at the map cross or by
distance and direction from my position, with a type and a note. It is sent to a contact or
a group as `Marker: hazard at 43.64012, -70.92011 (bridge out)`. The receiver stores it,
draws a triangle on the Atlas and shows `[marker] hazard 640 m NE of you: bridge out` in the
chat. **J** in the list shows contacts by distance with direction.

Not started: the docs for part 2, the WAITING ON ATAK video entries, the gateway to phones
running ATAK.

## Next steps

1. Flash the units with the new image (none flashed yet).
2. Apply the part 2 draft, build, test, write its docs, build the firmware again.

## Decisions for the owner

- Keep "send at every second interval when nothing changed", or send less often?
- Is 50 m and 5 battery points the right threshold?
- Should a one-time share default to position + battery (as built) or position only?
