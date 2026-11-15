#pragma once
// Live walkie-talkie between two units over WiFi.
//
// Set-up goes as an ordinary (end-to-end encrypted) LXMF message, over LoRa or WiFi:
//   "[wt] call <ipv4>:<port> <key, 44 base64>"   caller -> callee
// The callee answers straight over WiFi (UDP to that address); from then on Codec2 3200
// speech goes both ways as UDP datagrams, encrypted with the one-time call key
// (ChaCha20-Poly1305, a nonce per direction and packet). Half duplex: Space toggles talk /
// listen. A background task does the mic -> Codec2 -> network and network -> Codec2 ->
// speaker work, so e-ink refreshes do not break up the sound.
#include <stddef.h>
#include <stdint.h>

class AudioNotify;

namespace walkie {

constexpr uint16_t kPort = 4243;
constexpr const char* kInvitePrefix = "[wt] call ";

enum class State : uint8_t { Idle, Calling, Connected, Ended };

#if !defined(SIMULATOR)
void begin(AudioNotify* audio);
// Caller: opens the call on this unit's address myIp; writes the invite text. apSsid/apPass:
// this unit runs a hotspot for the call (the invite then says "ap=<ssid> <pass>"), else nullptr.
bool startCall(char* invite, size_t cap, uint32_t myIp, const char* apSsid, const char* apPass);
// Callee: parse an invite ("[wt] call ..."); ssid/pass are "" when there is no hotspot.
bool parseInvite(const char* text, uint32_t& ip, uint16_t& port, uint8_t key[32], char* ssid = nullptr,
                 size_t ssidCap = 0, char* pass = nullptr, size_t passCap = 0);
bool answer(uint32_t ip, uint16_t port, const uint8_t key[32]);
void hangUp(const char* why);
void setTalking(bool on);
bool talking();
bool peerTalking();                 // audio from the other side in the last 0.5 s
State state();
const char* status();               // one line for the screen
uint32_t changedAt();               // millis() of the last state change (redraw)
void tick();                        // main loop: time-outs
#else
inline void begin(AudioNotify*) {}
inline bool startCall(char*, size_t, uint32_t, const char*, const char*) { return false; }
inline bool parseInvite(const char*, uint32_t&, uint16_t&, uint8_t*, char* = nullptr, size_t = 0, char* = nullptr,
                        size_t = 0) { return false; }
inline bool answer(uint32_t, uint16_t, const uint8_t*) { return false; }
inline void hangUp(const char*) {}
inline void setTalking(bool) {}
inline bool talking() { return false; }
inline bool peerTalking() { return false; }
inline State state() { return State::Idle; }
inline const char* status() { return "not in the simulator"; }
inline uint32_t changedAt() { return 0; }
inline void tick() {}
#endif

}  // namespace walkie
