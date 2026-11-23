#pragma once
// Home Assistant voice satellite over WiFi, speaking the ESPHome native API (TCP 6053).
//
// Home Assistant's ESPHome integration finds the unit by mDNS ("_esphomelib._tcp") and adds
// it as an Assist satellite. With an encryption key set (the usual case) the connection is
// encrypted with Noise_NNpsk0_25519_ChaChaPoly_SHA256, the same as an ESPHome device with
// "api: encryption: key: ...": Home Assistant asks for that key when it adds the device.
// Pressing MIC streams the microphone (16 kHz mono) to Assist; Home Assistant's spoken answer
// is streamed back over the same connection and plays on the speaker.

#include <Arduino.h>
#include <string.h>

class AudioNotify;

namespace havoice {

constexpr uint16_t kPort = 6053;
constexpr size_t kKeyB64Len = 44;            // 32 bytes, base64

// ---- base64 key helpers (also used by Settings in the simulator) ------------------------
inline int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}
// The 44-character key -> 32 bytes. false: not a valid key.
inline bool keyDecode(const char* b64, uint8_t out[32]) {
    if (!b64 || strlen(b64) != kKeyB64Len || b64[43] != '=') return false;
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (int i = 0; i < 43; i++) {
        const int v = b64val(b64[i]);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < 32) out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o == 32;
}
inline void keyEncode(const uint8_t in[32], char out[kKeyB64Len + 1]) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (int i = 0; i < 32; i += 3) {
        const uint32_t v = ((uint32_t)in[i] << 16) | (i + 1 < 32 ? (uint32_t)in[i + 1] << 8 : 0) |
                           (i + 2 < 32 ? in[i + 2] : 0);
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = i + 1 < 32 ? t[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < 32 ? t[v & 63] : '=';
    }
    out[o] = 0;
}

#if !defined(SIMULATOR)
void begin(AudioNotify* audio);
// Call each loop: runs the API server while enabled and WiFi is up, streams the mic, plays
// answers. keyB64: the encryption key ("" = unencrypted, like ESPHome without a key).
void tick(bool wifiUp, bool enabled, const char* deviceName, const char* keyB64);
bool connected();           // Home Assistant is connected and subscribed to voice
bool talking();             // the mic is streaming to HA
bool busy();                // talking, or HA's answer is still running / playing (no sleep)
bool startTalk();           // MIC pressed: start an Assist run (false: not connected)
void stopTalk(const char* why);   // MIC released (or HA heard the end of speech)
// Wake word: while on (call each loop) and HA is connected, a run that starts at Home
// Assistant's wake word stage stays open and the mic streams to it; HA's wake word engine
// (e.g. openWakeWord) listens. MIC still works (it ends that run and starts push-to-talk).
void setWakeWord(bool on);
bool wakeListening();      // the mic streams to HA waiting for the wake word
const char* status();       // one line for Settings / the screen
const char* heard();        // last speech-to-text result ("" none)
const char* answer();       // last spoken answer text ("" none)
uint32_t changedAt();       // millis() of the last status / text change (redraw)
void setMicGain(uint8_t g);
const char* nodeName();     // the name Home Assistant sees ("com6-17cc")
bool selfTest();            // X25519 (RFC 7748) + ChaCha20-Poly1305 (RFC 8439) vectors
void tunnelStart(const char* keyB64);   // bench: the API over USB serial ("@API <hex>" lines)
void tunnelFeed(const char* hex);
void tunnelStop();
#else
inline void begin(AudioNotify*) {}
inline void tick(bool, bool, const char*, const char*) {}
inline bool connected() { return false; }
inline bool talking() { return false; }
inline bool busy() { return false; }
inline bool startTalk() { return false; }
inline void stopTalk(const char*) {}
inline void setWakeWord(bool) {}
inline bool wakeListening() { return false; }
inline const char* status() { return "simulator: no audio"; }
inline const char* heard() { return ""; }
inline const char* answer() { return ""; }
inline uint32_t changedAt() { return 0; }
inline void setMicGain(uint8_t) {}
inline const char* nodeName() { return "ratspeak-sim"; }
inline bool selfTest() { return true; }
inline void tunnelStart(const char*) {}
inline void tunnelFeed(const char*) {}
inline void tunnelStop() {}
#endif

}  // namespace havoice
