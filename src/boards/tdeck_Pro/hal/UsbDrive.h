#pragma once
// USB drive mode: the SD card shows up on the PC as a USB drive (mass storage), so maps,
// help texts and packs can be dragged on without a card reader.
//
// The ESP32-S3 has one USB port and two USB blocks behind it: USB-Serial/JTAG (the COM
// port used for the console and for flashing) and USB OTG (TinyUSB). start() moves the
// port to OTG and offers the card as a drive; the COM port disappears from the PC until
// the unit restarts. restartToNormal() hands the port back to USB-Serial/JTAG and
// restarts, so the next boot is a normal one with the console back.
//
// The caller must unmount the card first (the firmware must not use the file system while
// the PC writes to it) and keep it unmounted until the restart.
#include <stdint.h>

namespace usbdrive {

enum State : uint8_t {
    OFF,        // normal: the card belongs to the firmware
    ACTIVE,     // the PC sees the drive
    EJECTED,    // the PC ejected the drive (safe to restart)
    FAILED      // card or USB start failed
};

bool start(int csPin);          // card must be unmounted; false = FAILED (see message())
State state();
const char* message();          // short status / failure text
uint32_t sectors();             // card size in 512-byte sectors
uint32_t readKb();              // KB the PC read / wrote since start()
uint32_t writtenKb();
uint32_t lastWriteMs();         // millis() of the last write (0 = none)
[[noreturn]] void restartToNormal();
#if defined(SIMULATOR)
void simEject();                // console "usbeject": plays the PC's Eject
#endif

}  // namespace usbdrive
