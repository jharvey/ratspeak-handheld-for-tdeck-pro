#pragma once
// Live screen mirror over the USB serial console ("VNC-style" echo for the desktop
// simulator's live-device mode). Off until a host asks for it.
//
// Host -> device (one line each, handled by the serial console):
//   @M on        start mirroring / heartbeat (send at least every 6 s)
//   @M off       stop
//   @M sync      resend the whole screen
//   @K <raw>     TCA8418 event byte: (row*10 + col + 1) | 0x80 when pressed
//   @T <x> <y>   touch at panel coordinates
//
// Device -> host (mixed with the normal log lines):
//   @HELLO <w> <h>                     mirror started, full frame follows
//   @FB <y> <rows> <crc16> <base64>    rows y..y+rows-1, 1 bpp MSB-first, 1 = black
//   @ST <key=value ...>                status (battery etc.), every 2 s
// crc16 is CRC-16/CCITT-FALSE over the raw row bytes, 4 hex digits. A host that sees a
// bad line sends "@M sync".

#include <stddef.h>
#include <stdint.h>

namespace mirror {

void hostCommand(const char* arg);
bool active();

// Call from the main loop. Sends the rows of fb that changed since the last call
// (throttled, a bounded number of rows per call) and the status line.
void tick(const uint8_t* fb, uint16_t width, uint16_t height, const char* status);

// Exposed for the host side and tests.
uint16_t crc16(const uint8_t* d, size_t n);

}  // namespace mirror
