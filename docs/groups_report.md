# Group messages: report (22 Nov 2026)

Future_steps item 12. Built and tested in the simulator with four radios. Not yet tried on
real units (none were flashed).

## What was built

- **Groups**: a name plus up to 8 contacts, kept on the radio. Up to 8 groups.
- **Contacts > G** opens the Groups list: new group, members, rename, delete.
- **A message to a group** goes to every member as a normal message, one after the other,
  through the outbox that already spaces sends and retries.
- **Group chat**: one chat per group. It shows who said what ("ALEX: text") and, under each
  of your own messages, how many members have it ("2/3 delivered", "1 to send",
  "1 no reply"). **MIC** lists who got your last message, member by member.
- **Groups are the first rows of the Inbox**, marked "group".
- **Share a group** with one of its members. They press Enter in your chat to join. Members
  they did not know are saved as contacts.
- A radio or app **without the group** (Sideband, MeshChat, an older RatSpeak) sees a
  normal direct message from you. A RatSpeak radio marks it "[FAMILY] text".
- Console commands for the bench (`grp ...`), listed in the Description document.

## How to use it

The step-by-step guide is in `docs/HowTo.md`, "Groups". In short:

1. **C**, **G**, **New group**, type a name, **Enter**. Pick members with **Enter**, **^** when done.
2. On the group's page press **1** (Message), type, **Enter**. Or open the group from the Inbox.
3. **4** (Share), pick a member, **Enter**: once per member. They open your chat and press
   **Enter** on the empty line to join.

## Design choices

**No server and nothing new on the air.** A group message is one ordinary LXMF direct
message per member. Each copy is encrypted for that member and gets its own delivery
receipt, exactly like a message typed to that contact.

**The tag.** Each copy carries LXMF field 0x0B (named FIELD_GROUP in LXMF, with no format
defined there) holding one short text: `<8 hex group id> <8 hex send number> <group name>`,
for example `2fe77d85 6b02eaa7 TEAM`. It adds about 32 bytes to the packet. The message
text is untouched, so it reads normally everywhere.

- Group id: 4 random bytes made when the group is created. It is what makes two radios'
  copies "the same group"; the name is only a label and each radio can rename its copy.
- Send number: the same on every member's copy of one message. Your radio uses it to count
  its copies as one row ("2/3 delivered").

**The group chat is not stored separately.** The tagged messages already sit in each
member's own chat in the message store. Opening a group chat reads the newest 6 messages of
each member's chat and shows the newest 6 that carry this group's tag. So nothing extra is
written to flash, a restart loses nothing, and delivery states come straight from the
stored messages. The cost is reading: up to 6 records per member each time the chat opens
or refreshes (see Limits).

**The same messages also show in each member's own chat**, marked "[FAMILY]". When the
newest incoming message there is a group message, the bottom line offers "[Enter] open
group: FAMILY". This matters because the new-message alert and the unread count belong to
the sender's chat: you land there first, and one key takes you to the group.

**Unread counts.** Opening the group chat marks a member's chat as read only when
everything unread in it was this group's. An unread private message is never hidden by
reading the group.

**Sharing a group.** A group goes to a member as plain-text "group cards", one per person
(you first, then every member), readable on any LXMF app:

```
Group card: TEAM #2fe77d85 2/3
BOB
689f6a9e3984879ec417061d527eb1b8
```

The receiving radio shows "[group] TEAM 2/3 BOB". When all parts are in the chat, Enter on
the empty line joins: it saves unknown members as contacts (names you already gave are
kept), leaves itself out, and adds the group. A group can only be sent to one of its own
members, one member per share. Sending to all at once would be members x (members + 1)
messages, far too much airtime.

**When a member is added or removed later.** Every radio has its own copy of the member
list and nothing syncs by itself. The change is on your radio only, at once. To pass it on,
share the group again: the other radio sees the new cards and Enter replaces its member
list (its own name for the group is kept). Until then the others keep sending to the old
list. A removed member keeps their copy and can still write to the members they know; on a
radio that removed them, such a message shows in their own chat as "[FAMILY] text", not in
the group chat. A group message only appears in the group chat when its sender is in your
copy of the group.

**Storage.** Groups: NVS `rs_groups`, one blob, written only when a group changes. The
outbox keeps its format; the group mark of waiting messages is a second small blob
(`g2`). Saved data from the current firmware loads unchanged. A whole group message is one
outbox write, not one per member.

**Reuse for ATAK-style sharing.** `groupSend(group, text)` sends any text to every member
(all or nothing, outbox, check-in mode aware) and `sendMessage` / `sendOrQueue` /
`outboxAdd` take an optional group id. A location or marker to a group is one call.

## Limits

- **8 members per group, 8 groups.** The reason is airtime. At the default LoRa setting
  (Long Fast: SF11, 250 kHz) one copy of a 40-letter message is about 2.2 s on the air and
  its delivery receipt about 1 s; the longest message (120 letters) is about 2.8 s. The
  outbox sends one copy every 3.5 s. So a message to 8 members keeps the channel busy for
  about half a minute, and 8 people each answering once is about 4 minutes of solid
  traffic. More members than that would not be honest on LoRa. Groups of 3 to 5 are
  comfortable. (Calculated from the LoRa settings and packet sizes, not measured on air.)
- **Long Slow (SF12, 125 kHz)** is about 15 s per copy plus a 7 s receipt: a group of 8
  takes about 3 minutes per message. Keep groups to 2 or 3 there.
- **8 is also the outbox size.** A group message is sent only when the whole message fits:
  the first copy goes at once, the rest wait in the outbox. If other messages are already
  waiting, the group message is refused with "Not sent: radio busy" and the text stays on
  the typing line. Sharing an 8-member group (9 cards) needs an empty outbox.
- **The group chat shows the newest 6 group messages**, found among the newest 6 messages
  of each member's chat. A group message pushed back by 6 newer private messages with that
  member drops out of the group chat (it is still in that member's chat).
- **Order** in the group chat is by this radio's clock: when it received or sent each one.
- **Message length**: 120 letters, as in any chat.
- **Check-in mode**: a group message waits for the window like any other; all copies queue.

## Test results

All on the simulator (`C:\Users\MyPC\simrun\simb`), final source `4d74d09`, 22 Nov 2026.

| Test | Result |
|---|---|
| `sim/scripts/groups.txt`: create a group with the keys, add and remove members, rename, open the group chat, Inbox row, delete a second group, ninth member refused | PASS |
| `sim/scripts/groups_after.txt`: a restart keeps the group, its name and 7 members | PASS |
| `sim/scripts/groups_net.py`, four radios (ANN, BOB, CAT, DAN), 8 steps | 8 of 8 PASS, run twice |
| 1 send to a group of two; members without the group get a direct message; "delivered 2/2" | PASS |
| 2 share a group (once by console, once with the keys) and join; an unknown member is saved as a contact | PASS |
| 3 a group message shows as "ANN: text" in the members' group chat; "[TEAM] text" in ANN's own chat and Enter opens the group | PASS |
| 4 reply to the group: the other two see "BOB: text" | PASS |
| 5 send to a group of three: "delivered 3/3"; a radio without the group shows "[TEAM] text" | PASS |
| 6 a member added, group shared again: the other radio's copy is updated, its next message reaches the new member | PASS |
| 7 a member switched off: the others show delivered, the missing one never does | PASS, see note |
| 8 a restart keeps a joined group | PASS |
| Existing scripts: smoke, lists, lock, lock_boot, lock_button (3 scripts), standby, standby_after, share_a | PASS |
| `share_b.txt` (receive a contact card) | FAILED as it was: the script did not skip the setup wizard, so its keys went to the wizard. Not caused by groups. I added the skip line; PASS after that |
| `inbox_pages.txt` | PASS on the first run, FAILED once on the second (14 of 20 test chats made in time: the PC was busy with other simulators and the script's fixed waits were too short), then PASS twice when run alone. A timing weakness of the script; the firmware did the right thing |
| `standby_pair.py` | not run |
| `tools/stress/monkey.py`, 2 rounds, 5959 random keys | PASS, 0 failed |
| Firmware build on VM100 | SUCCESS, RAM 65.3 %, flash 56.4 %. Image commit `4540845` (build of `4d74d09`) |

Note on step 7 (member not reachable): while the copy is still being tried the row reads
"2/3 delivered, 1 to send"; after that "2/3 delivered, 1 no reply", and MIC shows "DAN: no
reply". In the 7 minutes the test watched, the message engine never marked the copy
"failed", so in practice an out-of-range member shows as "no reply", not "failed".

One thing found on the way: the device compiler refused one line the simulator's compiler
accepted (fixed before the commit). The groups code adds about 4.5 KB of permanent RAM;
the group chat borrows about 7 KB more only while it is being built.

No unit was flashed for these tests (see "Real units" below for what was tried after).

A slip to know about: my report commit `a2c52bd` also carried three lines that another
session had just written into `docs/HowTo.md` (the `point` command under "Making the videos
again"). They are that session's text, unchanged; nothing was lost.

## Real units, 22 Nov 2026 (not conclusive)

COM6 and COM9 were flashed with image `4540845` (owner's go-ahead; COM5 was not on USB, COM8
kept logging on the older firmware). Both started normally and their soak test carried on.

A group TEST (COM9 + COM8) was made on COM6 by console and one message, then the group
cards, were sent. The firmware accepted them ("1 now, 1 queued, 0 failed") but nothing
arrived on COM9 or COM8 in the 15 minutes watched. Cause seen in the logs, not a group
fault as far as I can tell: both units' message stores are nearly full from the soak test
(COM6 about 92 %, COM9 85 %) and spend 1.5 to 3 minutes at a time in "flash space"
clean-ups (COM6: "removed 0 oldest messages in 154714 ms"), so every outgoing message sat
in "saving (slow store)" with the engine rows "store held". COM6's own soak reports had
already stopped reaching COM8 at 07:12, six hours before the flash.

So groups are still unproven on real radios. A clean test needs units with room in the
message store (or the store emptied, which I did not do: it is the soak test's data).
Left on the units: group TEST on COM6, and the unsent test messages in its queue.

## Not done

- **Real units.** Nothing was flashed. To check on hardware: airtime and delivery with 3 or
  more units, and how long a busy group chat takes to open (it reads up to 6 stored
  messages per member; in the simulator the largest was 0.4 s for 3 members).
- **Unread count on the group's Inbox row.** The row says "group"; the count shows on the
  sender's row.
- **Voice messages, locations, walkie-talkie and the low-battery notice to a group.** MIC
  in a group chat shows who got the last message instead of the location menu.
- **Automatic sync** of member changes, and leaving a group on other people's radios.
- **A how-to video.** Instructional video 29 can be made now.
- **Sideband / MeshChat check on a real client.** The message is a standard LXMF message
  with one extra field, so it should show as plain text; this was not tried against a real
  Sideband.

## Decisions for the owner

1. **Should there be a group chat at all?** Recommendation: yes, as built. It costs no new
   radio scheme and a member without the feature still gets every message.
2. **Should groups live under Contacts?** Recommendation: yes, as built (Contacts, then
   **G**), with the group chats also listed first in the Inbox, because that is where chats
   are opened day to day. If you would rather have "Groups" as its own row in the MENU,
   that is a small change.
3. **Group size.** 8 is the hard limit. Say if you want it lower (5 would match the airtime
   better) or the reason shown on screen.
4. **Field number.** The tag uses LXMF field 0x0B (FIELD_GROUP), which LXMF names but does
   not define. If a future Sideband gives it another meaning, the tag can move to the
   custom fields (0xFB / 0xFC) at a cost of about 16 more bytes per message. Changing it
   later makes old and new firmware not see each other's group tags (messages still arrive).
5. **Try it on units?** The firmware is built but no unit was flashed. Say which units may
   be flashed for a three-unit group test.
