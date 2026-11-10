#pragma once
// Power log kept in RTC slow memory (RTC_NOINIT): it survives software, watchdog and panic
// resets (not a power loss), so a run on battery can be read back over USB afterwards,
// even if the device reset in between. Each boot adds a marker with the reset reason.
// Console: "rl" dumps it, "rl 0" clears it.
#include <stdint.h>

namespace rtclog {

void boot(int resetReason);                                   // call once early in setup
void sample(uint16_t mv, int16_t ma, bool usb, uint32_t sleeps, uint32_t sleptS);
void dump();
void clear();

}  // namespace rtclog
