#pragma once
// Remote console over WiFi (Settings > WiFi > Remote console; console "netcon").
//
// While on and WiFi is connected, the unit dials out to a relay (tools/netcon/relay.py,
// normally on the build VM) and carries the serial console both ways: everything the
// firmware prints is copied to the relay, and lines the relay sends are run like lines
// typed on the USB console (the lock rules still apply). Off by default; settings in NVS
// "rs_ncon" (on, host, port, key).
//
// The unit makes the connection, so nothing on the PC or the phone has to accept one.
// Both ends prove they know the shared key (HMAC-SHA256 over two random nonces) before
// any console data flows; the data itself is not encrypted (home network).
//
//   unit  -> relay   RSCON1 <name> <dest> <nonceU>
//   relay -> unit    AUTH <nonceR> <hmac(key, "relay|" nonceU "|" nonceR)>
//   unit  -> relay   OK <hmac(key, "unit|" nonceR "|" nonceU)>
//   then the console, both ways, as text lines.
//
// Output is kept in a 24 KB ring (PSRAM) while the setting is on, so a new connection
// starts with the most recent console lines.
#include <stddef.h>
#include <stdint.h>

// Writes bytes to the console so the WiFi copy gets them too. A plain Serial.write() on
// the Serial object is bound by the compiler straight to HWCDC::write (the object's type
// is known), skipping the output copy; going through a pointer the compiler cannot see
// through keeps the call virtual. printf / print / println are not affected.
#if defined(SIMULATOR)
#define consoleWrite(b, n) Serial.write((b), (n))
#else
#include <Arduino.h>
inline void consoleWrite(const uint8_t* b, size_t n) {
    Print* volatile p = &Serial;
    p->write(b, n);
}
#endif

namespace netcon {

constexpr uint16_t kDefaultPort = 4404;

struct Cfg {
    bool on = false;
    char host[64] = "";
    uint16_t port = kDefaultPort;
    char key[48] = "";
};

#if defined(SIMULATOR)
inline void load() {}
inline Cfg cfg() { return Cfg(); }
inline void set(const Cfg&) {}
inline void tick(bool, const char*, const char*) {}
inline int read() { return -1; }
inline const char* status() { return "not in simulator"; }
inline uint32_t sentKb() { return 0; }
inline uint32_t connects() { return 0; }
#else
void load();                    // NVS -> settings; installs the output copy when on
Cfg cfg();                      // current settings (copy)
void set(const Cfg& c);         // save to NVS and apply (reconnects)
// Main loop: WiFi state and this unit's name / address (sent in the greeting).
void tick(bool wifiUp, const char* name, const char* dest);
int read();                     // next byte the relay sent, -1 = none
const char* status();           // short status for the settings page / console
uint32_t sentKb();              // console KB sent since boot
uint32_t connects();            // connections made since boot
#endif

}  // namespace netcon
