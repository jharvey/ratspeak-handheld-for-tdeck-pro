# RatSpeak T-Deck Pro — Navigation & First-Use Guide

**Audience:** operators holding the device, and developers implementing or reworking UI.  
**Input priority:** **keyboard first**; touch is secondary where the panel works.  
**Radio priority:** **LoRa mesh / LXMF first**. WiFi is reserved for later (see TBD).  
**Stable hotkeys (do not reassign without a deliberate UX break):** **I**, **C**, **M**, **P**.

Related code (implementers):

- UI / nav: `src/boards/tdeck_Pro/StandaloneApp.cpp`
- Keyboard HAL: `src/boards/tdeck_Pro/hal/Keyboard.cpp`
- Power / battery: `src/boards/tdeck_Pro/hal/Power.cpp`
- Board notes: `AI-guide.txt` (repo root)

---

## 1. Purpose

This document describes:

1. How you move around the device with **keys** (and touch).
2. When navigation shortcuts **disappear** (text entry and similar modes).
3. The **screen map** and **Settings `S.*` tree**.
4. A **typical** “send a message” path.
5. A **first-start** path with **two LoRa devices** (required for a real test).

It is not a pinout or protocol spec. HAL and low-level bring-up stay in `hal/` and `AI-guide.txt`.

---

## 2. Physical controls

### 2.1 Keyboard (primary)

| Control | Typical role |
|--------|----------------|
| Letter keys | Navigation hotkeys when **not** typing; text when **in entry mode** |
| **I** | Inbox (stable) |
| **C** | Contacts (stable) |
| **P** | Peers (stable) |
| **M** | MENU overlay (stable) — **only when not composing** |
| **H** | Home |
| **S** | Settings |
| **G** | Move selection **up** (lists) |
| **V** | Move selection **down** (lists) |
| **Enter** | Activate / open / send / save |
| **Del / Backspace** | Delete character in entry mode |
| **Shift** (shown as **^** in some footers) | Back up one level / dismiss |
| **SYM** | Digit / symbol layer when typing numbers in settings fields |

Underlined characters on screen are the keyboard shortcuts for the current **browse** mode.

### 2.2 Touch (secondary)

Where the touch controller works:

- Top **MENU** tab toggles the menu overlay.
- Bottom tabs: **Contact** / **Peers** / **Inbox**.
- List rows and Settings rows can be tapped to select/activate.
- On **idle splash**, touch is ignored; a **physical key** wakes the device.

Prefer documenting and testing **keys** first so the product works if touch is flaky.

### 2.3 Top chrome (most screens)

Approximate layout:

| Zone | Content |
|------|---------|
| Left | Refresh control, battery icon |
| Center | Clock / date |
| Right | **MENU** (opens overlay) |

### 2.4 Bottom chrome (most screens)

| Tab | Key | Screen |
|-----|-----|--------|
| Contact | **C** | Contacts |
| Peers | **P** | Peers |
| Inbox | **I** | Inbox (message list) |

List screens may show a thin footer above these tabs: **V=Down**, **G=up**, and **v more** when more rows exist below the viewport.

---

## 3. Navigation model

Navigation is **mode-dependent**. The same key does different things in browse vs entry.

### 3.1 Browse mode

**Where:** Home, Contacts list, Peers list, Inbox list, Settings lists, open MENU overlay (with no text field focused).

**You can:**

- Jump screens with **I / C / P / M / H / S** (and underlined labels).
- Move the highlight with **G** (up) and **V** (down).
- Open the highlighted row with **Enter**.
- On many lists, press **1–9** to jump to that row (and on Settings, activate it).
- Use bottom tabs or MENU by key or touch.

**Scroll hints:**

- **v more** — more rows below; keep pressing **V** (or scroll) to reach them.
- **^ more** — more rows above (Settings and similar); aligns under the MENU column when shown.
- When there is nothing above or below, the corresponding hint is **hidden** (empty footer cell or no `^ more` box).

### 3.2 Entry mode (nav shortcuts suppressed)

**Where:**

- Typing a message in an Inbox **thread**
- Renaming a contact / editing display name
- Typing Settings values (timeout seconds, manual time fields, lat/lon, etc.)

**While entry mode is active:**

- Letter keys **insert text** (or digits when SYM/num layer is on).
- **M / C / P / I** do **not** jump screens (no underlined nav on those letters for chrome).
- **Enter** confirms (send message, save name, apply field).
- **Del** edits the draft.
- **Shift / ^** backs out without treating the draft as a hotkey destination.

**Rule of thumb:** if you are looking at a compose box or a blinking field, you are in entry mode until you save, send, or explicitly go back.

### 3.3 MENU overlay mode

Press **M** (browse mode only) or the **MENU** tab.

- Overlay lists destinations (Home, Settings, Contacts, Peers, Inbox).
- Press the underlined letter or tap a row to go there.
- **M** again (or back) closes the overlay without changing screen when appropriate.

### 3.4 Digits and submenus

Settings and many lists are numbered **1 … n** on the left of each row.

- **Implication:** item **4** on the root Settings list is the path segment **S.4**; opening it and choosing item **8** leads toward **S.4.8**, and so on.
- Path label under the top bar looks like **`S.4 Battery`** or **`S.9.5 SD card`** (path + short name).
- On Settings, pressing a digit typically **selects and activates** that row (no extra Enter), so `M` → Settings → `4` → `8` can reach battery log quickly.
- On Inbox / Contacts / Peers, digits usually **select** the row; **Enter** opens it.

Numbers **0–9** on screen are the user’s map of depth; implementers should keep numbering stable when adding items (append or document breaks).

---

## 4. Screen map

Screen figures are **240×320 e-ink style** (no bezel/keyboard). Logo matches the RatSpeak mark (speech-bubble + mouse).

### Idle splash

![Idle splash — RatSpeak logo, clock, battery, uptime, device name, press a KEY to wake](images/screen-idle.png)

*Idle: key-only wake. Unread line may update on RX without leaving this screen.*

### Home with MENU open

![Home with MENU overlay — five menu rows end above bottom tabs; underlined H/S/C/P/I](images/screen-home-menu.png)

*Home browse mode + MENU overlay. Overlay **Inbox** sits above the bottom tab bar (not flush to the bottom of the panel). Bottom tabs: **C** Contact, **P** Peers, **I** Inbox.*

| Screen | Role |
|--------|------|
| **Home** | Status tiles (LoRa, peers, audio, …), **Announce ID**, identity summary, battery shorthand |
| **Idle splash** | Low-distraction clock, battery, uptime, unread hint, device name; **press a key to wake** |
| **Contacts** | Trusted / named destinations; row **0** is **this device (MyID)** |
| **Peers** | Recently heard announces (and related discoveries); start of “add a name” |
| **Inbox** | Conversation list; per-row **unrd ##** on the right when implemented |
| **Thread** | Messages with one peer; compose at bottom; Enter sends |
| **Settings** | Hierarchical **S.*** menus |
| **MENU overlay** | Quick jump between main screens |

### 4.1 Home notes

- **Announce ID** broadcasts this radio’s **public** identity over LoRa so others can see it in Peers. It is **not** a private-key dump.
- Device display name appears near the bottom on most screens; on Home it sits under Announce so it does not cover the button.

### 4.2 Idle notes

- Entered after **idle timeout** (see **S.3.2**).
- **Inbound messages** may update the unread line on the splash **without** leaving idle.
- Only a **physical key** exits idle (touch does not wake).
- Leaving idle returns to the screen you were on before idle.

---

## 5. Settings tree (`S.*`)

Root path **`S`**. Each numbered row is a segment. Names below match on-device path labels where possible.

```
S     Settings
├── S.1     Time & Location
│   ├── S.1.1   Set RTC (manual clock/date fields, then apply)
│   ├── S.1.2   Time zone
│   ├── S.1.3   Enable GPS (toggle; row index may vary slightly by build)
│   ├── S.1.4   Manual location (lat,lon when no GPS)
│   └── S.1.x   Sync GPS now (when fix available)
├── S.2     WiFi                          [TBD — not required for first LoRa use]
├── S.3     Screen & I/O
│   ├── S.3.1   Theme (white paper / black ink)
│   ├── S.3.2   Idle splash
│   │   ├── S.3.2.1  No idle screen
│   │   ├── S.3.2.2  Idle, no password
│   │   ├── S.3.2.3  Idle + password (TBD)
│   │   └── S.3.2.4  Timeout (seconds)
│   └── S.3.3   Key backlight
├── S.4     Battery
│   ├── S.4.1 … S.4.7  Voltage / status / gauge fields (see on-device rows)
│   └── S.4.8   Bat log
│       ├── S.4.8.1  Internal logging
│       └── S.4.8.2  SD card logging
├── S.5     Identity (destination hash, display name, LXMF)
├── S.6     LoRa (freq / enable summary)
├── S.7     Alerts (audio / vibrate)
├── S.8     Diagnostics (paths, links, RSSI, LXMF queue)
└── S.9     Storage
    ├── S.9.1…  Heap / PSRAM / flash summaries
    ├── S.9.4   Send Announce ID (same idea as Home)
    └── S.9.5   SD card tools (mount / format-related; see on-device)
```

**Back:** from a sub-page, **^** / Shift returns toward **S** (and previous segments). Path text under the header always shows where you are.

**WiFi (S.2):** placeholder for a later revision. First-mile product path is **LoRa only**; leave S.2 documented as **TBD** until station mode / credentials UX is defined.

---

## 6. Typical user sequence (send a message)

Goal: from a powered device, reach “message on the air” with few keys.

Assumes contacts already exist (see §7 if not).

1. **Wake** if on idle splash → press any key.
2. **Inbox** → press **I** (or MENU → Inbox, or bottom Inbox tab).
3. **Select peer** → **V** / **G** or digit **1–9**, then **Enter** (opens thread).
4. **Type** message (entry mode: nav underlines off).
5. **Enter** to send.
6. Optional: **^** / back to return to the Inbox list.

**Fast path muscle memory:** `I` → digit → `Enter` → type → `Enter`.

If already in the correct thread, skip to step 4. New inbound messages should refresh the list/thread while preserving an in-progress draft when the UI auto-redraws.

---

## 7. First-start sequence (two devices required)

A single radio can boot and show UI, but **messaging is only meaningful with a second LoRa RatSpeak device** (or compatible peer) in range.

Call them **Pro A** and **Pro B**.

### 7.1 After programming (each device)

1. Power on; wait through boot splash to **Home** (or wake from idle).
2. **Set time** (recommended so logs and UI clocks make sense):
   - **M** or **S** → Settings → **1** Time & Location → set RTC / timezone as needed (**S.1.1**, **S.1.2**).
3. **Name this device** (contact **0 / MyID**):
   - Open **Contacts** (**C**).
   - Row **0** is local identity (e.g. `0:MyID:…`).
   - Open / edit name and save (Enter).  
   - Default factory-style name may be **MyRatSpeakPro** until changed.
4. Confirm **LoRa** looks alive on Home (RSSI / online / peer tiles as implemented).

### 7.2 Discover each other over LoRa

1. On **Pro A** Home → activate **Announce ID** (button or Settings storage announce row).
2. On **Pro B** Home → **Announce ID** as well.
3. Open **Peers** (**P**) on each device.
4. Confirm the other radio appears (hash and/or temporary name, RSSI if shown).

**Announce ID** = chirp **public** identity for discovery. It does not replace careful handling of secrets; naming is local metadata on top of the destination hash.

### 7.3 Save a contact with a human name

1. On **Pro A**, in **Peers**, highlight **Pro B**’s row → **Enter** (or follow on-screen open/save flow).
2. Assign a display name (e.g. `PRO-B` or a person name) → **Enter** to save.
3. Repeat on **Pro B** for **Pro A**.
4. Check **Contacts** (**C**): named peers should appear for later Inbox use.

Row indices on Contacts are **0 = this device**, then **1, 2, 3…** for saved others.

### 7.4 First test message

1. On **Pro A**: **I** → select **Pro B** → **Enter** → type `test` → **Enter**.
2. On **Pro B**: idle may show unread; or open **I** and confirm the thread.
3. Reply from **Pro B** so **Pro A** sees a return path.

If nothing appears:

- Re-announce both sides.
- Move closer / check antenna and **S.6** LoRa summary.
- Confirm both builds are standalone T-Deck Pro LoRa images.

### 7.5 WiFi

**TBD.** Not required for the sequence above. Future work: credentials, enable toggle under **S.2**, and how WiFi transport interacts with LoRa-first messaging.

---

## 8. Idle & notifications

| Topic | Behavior |
|-------|----------|
| Timeout | **S.3.2** / **S.3.2.4** |
| Wake | Physical **key** only |
| Unread on splash | May update on RX without leaving idle |
| Beep / vibe | Optional alerts (**S.7**) on inbound when enabled |

---

## 9. Stable hotkey policy

| Key | Meaning in browse mode | Must remain |
|-----|------------------------|-------------|
| **I** | Inbox | Yes |
| **C** | Contacts | Yes |
| **P** | Peers | Yes |
| **M** | MENU overlay (not while composing) | Yes |
| **H** | Home | Preferred stable |
| **S** | Settings | Preferred stable |
| **G** / **V** | Up / down in lists | Preferred stable |

Changing **I/C/M/P** requires updating this document, on-screen underlines, and muscle memory—treat as a breaking UX change.

---

## 10. Implementer checklist (programming from scratch)

When rebuilding UI behavior:

1. **Browse vs entry:** clear flag (compose length, name-edit, settings field edit) gates hotkeys.
2. **Underlines** only for actions that work *now*.
3. **Scroll hints** hide when `scroll == 0` or no remaining rows.
4. **Settings path string** matches tree segment (`S.4.8` + label).
5. **Digits** on Settings activate; lists select consistently.
6. **Idle** ignores touch; RX must not call the same path as “user woke device.”
7. **Announce** copy never implies private key export.
8. **HAL** stays out of this doc—board pins, BQ27220, TCA8418, e-ink, radio wiring belong under `hal/` + `AI-guide.txt`.

---

## 11. Document history

| Version | Notes |
|---------|--------|
| 0.1 | Initial draft from standalone T-Deck Pro behavior: key-first nav, LoRa-first bring-up, stable **I/C/M/P**, Settings tree, two-device first message |
| 0.2 | Added full-device reference photos (idle splash, Home+MENU) with bezel and keyboard |
| 0.3 | Composited RatSpeak UI onto LilyGo product bezel/keyboard graphic; larger figures |
| 0.4 | Clean screen graphics: updated logo; MENU overlay Inbox raised above bottom tabs |

