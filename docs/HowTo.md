# RatSpeak T-Deck Pro: how to

Short, step-by-step guides for everyday use. Each one has a short video made with the
simulator (`docs/videos/`): a hand points at every key as it is pressed, the key's name
shows above the keyboard, and you hear a click.

Keys as of 14 Nov 2026.

## The keys you need

| Key | Does |
|---|---|
| **M** | Menu. **M** then **H** = Home, **M** then **S** = Settings |
| **I** | Inbox (your chats) |
| **C** | Contacts |
| **P** | Peers (every radio heard). **R** on Peers = announce this radio |
| **A** | Atlas, the offline map |
| **T** / **V** | Up / down in lists |
| **Enter** | Open, send, save |
| **^** (Shift, tapped alone) | Back |
| **Alt** + letter | Small letter (the keyboard types capitals) |
| **SYM** held + key | Digits and symbols: **W E R** = 1 2 3, **S D F** = 4 5 6, **Z X C** = 7 8 9, **MIC** = 0 |
| **MIC** | Full screen refresh. With Home Assistant voice on: talk to Home Assistant |

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
   small letter, **SYM** + key digits and symbols.
5. Press **Enter** to send. "delivered" shows when the other radio got it.
6. **^** goes back to the list.

If the other radio is out of range the message waits in a queue and goes out by itself
when that radio is heard again.

## Read a message

Video: [read_message.mp4](videos/read_message.mp4)

1. The idle screen shows how many messages are new. Press any key to wake the radio.
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

## Connect to WiFi

Video: [wifi.mp4](videos/wifi.mp4)

1. **M**, **S** (Settings), then **3** (WiFi).
2. Press **3** again: Scan networks.
3. Pick your network by its number.
4. Type the password (**Alt** for small letters, **SYM** for digits; **MIC** shows or
   hides it) and press **Enter**.
5. "connected" shows on the WiFi page. The radio joins this network again by itself.

WiFi is optional: messages go over LoRa radio without it. Saved networks: WiFi page, row 4.

Second video: [add_wifi.mp4](videos/add_wifi.mp4): a radio in Typical mode (WiFi settings on;
Minimal mode has no WiFi page) joins "ARRL-HQ" with the password HIRAMMAXIM.

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
3. **4** turns the touch screen off (keys only); **4** again turns it on.
4. **5** Typical features: turn each one on or off (walkie-talkie, voice messages, Home
   Assistant voice, WiFi settings, Atlas, Library & games, Peers & announce, Advanced).

In Minimal, Settings shows only row 1, so you can always get back: **M**, **S**, **1**, **2**.

## Open the Atlas (offline map)

Video: [atlas.mp4](videos/atlas.mp4)

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
2. When a new radio announces, it shows in the list (press **P** again to refresh).
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

---

## Walkie-talkie (live talk)

1. **C**, pick the person, **Enter** (their card), **W** = call.
2. On the same WiFi the call starts at once; with no shared WiFi the invite goes over LoRa and
   your unit opens a small hotspot the other one joins.
3. **Space** = talk / listen, **^** = hang up. The other person answers with **Enter**.

More on the card: `library/Help-11-Walkie-talkie.txt`.

## Voice message

1. Open the chat, **MIC**, **5** (Voice message).
2. **Space** = start recording, **Space** again = stop and send (up to 20 s). **^** = cancel.
3. To listen: **MIC**, **5**, **P** plays the newest one.

More on the card: `library/Help-12-Voice-messages.txt`.

## Battery empty

When the battery runs out the unit shows its idle screen (If found ...) with
**BATTERY EMPTY / PLUG IN A USB CHARGER** and switches itself off. It stays off until USB
is plugged in; then it starts with **BOOTING v0.1** at the bottom of the idle screen.

## Making the videos again

`tools/howto/make_videos.ps1` runs the scripts in `sim/scripts/howto/` on the simulator
and turns each recording into an MP4 with `tools/howto/make_video.py` (hand pointer,
key names, captions, note boxes, click sounds). Digits and small letters are shown the way a
hand types them: one hand holds **SYM** or **Alt** (half click down, half click up), the
other presses the key (full click). See the top of both files.

Captions above the device sit on a deep-blue banner (white bold text) that flashes orange and
grows in whenever the caption changes; note boxes over the screen pop in the same way.

With the repo on the NAS (M:), Windows will not run the simulator from the share ("Access is
denied"): build it to C: (`C:\Users\MyPC\simrun\build_simb.bat`), put Qt's `mingw_64\bin` and
`Tools\mingw1310_64\bin` on PATH, and pass `-SimExe C:\Users\MyPC\simrun\simb\ratspeak-sim.exe
-Seed C:\Users\MyPC\simrun\sim2\build\howto_seed`.
