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
4. Type your message. **Alt** + key gives small letters, **SYM** + key digits and symbols.
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

## Send (share) a contact

Video: [share_contact.mp4](videos/share_contact.mp4)

Send someone's address to another person so they can message them.

1. Press **C** for Contacts, pick the contact, **Enter**. (Row 0 MyID sends your own.)
2. Press **3** (Share): hold **SYM**, press **R**.
3. The list reads "Send *name* to:". Pick who gets it and press **Enter**. **^** cancels.

The other radio shows **[card] name** in the chat; **Enter** on the empty message line
saves it as a contact. Other LXMF apps see "Contact card: name" and the address.

## Connect to WiFi

Video: [wifi.mp4](videos/wifi.mp4)

1. **M**, **S** (Settings), then **3** (WiFi).
2. Press **3** again: Scan networks.
3. Pick your network by its number.
4. Type the password (**Alt** for small letters, **SYM** for digits; **MIC** shows or
   hides it) and press **Enter**.
5. "connected" shows on the WiFi page. The radio joins this network again by itself.

WiFi is optional: messages go over LoRa radio without it. Saved networks: WiFi page, row 4.

## Set the If-found details

Video: [if_found.mp4](videos/if_found.mp4)

What a finder sees on the screen, even with a flat battery (e-ink keeps the picture).

1. **M**, **S**, **4** (Screen & I/O), **2** (Idle splash), **9** (If-found details).
2. Press a row's number, type, **Enter**: 1 Name, 2 Phone, 3-4 Address, 5 Town,
   6 State, 7 ZIP, 8 Country, 9-10 anything else. Fill in only what you want shown.
3. **^** back to Idle splash. Press **8** (Idle screen) until it reads **If found**.
4. Row 10 hides or shows the address lines.

When the radio goes to sleep, the If found screen shows. With a password lock
(Idle splash, row 3) it shows while locked too.

## Change the operator mode, touch on / off

Video: [mode_touch.mp4](videos/mode_touch.mp4)

1. **M**, **S**, **1** (Mode).
2. Pick: **1** Minimal (only Inbox, Contacts and Settings: the radio wakes to the Inbox),
   **2** Normal, **3** Advanced (everything).
3. **4** turns the touch screen off (keys only); **4** again turns it on.

In Minimal, Settings shows only row 1, so you can always get back: **M**, **S**, **1**, **3**.

## Open the Atlas (offline map)

Video: [atlas.mp4](videos/atlas.mp4)

The Atlas needs map files on the SD card; without them the menu has no Atlas.

1. Press **A** on any screen (or **M**, then **A**). The map opens on your position.
2. **W** zooms out (wider), **Z** zooms in.
3. **T F V H** move the map up, left, down, right.
4. **D** (or **Enter**) centres it on you again.
5. **K** shows the card with all map keys (waypoints, contacts, find a place ...).
6. **^** closes the map.

More: the help files on the SD card (Library): Help-6 Map, Help-7 Waypoints.

---

## Making the videos again

`tools/howto/make_videos.ps1` runs the scripts in `sim/scripts/howto/` on the simulator
and turns each recording into an MP4 with `tools/howto/make_video.py` (hand pointer,
key names, captions, click sounds). See the top of both files.
