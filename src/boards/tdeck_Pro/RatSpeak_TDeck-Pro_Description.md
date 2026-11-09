# RatSpeak T-Deck Pro — Navigation & First-Use Guide

**Audience:** operators holding the device, and developers implementing or reworking UI.  
**Input priority:** **keyboard first**; touch is secondary where the panel works.  
**Radio priority:** **LoRa mesh / LXMF first**. WiFi (station) is optional: scan, join, saved passwords, auto-connect (S.2).  
**Stable hotkeys:** **I**, **C**, **M**, **P** (do not reassign lightly).

Related code: `StandaloneApp.cpp`, `hal/Keyboard.cpp`, `hal/Power.cpp`, `AI-guide.txt`.

**Figures:** RatSpeak UI on the LilyGo T-Deck Pro bezel and keyboard (`images/screen-*.png`). Display content is scaled to the panel only—layout is unchanged. Selection = bold outline; MENU underline under **M** only; bottom underlines under **C / P / I**.

---

## 1. How to

### 1.1 Typical send a message

1. Wake if idle → any key.  
2. **I** → Inbox *(skip if already on Inbox or in the right thread)*.  
3. Select contact via **touch** or a **number**, then **Enter**.  
4. Type message (**Alt** for case). Nav underlines off.  
5. **Enter** to send.  
6. Optional **^** back to list.

```text
I  →  select  →  type  →  Enter  →  ^ (optional)
```

### 1.2 Add a contact / name a contact

**Discover first (two radios):** on each device, Home → **Announce ID**, then open **Peers** (**P**) and confirm the other appears.

**Name them:**

1. **C** → Contacts *(or **P** Peers if they are not saved yet)*.  
2. Select the new device (touch, **V**/**G**, or digit).  
3. **Enter** to open name edit.  
4. Type the display name (**Alt** for case).  
5. **Enter** to save.

```text
C  →  select new device  →  type name  →  Enter
```

Row **0** is always **this** device (**MyID**). Rename it the same way to set the local display name (default may be **MyRatSpeakPro**).

### 1.3 First-boot setup

Do this on each newly programmed radio before relying on messaging.

| Step | What | How |
|------|------|-----|
| 1 | **Configure time** | **M → S → 1** (Time & Location) → set RTC (**S.1.1**) and timezone (**S.1.2**) |
| 2 | **Display theme** | **M → S → 3** (Screen) → **1** Theme → toggle **White paper** vs **Black ink** |
| 3 | **Name this device** | **C** → row **0 MyID** → type name → **Enter** |
| 4 | **WiFi** | **M → S → 2** — scan, join with password, saved networks, auto-connect (not required for LoRa messaging) |
| 5 | **Meet the other radio** | Announce ID both sides → **P** → name contact (§1.2) → test send (§1.1) |

A **second LoRa RatSpeak device** in range is required to verify messaging.

### 1.4 Install on a fresh T-Deck Pro

**Status: TBD** — refine with your exact PlatformIO env and button labels if they differ.

1. Plug in USB, open the PlatformIO project.  
2. Select env **`tdeck_pro_standalone`**.  
3. Build and upload (`pio run -t upload --upload-port COMx`).  
4. Open serial monitor at **115200** if needed.  
5. Power-cycle and continue with §1.3.

---

## 2. Physical controls (summary)

| Key | Browse mode | Entry mode |
|-----|-------------|------------|
| **I** | Inbox | types letter (nav off) |
| **C** | Contacts | types letter |
| **P** | Peers | types letter |
| **M** | MENU overlay | does **not** open menu |
| **H** | Home | — |
| **S** | Settings (or **M→S**) | — |
| **G** / **V** | Up / down | — |
| **Enter** | Activate / open | Send / save |
| **Alt** | — | Case shift while typing |
| **SYM** | — | Digits / symbols when needed |
| **Shift /^** | Back | Back / dismiss |

Touch is secondary: MENU tab, bottom tabs, list rows. Idle wake is **key only**.

---

## 3. Navigation model

**Browse:** hotkeys and underlines active; **G/V** move; digits select (Settings often activates).  
**Entry:** compose or field focused — **M/C/P/I** do not navigate; **Alt** changes case.  
**MENU overlay:** **M** opens column under MENU (**H/S/C/P/I**); top bar stays visible.  
**Scroll:** **v more** / **^ more** only when more rows exist.

---

## 4. Screen map & figures

### 4.1 Idle splash

![Idle splash](images/screen-idle.png)

- RatSpeak logo, date/time, battery %, plug status, message line, uptime, device name.  
- **press a KEY to wake** (touch does not wake).

### 4.2 Home

![Home screen](images/screen-home.png)

- Status tiles, **Announce ID**, device name below Announce.  
- Bottom **Contact | Peers | Inbox**.

### 4.3 Home with MENU overlay

![Home with MENU overlay](images/screen-home-menu.png)

- Overlay column under **MENU**: **H**ome · **S**ettings · **C**ontacts · **P**eers · **I**nbox.  
- Overlay ends **above** the bottom tab bar. Top bar stays visible.

### 4.4 Contacts

![Contacts](images/screen-contacts.png)

- **0: MyID** = this radio. Selection = bold outline.

### 4.5 Inbox (list)

![Inbox list](images/screen-inbox.png)

- Per-row **unrd ##** when unread. **v more** when the list continues.

### 4.6 Inbox thread (typing)

![Inbox compose](images/screen-inbox-compose.png)

- Entry mode; **M/C/P/I** nav off. Footer **^ BACK UP TO INBOX**.

### 4.7 Settings root (`S`)

![Settings](images/screen-settings.png)

- Open **M → S**. Numbered rows → **S.1 … S.9**.

### 4.8 Settings **S.3 Screen**

![Settings S.3](images/screen-settings-s3.png)

- Theme, Idle splash, Key backlight.

---

## 5. Settings tree (`S.*`)

```
S     Settings          ← M then S
├── S.1     Time & Location
│   ├── S.1.1   Set RTC
│   ├── S.1.2   Time zone
│   ├── S.1.3   Enable GPS
│   ├── S.1.4   Manual location
│   └── …       Sync GPS now
├── S.2     WiFi                    (status, IP)
│   ├── S.2.3   Scan networks       pick → saved: join; new locked: type password (8-63, Alt+key = case)
│   │   └── …   Join WiFi           Password · Auto-connect (default ON) · Join (saves on success)
│   ├── S.2.4   Saved networks      up to 8, stored in NVS rs_wifi
│   │   └── …   Saved network       Connect now · Auto-connect · Password · Forget
│   └── S.2.5   Disconnect          radio off, auto-connect paused until next manual join
├── S.3     Screen & I/O
│   ├── S.3.1   Theme (white / black)
│   ├── S.3.2   Idle splash (+ S.3.2.4 timeout)
│   └── S.3.3   Key backlight
├── S.4     Battery (+ S.4.8 bat log)
├── S.5     Identity
├── S.6     LoRa
├── S.7     Alerts
├── S.8     Diagnostics
└── S.9     Storage (+ S.9.5 SD)
```

---

## 6. Idle & notifications

| Topic | Behavior |
|-------|----------|
| Timeout | **S.3.2** / **S.3.2.4** |
| Wake | Any **key** |
| Unread on splash | May update on RX without leaving idle |
| Beep / vibe | **S.7** when enabled |

---

## 7. Stable hotkey policy

**I / C / M / P** must stay. **S** is reached via **M → S**. **Alt** = case in entry mode.

---

## 8. Implementer checklist

1. Browse vs entry gates **I/C/M/P**.  
2. Underlines only when the action works.  
3. Bottom **C/P/I** in browse mode on main screens.  
4. Settings: **M → S**.  
5. Idle: key wake only.  
6. Announce never implies private-key export.  
7. HAL detail: `hal/` + `AI-guide.txt`.

---

## 9. Document history

| Ver | Notes |
|-----|--------|
| 0.1 | Initial draft: How-to first, device figures (`images/screen-*.png` with bezel/keyboard), Settings tree, LoRa-first bring-up |
