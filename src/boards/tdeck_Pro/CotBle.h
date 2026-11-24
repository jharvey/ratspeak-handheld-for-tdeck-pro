#pragma once
// ATAK feed over Bluetooth Low Energy: the same CoT XML events as the WiFi feed (CotFeed.h),
// carried to a phone over BLE instead of UDP. A phone app (RatSpeak ATAK bridge, tools/android)
// connects, pairs with the 6-digit code this radio shows, and hands each event to ATAK on the
// same phone (localhost UDP); events ATAK sends come back the same way.
//
// GATT: the Nordic UART Service, so generic BLE tools can watch it too.
//   service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   RX 6E400002 (phone writes) / TX 6E400003 (radio notifies)
// Stream: each event is the XML ("<?xml ...<event ...>...</event>") followed by '\n', cut into
// notifications of (MTU - 3) bytes. The reader splits on "</event>".
// Security: pairing with passkey (this radio displays it, the phone types it), bonded, LE Secure
// Connections; nothing is sent before the link is encrypted and authenticated.
//
// Memory: the Bluetooth controller's RAM is only kept at boot when the ATAK link is set to
// Bluetooth (btInUse() reads the setting); otherwise the Arduino core frees it as before. So a
// switch to Bluetooth takes a restart (back to WiFi does not).
//
// Simulator: a UDP stand-in on 127.0.0.1:<RS_SIM_BLE_PORT> (default 4390); the first datagram
// from a peer "connects" it (no pairing), events go back to that peer.
#include <stddef.h>
#include <stdint.h>

namespace cotble {

enum Event : uint8_t { EV_NONE = 0, EV_PAIR_CODE, EV_CONNECTED, EV_DISCONNECTED, EV_PAIR_FAILED };

bool reserved();                       // controller RAM kept at boot (link = Bluetooth)
bool begin(const char* name);          // start advertising as `name`; false when not reserved / failed
void end();
bool active();                         // begun (advertising or connected)
bool connected();                      // a phone is connected and the link is secure
uint32_t passkey();                    // the pairing code this radio shows
const char* state();                   // "off", "waiting for phone", "pairing", "connected"
bool send(const char* xml, size_t n);  // one event (adds '\n'); false when not connected
int poll(char* buf, size_t cap);       // one received event, zero-terminated; -1 = none
Event takeEvent();                     // pairing / connection news for the screen
void forget();                         // delete all bonded phones
uint32_t txEvents();
uint32_t rxEvents();

}  // namespace cotble

// NVS key the boot check reads (StandaloneApp's CotCfg in namespace rs_cot, key cfg, byte 1).
constexpr uint8_t kCotLinkWifi = 0, kCotLinkBle = 1;
