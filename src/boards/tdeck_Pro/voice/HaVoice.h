#pragma once
// Home Assistant voice satellite over WiFi (Wyoming protocol, TCP port 10700).
//
// Home Assistant finds the unit by mDNS ("_wyoming._tcp"), connects, and adds it as an Assist
// satellite. Holding MIC streams the microphone (16 kHz mono) to HA's Assist pipeline; HA's
// spoken reply (and announcements) play on the speaker. Push-to-talk only: no wake word.
//
// Protocol: each event is one JSON line {"type", "data_length", "payload_length"} followed by
// data_length bytes of JSON data and payload_length bytes of payload (audio PCM).

#include <Arduino.h>

class AudioNotify;

namespace havoice {

constexpr uint16_t kPort = 10700;

#if !defined(SIMULATOR)
void begin(AudioNotify* audio);
// Call each loop: runs the server while enabled and WiFi is up, streams mic audio, plays replies.
void tick(bool wifiUp, bool enabled, const char* deviceName);
bool connected();           // Home Assistant is connected and has started the satellite
bool talking();             // the mic is streaming to HA
bool busy();                // talking, or HA's answer is still running / playing (no sleep)
bool startTalk();           // MIC pressed: start an Assist run (false: not connected)
void stopTalk(const char* why);   // MIC released (or HA heard the end of speech)
const char* status();       // one line for Settings / the screen
const char* heard();        // last speech-to-text result ("" none)
const char* answer();       // last spoken answer text ("" none)
uint32_t changedAt();       // millis() of the last status / text change (redraw)
void setMicGain(uint8_t g);
#else
inline void begin(AudioNotify*) {}
inline void tick(bool, bool, const char*) {}
inline bool connected() { return false; }
inline bool talking() { return false; }
inline bool busy() { return false; }
inline bool startTalk() { return false; }
inline void stopTalk(const char*) {}
inline const char* status() { return "simulator: no audio"; }
inline const char* heard() { return ""; }
inline const char* answer() { return ""; }
inline uint32_t changedAt() { return 0; }
inline void setMicGain(uint8_t) {}
#endif

}  // namespace havoice
