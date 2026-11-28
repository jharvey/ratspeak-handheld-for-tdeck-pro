# RatSpeak T-Deck Pro: how to

Short, step-by-step guides for everyday use. Each one has a short video made with the
simulator (`docs/videos/`): a hand points at every key as it is pressed, the key's name
shows above the keyboard, and you hear a click.

A 90-second tour of everything: [Overview.mp4](videos/Overview.mp4) (clips from the videos below,
joined by ffmpeg; no sound).

Every screen, one picture each with the keys that reach it: [screens/index.md](screens/index.md) (also [index.html](screens/index.html), and a 3-minute walk through all of them: [Screen_catalog.mp4](videos/Screen_catalog.mp4); remade with `tools/howto/make_catalog.py`).

Keys as of 28 Nov 2026.

## The keys you need

| Key | Does |
|---|---|
| **M** | Menu. **M** then **H** = Home, **M** then **S** = Settings |
| **I** | Inbox (your chats) |
| **C** | Contacts. **G** on Contacts = Groups |
| **P** | Peers (every radio heard). **R** on Peers = announce this radio |
| **A** | Atlas, the offline map. **J** on the Atlas = markers, **Alt+J** = new marker, **O** = map layers |
| **T** / **V** | Up / down in lists |
| **Enter** | Open, send, save |
| **^** (Shift, tapped alone) | Back |
| **Alt** + letter | Small letter (the keyboard types capitals) |
| **SYM** held + key | Digits and symbols: **W E R** = 1 2 3, **S D F** = 4 5 6, **Z X C** = 7 8 9, **MIC** = 0 |
| **MIC** | Full screen refresh. With Home Assistant voice on: talk to Home Assistant (or use a wake word: S.3.7 row 7) |

Quiet hours (no message sound or vibration at night): **M**, **S**, Alerts page, last row.

Numbers pick a row: in Settings the number opens that row at once; in lists it selects
the row, then **Enter** opens it. While you type a message the letter keys type letters:
press **^** first to use them as keys again.

---

## Send a message

Video: [send_message.mp4](videos/send_message.mp4)

1. Press **I** to open the Inbox. Your chats are at the top.
2. Pick the chat: **T** / **V**, or hold **SYM** and type its number.
3. Press **Enter** to open the chat.
4. Type your message. The keyboard types capitals, which is fine; **Alt** + key gives a
   small letter, **SYM** + key digits and symbols. The typing box grows to three lines as
   you write; a longer message shows its last three lines.
5. Press **Enter** to send. "delivered" shows when the other radio got it.
6. **^** goes back to the list.

If the other radio is out of range the message waits in a queue and goes out by itself
when that radio is heard again.

Quick reply: in a chat, **MIC** then **Q** shows eight short answers (OK, Yes, No, On my
way, Where are you?, Call me, I'm safe, Wait for me). Its number sends it at once, any other
key closes the list. In a group chat **MIC** shows who got the last message; **Q** works there
too.

## SOS

Video: [SOS_and_quick_replies.mp4](videos/SOS_and_quick_replies.mp4) (two radios, side by side)

Hold the **top side button** for 3 seconds. The radio buzzes and counts down 5 seconds
("SOS in 5 s - any key cancels"); any key or a short press of the button stops it. Then it
sends "SOS! <your name> needs help." with your position (GPS, the last fix or the saved
position) and battery to all your saved contacts (at most 8), or to one group: **M**, **S**,
Alerts, row 7 "SOS sends to". After that it sends a position update every 2 minutes and the
full SOS every 10 minutes, also after a restart, until you hold the button 3 seconds again:
that sends "SOS cancelled".

A radio that gets an SOS wakes up and sounds an alarm every 3 seconds for a minute, loud,
even in quiet hours or with the beep off. Any key silences it; the sender shows on the map.

A short press of the top button still locks the radio (it now acts when you let go). To
turn the SOS hold off: Alerts row 8 "SOS button (hold 3 s)"; a long hold is then just a press.
An SOS goes to the front of the send queue; in Check-in mode LoRa turns on for it instead of
waiting for the next window.

## Read a message

Video: [read_message.mp4](videos/read_message.mp4)

1. The idle screen shows how many messages are new: "1 NEW" at the right of its top
   banner (the banner also shows the battery, the time and the radio's name, on every idle
   screen). Press any key to wake the radio.
2. Press **I** for the Inbox. Chats with new messages are marked.
3. Pick the chat and press **Enter**. The newest message is at the bottom; **T** / **V**
   scroll through older ones.
4. Type an answer and **Enter**, or **^** to go back.

## Add a contact

Video: [add_contact.mp4](videos/add_contact.mp4)

New radios are not saved by themselves: you save one by giving it a name.

1. Both radios announce once: **M**, **H** (Home), then **N** (Announce ID).
   (Or **P** for Peers, then **R**.) Do it on both radios.
2. Press **P** for Peers: every radio heard, with its signal and how long ago.
3. Pick the new radio (**T** / **V**) and press **Enter**. Its contact page opens:
   1 Message, 2 Rename, 3 Share, 4 Map number.
4. Press **2** (hold **SYM**, press **E**), type a name, **Enter**.
5. It is now in Contacts (**C**). Peers marks saved radios with \*.

To name your own radio: **C**, row 0 (MyID), **Enter**, **2**, type, **Enter**.

## Swap contacts (send a contact card)

Video: [contact_exchange.mp4](videos/contact_exchange.mp4) (Jack and Jill, side by side)

Send your own address (or a contact's) to another radio so they can message you.

1. Press **C** for Contacts. Row 0 (MyID) is your own card: **Enter**.
   (Any other contact sends theirs.)
2. Press **3** (Share): hold **SYM**, press **R**.
3. The list reads "Send *name* to:". Pick who gets it and press **Enter**. Someone who is
   not a contact yet: **P** switches to Peers, where every radio that announced is listed.
   **^** cancels.

The other radio shows **[card] name** in the chat; **Enter** on the empty message line
saves it as a contact. They can send their card back the same way. Other LXMF apps see
"Contact card: name" and the address.

## Groups: one message to several contacts

Video: [Group_messages.mp4](videos/Group_messages.mp4) (two radios, side by side)

Full details: [groups_report.md](groups_report.md).

A group is a name and up to 8 of your contacts, for example "FAMILY". A message to the
group goes to every member, one after the other (about 4 seconds each on LoRa).

**Make a group**

1. Press **C** for Contacts, then **G** for Groups.
2. Pick **New group** (or press **N**), **Enter**. Type a name (up to 16 letters), **Enter**.
3. The list of your contacts opens. **Enter** on a contact puts it in the group, marked
   **[x]**; **Enter** again takes it out. **^** when done.

**Send to a group**

1. **C**, **G**, pick the group, **Enter**, then **1** (Message). Or press **I**: groups
   are the first rows of the Inbox.
2. Type and press **Enter**. Under your message the chat shows how many members have it:
   "2/3 delivered", "1 to send" (still going out), "1 no reply" (out of range or off).
3. **MIC** shows who got your last message, member by member. Any key goes back.

Messages from the others show as "ALEX: text" in the group chat.

**Give the group to its members**

Until a member has the group, your group messages arrive in their chat with you, marked
**[FAMILY]**, and their answer comes back to you alone.

1. **C**, **G**, pick the group, **Enter**, then **4** (Share).
2. Pick a member, **Enter**. Do this once for each member.
3. On their radio the chat with you shows **[group] FAMILY 1/3 ...** lines. When all parts
   have arrived, **Enter** on the empty message line joins the group. Members they did not
   know are saved as contacts.

**Change a group**

On the group's page: **2** Members (put contacts in or out), **3** Rename, **5** Delete
(press **5** twice). A change is on your radio only. Share the group again and the others
press **Enter** in your chat to take the new member list.

Other LXMF apps (Sideband, MeshChat) show a group message as a normal message from you.

## Connect to WiFi

Video: [wifi.mp4](videos/wifi.mp4)

1. **M**, **S** (Settings), then **3** (WiFi).
2. Press **3** again: Scan networks.
3. Pick your network by its number.
4. Type the password (**Alt** for small letters, **SYM** for digits; it shows as you
   type it) and press **Enter**.
5. "connected" shows on the WiFi page. The radio joins this network again by itself.

WiFi is optional: messages go over LoRa radio without it. Saved networks: WiFi page, row 4.

Second video: [add_wifi.mp4](videos/add_wifi.mp4): a radio in Minimal mode (which has no WiFi
page) is switched to Typical (**M**, **S**, **1**, **2**) and joins "ARRL-HQ" with the password
HIRAMMAXIM.

## Set the If-found details

Video: [if_found.mp4](videos/if_found.mp4)

What a finder sees on the screen, even with a flat battery (e-ink keeps the picture).

1. **M**, **S**, **4** (Screen & I/O), **2** (Idle splash), **9** (If-found details).
2. Press a row's number, type, **Enter**: 1 Name, 2 Phone, 3-4 Address, 5 Town,
   6 State, 7 ZIP, 8 Country, 9-10 anything else. Fill in only what you want shown.
3. **^** back to Idle splash. Press **8** (Idle screen) until it reads **If found**.
4. Row 10 hides or shows the address lines.

When the radio goes to sleep, the If found screen shows. With a password lock
(Idle splash, row 3) it shows while locked too. The top button on the side locks the radio at
once (with no password it just shows the idle screen); the bottom one restarts it, and a
restart also ends on the idle screen, locked if a password is set.

## Change the operator mode, touch on / off

Video: [mode_touch.mp4](videos/mode_touch.mp4)

1. **M**, **S**, **1** (Mode).
2. Pick: **1** Minimal (only Inbox, Contacts, Settings row 1 and the walkie-talkie: the
   radio wakes to the Inbox), **2** Typical (the features picked on row 5), **3** Everything.
3. **4** turns the touch screen on or off (a new radio starts with it off: keys only).
4. **5** Typical features: turn each one on or off (walkie-talkie, voice messages, Home
   Assistant voice, WiFi settings, Atlas, Library & games, Peers & announce, Advanced).

In Minimal, Settings shows only row 1, so you can always get back: **M**, **S**, **1**, **2**.

## Open the Atlas (offline map)

Video: [atlas.mp4](videos/atlas.mp4); map layers: [Map_layers.mp4](videos/Map_layers.mp4)

The Atlas needs map files on the SD card; without them the menu has no Atlas.

1. Press **A** on any screen (or **M**, then **A**). The map opens on your position.
   (On a list row of someone with a known position, **A** opens the map at that person.)
2. **K** shows the card with all map keys; **K** again hides it.
3. **T F V H** move the map up, left, down, right. **W** zooms out (wider), **Z** zooms in.
4. **D** (or **Enter**) centres it on you again.
5. **L** locates a place: type the start of its name, **Enter**, then its number
   (**SYM** + **W** = 1) jumps there. **Alt** + **P** right away saves it as a waypoint.
6. **G** shows the heading and distance from you to it (or to the selected contact);
   **G** again goes back to the map.
7. **N** steps through your contacts' last shared positions, newest first.
8. **I** (Inbox), **C** (Contacts) or **^** leave the map.

In the video the radio is at the Alamo in San Antonio; it finds Mount Rushmore (1667 km,
heading 346) and steps through three contacts: at the Vietnam Veterans Memorial in
Washington, hiking in Montana, and at the Cutler VLF station in Maine.

More: the help files on the SD card (Library): Help-6 Map, Help-7 Waypoints.

## Find someone: announce and Peers

Video: [peer_search.mp4](videos/peer_search.mp4)

Announce tells radios nearby who you are, so they can find you and message you. Every radio
that announced and was heard shows in **Peers**, with its signal and how long ago.

1. **M**, then **P**: Peers. **R** announces this radio.
2. When a new radio announces, it shows in the list with its name (press **P** again to
   refresh). **H** shows only the radios heard in the last 10 minutes, 1 hour or 24 hours
   (press again for the next, then all).
3. Pick it and press **Enter**: its contact page. **2** (Rename), type a name, **Enter**:
   it is now a contact.
4. **Enter** again, **1** (Message): type and **Enter** to send.

## A typical check-up

Video: [typical_checkup.mp4](videos/typical_checkup.mp4)

1. The idle screen shows the number of new messages at the top.
2. Any key wakes the radio. With a password lock, type the password and **Enter**.
3. The Inbox shows "1 new" on the chat with the new message: its number (here **3**:
   **SYM** + **R**), then **Enter** opens it.
4. Type the answer (digits: hold **SYM**) and **Enter**.
5. **^** back. After a while the radio sleeps (and locks) by itself.

## Lock the radio with a password

Video: [Password_lock_and_unlock.mp4](videos/Password_lock_and_unlock.mp4)

1. **M**, **S**, **4** (Screen & I/O), **2** (Idle splash).
2. **3** (On + password): type a new password (4 to 32 characters), **Enter**; type it
   again, **Enter**. **MIC** shows or hides it while you type.
3. **7** (Lock now) locks at once. The radio also locks when it goes to sleep, and asks for
   the password at every start.
4. To unlock: any key, the password, **Enter**.

Messages still arrive while it is locked. After 3 wrong tries you must wait 30 s, then
longer each time. There is no way to read a forgotten password back.

## Announce your radio

Video: [Announce_your_unit.mp4](videos/Announce_your_unit.mp4)

Announce tells the radios nearby who you are, so they can find you and message you.

1. **M**, **H** (Home): your name and your address.
2. **N** (Announce ID) sends them once.
3. Or: **P** (Peers), then **R**.

Announce once when you meet someone new, or when others cannot find you. It is not needed
for every message.

## Charging and the Battery page

Video: [Charging_and_battery.mp4](videos/Charging_and_battery.mp4)

1. Plug a USB-C charger in. The battery icon at the top left shows the charging sign.
2. **M**, **S**, **6** (Battery): voltage, charge in %, the current going in (+) or out (-).
3. **V** goes down: row 9 Full-charge cal, row 11 Time left (measured from your own use).

New radio, or the % looks wrong? Charge it full, use it until it switches itself off, and
charge it full again. After that the % is right.

## Energy modes

Video: [Energy_modes.mp4](videos/Energy_modes.mp4)

How much the radio sleeps. More sleep means more days on one charge.

1. **M**, **S**, **5** (Power). Each row is a mode, with the battery life in it.
2. A row's number picks it: **1** Full power (always awake), **2** Balanced (the everyday
   mode), **3** Eco (like Balanced, but WiFi off and the GPS once an hour), **4** Saver (wakes
   once a minute; LoRa still listens), **5** Ultra-low (LoRa only, wakes every 5 minutes),
   **6** On demand (LoRa only for a while after a key press), **7** Check-in (LoRa only at set
   times; the row opens its schedule), **8** Custom.
3. Further down: 9 Energy monitor, 10 Full power on USB.

## Low-battery notice

Video: [Low_battery_notice.mp4](videos/Low_battery_notice.mp4) (two radios, side by side)

The radio tells one contact when its battery gets low: the battery % and its last position.

1. **M**, **S**, **6** (Battery). **V** down to row 12, Low-battery notice, **Enter**.
2. **2** (Send to): pick the contact by its number, **Enter**. The bottom line confirms,
   for example "Low battery -> SAGE at 10%".
3. Row 1 of the same page (Send at) changes the % or turns the notice off.

## Share your location

Video: [Share_your_location.mp4](videos/Share_your_location.mp4) (two radios, side by side)

Nothing about where you are, or your battery, is sent unless you send it. Automatic sharing
is off until you turn it on.

1. Open the chat with the person: **C**, their number, **Enter**, **1** (Message).
2. **MIC** opens the share menu. **1** sends your position and battery % once.
3. **B** in that menu changes what is shared: position + battery, position only, battery
   only. Rows 1 and 2 show the choice.
4. **MIC**, **2** (Auto-share): every 5 minutes; **2** again: 15, 30, 60 minutes, off. Row 3
   sets for how long. While it is on, the chat's top line shows a mark such as
   **[loc+bat 5m]** and the clock box has a small pin.
5. To a group: open the group chat, **MIC**, then **L**. Rows 1 to 3 and **B** work the same;
   every member gets a copy.
6. To stop all sharing: **M**, **S**, **2** (Time & Location), row 8.

Automatic sharing sends only when it is worth it: when you moved more than 50 m or the
battery changed by 5 points, and otherwise at every second interval. With no GPS fix (GPS
off, standby, a power mode without GPS) the battery goes alone, and the other radio keeps
your last position with its age.

The other radio shows **[loc]** or **[bat]** in the chat for a one-time share; **Enter** on
the empty message line opens the map there. Automatic updates make no sound and show as one
line (the newest). Your position is on their Atlas; your battery % and its age are on your
contact page (row 1) and in their share menu (row 4). Sideband and other LXMF apps show a
one-time share as readable text; Sideband also gets it as telemetry.

## Markers (items of interest)

Video: [Markers.mp4](videos/Markers.mp4) (two radios, side by side)

Mark a place with a type and a short note (a hazard, a camp, water, a meet-up) and send it.

1. **A** (Atlas), **J**: the marker list, nearest first. The last row is **New marker**,
   **Enter**. (**Alt+J** on the map makes one at the map cross.)
2. The form: **1** type (Enter steps through them), **2** where (my position or the map
   cross), **3** distance, **4** direction, **5** a note (type it, **Enter**), **6** save.
3. On the marker's page: **1** show it on the map (a triangle), **2** send it (pick the
   contact or group, **Enter**), **3** twice removes it.
4. A marker you get shows in the chat as "[marker] hazard 640 m NE of you: bridge out".
   **Enter** on the empty message line opens the map on it.
5. **J** in the marker list shows the contacts who shared a position, nearest first.

ATAK: phones running ATAK can see this unit, its contacts and the markers:
**M**, **S**, **3** (WiFi), row 9 "ATAK feed (CoT)", **Enter** turns it on.
The next row, "ATAK link to phone", switches between WiFi and Bluetooth (switching to
Bluetooth restarts the radio once). Bluetooth needs the Android app `firmware/RatSpeak-ATAK-bridge.apk`: scan, tap the radio and
type the 6-digit PIN the radio shows (see tools/android/atak-bridge/README.md).
Video: [ATAK_over_Bluetooth.mp4](videos/ATAK_over_Bluetooth.mp4).

## Waypoints

Video: [Atlas_waypoints.mp4](videos/Atlas_waypoints.mp4)

1. On the Atlas (**A**), bring the place to the middle of the screen (**Z** zooms in,
   **T F V H** move).
2. **Alt** + **P**: type a name, **Enter**. The waypoint is saved.
3. **P** steps through your waypoints; the bottom line shows the name, and how far and which
   way it is from you.
4. **Alt** + **SYM**, twice, removes the selected one.
5. The list: **M**, **S**, **2** (Time & Location), **9** (Waypoints).

## Breadcrumb track

Video: [Atlas_breadcrumb_track.mp4](videos/Atlas_breadcrumb_track.mp4)

The radio remembers the way you walked, so you can find the way back.

1. **M**, **S**, **2** (Time & Location), **0** (row 10, Track), **1** (Record track).
2. Walk. The radio drops a point about every 25 m; the Atlas shows them as a dotted line.
3. On the Atlas, **B** is track back: the top line shows the distance to the next point.
   **G** shows a big arrow; **B** again stops it.
4. On the Track page, **3** (Export as GPX) saves the track on the SD card; **4** clears it
   (**Enter** twice).

## Navigate to a contact

Video: [Navigate_to_a_contact.mp4](videos/Navigate_to_a_contact.mp4)

1. On the Atlas (**A**), **N** steps through your contacts' last shared positions.
2. **G** (go): a big arrow, the distance, the direction, and how old their position is.
3. **G** or **^** goes back to the map; **D** centres it on you.

North is at the top. The radio has no compass: use the direction with a landmark, the sun
or a compass.

## Arrival alerts

Video: [Arrival_alerts.mp4](videos/Arrival_alerts.mp4)

Tells you when a contact comes near you, or near one of your waypoints. They must be sharing
their location with you.

1. **M**, **S**, **9** (Alerts), **5** (Arrival alerts), **1** (New alert).
2. Row 1 Contact, row 2 Near (me, or a waypoint), row 3 Within (the distance), row 4 Repeat
   (once, or every time): press a row's number until it is right.
3. **5** (Save alert). **Enter** twice on an alert in the list removes it.

## Back up to the SD card

Video: [SD_backup_and_restore.mp4](videos/SD_backup_and_restore.mp4)

1. **M**, **S**, **V** down to row 11 (Storage), **Enter**, then **5** (SD card).
2. **8** (Back up now): messages, contacts, settings and your identity go to the card.
3. **9** (Restore backup) lists the backups; **Enter** twice on one restores it (the radio
   restarts as it was at that time).
4. Only the contacts: **M**, **S**, **7** (Identity), Export contacts (SD) / Import contacts
   (SD).

Keep the card safe: whoever has the backup can be you.

## Update the firmware from the SD card

Video: [Firmware_update_from_SD.mp4](videos/Firmware_update_from_SD.mp4)

1. On a PC, copy the new `.bin` file into the folder `firmware` on the SD card.
2. Battery at 30 % or more, or on a charger. **M**, **S**, **V** down to row 11 (Storage),
   **Enter**, **5** (SD card).
3. **0** (row 10, Update firmware), then **Enter** again to install. It takes about half a
   minute, then the radio restarts. Do not switch it off meanwhile.

If the new firmware does not start properly, the radio goes back to the old one by itself.

## Range test

Video: [Range_test.mp4](videos/Range_test.mp4)

1. **M**, **S**, **8** (LoRa). (In Typical mode, turn the Advanced feature on first:
   Settings, **1**, **5**.)
2. **6** (Range test): pick the other radio by its number, **Enter**.
3. The screen counts the pings that came back, with signal strength and quality, and the
   lost ones. With an SD card in, each ping is also written to a file.
4. **^** stops the test.

## Library and games

Video: [Library_and_games.mp4](videos/Library_and_games.mp4); notes:
[Notes_in_the_Library.mp4](videos/Notes_in_the_Library.mp4)

1. **M**, **R** (Library): the text files on the SD card, here the help pages.
2. A file's number opens it. **Space** or **N** next page, **B** back a page, **L** finds a
   word, **K** shows all the reader keys, **^** back to the list.
3. The last rows are the games and **New note**: type the note, **Enter** saves it on the SD
   card as Note-<date>-<time>.txt, which then opens like any other file.
4. The games. Rat Trap: **T F V H** move, **D** or **Enter** opens a
   square. Chess: **Enter** picks a piece up and puts it down, **U** takes a move back,
   **L** sets the level.

## A tour of Settings

Video: [Settings_tour.mp4](videos/Settings_tour.mp4)

One pass through every Settings row: 1 Mode, 2 Time & Location, 3 WiFi, 4 Screen & I/O,
5 Power, 6 Battery, 7 Identity, 8 LoRa, 9 Alerts, 10 Diagnostics, 11 Storage. Rows 10 and
up take both digits: **1** **0** opens row 10, **1** **1** row 11 (**0** alone is row 10 too).

---

## Walkie-talkie (live talk)

Video: [walkie_talkie.mp4](videos/walkie_talkie.mp4)

1. **C**, pick the person, **Enter** (their card), **W** = call.
2. On the same WiFi the call starts at once; with no shared WiFi the invite goes over LoRa and
   your unit opens a small hotspot the other one joins.
3. **Space** = talk / listen, **^** = hang up. The other person answers with **Enter**.

More on the card: `library/Help-11-Walkie-talkie.txt`.

## Voice message

Video: [voice_message.mp4](videos/voice_message.mp4)

1. Open the chat, **MIC**, **5** (Voice message).
2. **Space** = start recording, **Space** again = stop and send (up to 20 s). **^** = cancel.
3. To listen: **MIC**, **5**, **P** plays the newest one.

More on the card: `library/Help-12-Voice-messages.txt`.

## Check the GPS

1. **M**, **S**, **2** (Time & Location): row 3 shows the fix level and the error, e.g.
   "ON  good ~12 m  9 sats". **good** = within about 30 m, **fair** = up to 300 m, **rough** =
   worse (up to about a mile), **TIME ONLY** = no position yet, but the clock is set.
2. To watch the satellites live: **M**, **S**, **1** **0** (Diagnostics), **1** **0** (GPS
   signals). One bar per satellite, taller = stronger, grouped by system. About 25 is enough
   for a fix; under 15 is weak. It redraws every 2 seconds and keeps the radio awake while open.
3. The radio keeps its last position across a restart (shown with its age) and gives the GPS
   what it learned about the satellites, so the next fix comes sooner.

**Field log:** Diagnostics, row 11 (on by default except in Minimal). A line a minute on the
SD card in `ratdeckpro/field.log`: fix level, satellites and their strength, position, USB or
battery, battery %. Read it on a PC after a drive or a walk.

## Battery empty

Video: [Battery_empty_wont_turn_on.mp4](videos/Battery_empty_wont_turn_on.mp4)

When the battery runs out the unit shows its idle screen (If found ...) with
**BATTERY EMPTY / PLUG IN A USB CHARGER** and switches itself off. It stays off until USB
is plugged in; then it starts with **BOOTING v0.1** at the bottom of the idle screen.

## Making the videos again

`tools/howto/make_videos.ps1` runs the scripts in `sim/scripts/howto/` on the simulator
and turns each recording into an MP4 with `tools/howto/make_video.py` (hand pointer,
key names, captions, note boxes, click sounds). To point at something on the screen a script
says `point <x> <y> <ms> <w> <h>` (the item's centre and size in screen pixels): the video
shows a blinking box around it with the hand beside it. Digits and small letters are shown the way a
hand types them: one hand holds **SYM** or **Alt** (half click down, half click up), the
other presses the key (full click). See the top of both files.

Captions above the device sit on a paper-coloured banner (dark brown bold text) that flashes
brighter and grows in whenever the caption changes; note boxes over the screen (yellow) pop in
the same way. A caption stays long enough to be read, and a note box for its whole time, before
a quiet stretch is shortened.

With the repo on the NAS (M:), Windows will not run the simulator from the share ("Access is
denied"): build it to C: (`C:\Users\MyPC\simrun\build_simb.bat`), put Qt's `mingw_64\bin` and
`Tools\mingw1310_64\bin` on PATH, and pass `-SimExe C:\Users\MyPC\simrun\simb\ratspeak-sim.exe
-Seed C:\Users\MyPC\simrun\sim2\build\howto_seed`.

`-DataRoot <folder>` puts the simulated radios' folders (and the recorded frames) there instead
of next to the simulator, for example on M: when C: is nearly full. The seed folder holds what goes
on a radio's SD card: `atlas` (map tiles and place names), `library` (the help files of
`tools/sdcard/library` in `ratdeckpro/library`) and `fw` (a firmware `.bin` in `firmware`).
