# RatSpeak ATAK bridge (Android)

Carries a T-Deck Pro's ATAK feed over Bluetooth LE to ATAK on the same phone, so the radio and
the phone need no WiFi network. The radio side is `src/boards/tdeck_Pro/CotBle.cpp`.

```
ATAK ──UDP 127.0.0.1:4242──▶ ┐                ┌─ BLE (paired, encrypted) ─▶ T-Deck Pro ⇄ LoRa ⇄ other radios
ATAK ◀─SA 239.2.3.1:6969 /    ├─ bridge app ──┤
        output 127.0.0.1:4349 ┘                └─ ◀────────────────────────
```

- **Radio to ATAK:** the radio notifies CoT XML events (Nordic UART service, TX 6E400003),
  cut into MTU-sized pieces, each event followed by `\n`. The app joins them, splits on
  `</event>` and sends each event to ATAK's local UDP input (127.0.0.1:4242 by default;
  changeable in the app).
- **ATAK to radio:** the app reads ATAK's SA multicast (its own position) and UDP port 4349
  (add it once in ATAK: Settings, Network Preferences, Manage Outputs, 127.0.0.1:4349 UDP,
  for markers). It writes those events to the radio (RX 6E400002). Events with a
  `RatSpeak-` uid came from a radio and are not sent back.
- **Pairing:** on the first connection the radio asks for LE Secure Connections pairing with a
  passkey. It shows a 6-digit code, and Android's pairing dialog asks for it. The radio sends
  nothing before the link is encrypted and authenticated.
- A foreground service keeps the link while ATAK is in front, and reconnects every 5 s when
  the radio is out of reach.

Android 8 (API 26) or newer. Permissions: Nearby devices (Bluetooth scan / connect; location
on Android 8 to 11), notifications.

## Build

No Gradle and no Android Studio: `build.sh` runs aapt2, javac, d8, zipalign and apksigner
(JDK 17 and the Android SDK's platform 34 + build-tools 34.0.0). From the PC:

```
python tools/android/build_apk.py --version-code 2 --version-name 0.2
```

This builds on VM100 and writes `firmware/RatSpeak-ATAK-bridge.apk`. The signing key is on
VM100 in `~/.android-keys`, with a backup on the NAS in `M:\ratspeak-keys`. Every update must
be signed with the same key, or Android refuses to install it over the old one.

## Test without a phone

`python sim/scripts/atak_ble.py` plays the phone against the simulator. The simulator's
stand-in for the BLE link is UDP on 127.0.0.1:4391.
