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

**Wireframe note:** diagrams below are schematic (240×320 e-ink layout). Pixel fonts and exact spacing may differ slightly on device.

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
| **I** | Inbox — available on almost every main screen via bottom tab / hotkey |
| **C** | Contacts — same |
| **P** | Peers — same |
| **M** | MENU overlay — opens the top-right menu (browse mode only) |
| **S** | Settings — use **after** MENU: **M** then **S** (reliable path) |
| **H** | Home — usually via MENU (**M** then **H**) |
| **G** | Move selection **up** (lists) |
| **V** | Move selection **down** (lists) |
| **Enter** | Activate / open / send / save |
| **Del / Backspace** | Delete character in entry mode |
| **Shift** (shown as **^** in some footers) | Back up one level / dismiss |
| **Alt** | Toggle **uppercase ↔ lowercase** while typing |
| **SYM** | Digit / symbol layer when typing numbers in settings fields |

Underlined characters on screen are the keyboard shortcuts for the current **browse** mode.

#### Always-visible destinations (typical)

On **Home**, **Contacts**, **Peers**, **Inbox**, and **Settings** (browse mode):

| Control | How you reach it |
|---------|------------------|
| **C** / Contact tab | Contacts |
| **P** / Peers tab | Peers |
| **I** / Inbox tab | Inbox |
| **M** / MENU tab | Overlay: Home, Settings, Contacts, Peers, Inbox |

**Settings is not a bottom tab.** The consistent way to open Settings is:

1. Press **M** (MENU overlay), then  
2. Press **S** (Settings).

(You may also use the MENU row by touch.) Do not assume a lone **S** from every context; **M → S** is the documented operator path.

### 2.2 Touch (secondary)

Where the touch controller works:

- Top **MENU** tab toggles the menu overlay.
- Bottom tabs: **Contact** / **Peers** / **Inbox**.
- List rows and Settings rows can be tapped to select/activate.
- On **idle splash**, touch is ignored; a **physical key** wakes the device.

Prefer documenting and testing **keys** first so the product works if touch is flaky.

### 2.3 Top chrome (most screens)

| Zone | Content |
|------|---------|
| Left | Refresh control, battery icon |
| Center | Clock / date |
| Right | **MENU** (opens overlay) — underline under **M** when active as hotkey |

### 2.4 Bottom chrome (most screens)

| Tab | Key | Screen |
|-----|-----|--------|
| Contact | **C** | Contacts |
| Peers | **P** | Peers |
| Inbox | **I** | Inbox (message list) |

These three stay available in browse mode so you can jump to people or mail without opening MENU. List screens may also show **V=Down**, **G=up**, and **v more** above the tabs.

---

## 3. Navigation model

Navigation is **mode-dependent**. The same key does different things in browse vs entry.

### 3.1 Browse mode

**Where:** Home, Contacts list, Peers list, Inbox list, Settings lists, open MENU overlay (with no text field focused).

**You can:**

- Jump with **C / P / I** (bottom destinations) and **M** (MENU).
- Open Settings with **M** then **S**.
- Move the highlight with **G** (up) and **V** (down).
- Open the highlighted row with **Enter**.
- On many lists, press **1–9** to jump to that row (and on Settings, activate it).
- Use bottom tabs or MENU by key or touch.

**Scroll hints:**

- **v more** — more rows below; keep pressing **V** to reach them.
- **^ more** — more rows above (Settings and similar); aligns under the MENU column when shown.
- When there is nothing above or below, the corresponding hint is **hidden**.

### 3.2 Entry mode (nav shortcuts suppressed)

**Where:**

- Typing a message in an Inbox **thread**
- Renaming a contact / editing display name
- Typing Settings values (timeout seconds, manual time fields, lat/lon, etc.)

**While entry mode is active:**

- Letter keys **insert text**.
- **Alt** switches **upper ↔ lower** case.
- **SYM** (when needed) for digits/symbols on settings fields.
- **M / C / P / I** do **not** jump screens (underlines off for those nav letters).
- **Enter** confirms (send / save / apply).
- **Del** edits the draft.
- **Shift / ^** backs out.

**Rule of thumb:** if you see a compose box or a focused field, you are in entry mode until you save, send, or go back.

### 3.3 MENU overlay mode

Press **M** (browse mode) or the **MENU** tab.

- Overlay lists: Home, Settings, Contacts, Peers, Inbox.
- Press the underlined letter (**H / S / C / P / I**) or tap a row.
- **M** again can close the overlay.

**Settings path:** **M** → **S**.

### 3.4 Digits and submenus

Settings and many lists are numbered **1 … n** on the left of each row.

- Item **4** on root Settings → path segment **S.4**; deeper picks build **S.4.8**, etc.
- Path under the top bar: **`S.4 Battery`**, **`S.9.5 SD card`**, …
- On Settings, a digit often **selects and activates** (example: **M S 4 8** toward battery log).
- On Inbox / Contacts / Peers, digits **select**; **Enter** opens.

---

## 4. Screen map & wireframes

| Screen | Role |
|--------|------|
| **Home** | Status tiles, **Announce ID**, identity / battery summary |
| **Idle splash** | Clock, battery, uptime, unread hint; key to wake |
| **Contacts** | Row **0 = this device (MyID)**; saved peers |
| **Peers** | Heard announces; start of naming a contact |
| **Inbox** | Conversations; **unrd ##** per row when shown |
| **Thread** | Chat + compose; Enter sends |
| **Settings** | Hierarchical **S.*** menus |
| **MENU overlay** | Jump among main screens |

### 4.1 Home

![Home screen wireframe](images/screen-home.svg)

- **Announce ID** = public identity over LoRa (not a private-key export).
- Bottom: **Contact | Peers | Inbox** with **C / P / I** underlines.
- Top-right **MENU** → **M**, then **S** for Settings or **H** for Home from elsewhere.

### 4.2 Contacts

![Contacts screen wireframe](images/screen-contacts.svg)

- **0: MyID** is always this radio; set the display name here early.
- Highlighted row (inverted) is the current selection; **Enter** opens / edits.
- Footer **V=Down** / **G=up**; bottom tab **Contact** active.

### 4.3 Inbox

![Inbox screen wireframe](images/screen-inbox.svg)

- **Unread** summary at top; each row can show **unrd ##** on the right.
- **I** selects this screen from browse mode on other pages.
- **v more** when additional conversations sit below the fold.

### 4.4 Settings (root `S`)

![Settings root wireframe](images/screen-settings.svg)

- Open with **M** then **S**.
- Path line: **`S Settings`** (deeper: **`S.4 Battery`**, etc.).
- Numbered rows **1–9**; **v more** when the list continues (e.g. Diagnostics, Storage).

### 4.5 Idle notes

- Timeout: **S.3.2** / **S.3.2.4**.
- RX may update unread on the splash **without** leaving idle.
- Only a **physical key** wakes (not touch).

---

## 5. Settings tree (`S.*`)

Root path **`S`**. Reach with **M → S**.

```
S     Settings
├── S.1     Time & Location
│   ├── S.1.1   Set RTC
│   ├── S.1.2   Time zone
│   ├── S.1.3   Enable GPS (toggle)
│   ├── S.1.4   Manual location
│   └── S.1.x   Sync GPS now
├── S.2     WiFi                          [TBD — not required for first LoRa use]
├── S.3     Screen & I/O
│   ├── S.3.1   Theme
│   ├── S.3.2   Idle splash
│   │   ├── … modes …
│   │   └── S.3.2.4  Timeout (seconds)
│   └── S.3.3   Key backlight
├── S.4     Battery
│   ├── S.4.1 …   Status / voltage fields
│   └── S.4.8   Bat log
│       ├── S.4.8.1  Internal logging
│       └── S.4.8.2  SD card logging
├── S.5     Identity
├── S.6     LoRa
├── S.7     Alerts
├── S.8     Diagnostics
└── S.9     Storage
    ├── … heap / flash …
    ├── Send Announce ID
    └── S.9.5   SD card tools
```

**Back:** **^** / Shift toward previous segment.  
**WiFi (S.2):** TBD for a later revision; first-mile path is **LoRa only**.

---

## 6. Typical user sequence (send a message)

1. Wake if idle → any key.  
2. **I** → Inbox (bottom tab / hotkey; always present in browse mode).  
3. Select peer → **V** / **G** or digit → **Enter**.  
4. Type message (**Alt** for case). Nav underlines off.  
5. **Enter** to send.  
6. Optional **^** back to list.

**Muscle memory:** `I` → digit → `Enter` → type → `Enter`.

**Settings mid-flow:** `M` → `S` → digits…

---

## 7. First-start sequence (two devices required)

Messaging needs a **second LoRa RatSpeak device** in range (**Pro A** / **Pro B**).

### 7.1 Each device after programming

1. Boot to Home (or wake from idle).  
2. Time: **M → S → 1** (Time & Location) → RTC / zone as needed.  
3. Name this radio: **C** → row **0 MyID** → edit name → **Enter**. Default may be **MyRatSpeakPro**.  
4. Confirm LoRa status on Home.

### 7.2 Discover over LoRa

1. **Pro A** Home → **Announce ID**.  
2. **Pro B** Home → **Announce ID**.  
3. **P** (Peers) on each; confirm the other hash/name appears.

Announce = **public** identity chirp for discovery.

### 7.3 Save a contact name

1. Peers → highlight other device → **Enter** → type name → **Enter**.  
2. Mirror on the other radio.  
3. **C** should list named peers; **0** remains self.

### 7.4 Test message

1. **Pro A:** **I** → select **Pro B** → `test` → **Enter**.  
2. **Pro B:** check Inbox / thread; reply.

If nothing: re-announce, closer range, check **S.6** LoRa, matching firmware builds.

### 7.5 WiFi

**TBD.** Not required above.

---

## 8. Idle & notifications

| Topic | Behavior |
|-------|----------|
| Timeout | **S.3.2** / **S.3.2.4** |
| Wake | Physical **key** only |
| Unread on splash | May update on RX without leaving idle |
| Beep / vibe | **S.7** when enabled |

---

## 9. Stable hotkey policy

| Key | Browse-mode meaning | Must remain |
|-----|---------------------|-------------|
| **I** | Inbox (bottom destination) | Yes |
| **C** | Contacts (bottom destination) | Yes |
| **P** | Peers (bottom destination) | Yes |
| **M** | MENU overlay (not while composing) | Yes |
| **S** | Settings **via MENU** (**M** then **S**) | Yes (as MENU child) |
| **Alt** | Uppercase ↔ lowercase in entry mode | Yes |
| **H** | Home via MENU | Preferred |
| **G** / **V** | Up / down | Preferred |

Changing **I / C / M / P** is a breaking UX change (docs, underlines, muscle memory).

---

## 10. Implementer checklist

1. Browse vs entry gates **I/C/M/P** hotkeys.  
2. Underlines only when the action works.  
3. Bottom **C / P / I** present on main screens in browse mode.  
4. Settings entry path **M → S** documented and tested.  
5. **Alt** toggles case in compose/name fields.  
6. Scroll hints hide when no more rows.  
7. Idle: key wake only; RX must not equal “user woke.”  
8. Announce copy never implies private-key export.  
9. HAL detail stays in `hal/` + `AI-guide.txt`.

---

## 11. Document history

| Version | Notes |
|---------|--------|
| 0.1 | Initial draft: key-first nav, LoRa-first bring-up, stable **I/C/M/P** |
| 0.2 | Clarify always-on **C/P/I**, Settings via **M→S**, **Alt** case toggle; add Home / Contacts / Inbox / Settings wireframes |
