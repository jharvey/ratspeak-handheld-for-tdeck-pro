#include "UsbDrive.h"

#include <Arduino.h>
#include <SPI.h>
#include <USB.h>
#include <USBMSC.h>
#include <sd_diskio.h>
#include <esp_system.h>
#include <driver/periph_ctrl.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/usb_serial_jtag_reg.h>

#include "hal/SharedSPIBus.h"
#include "ff.h"
#include "diskio.h"

// Card start-up from the SD library (sd_diskio.cpp, not in its header). sdcard_init() only
// registers the card; FatFs normally starts it at mount, which this mode never does.
uint8_t ff_sd_initialize(uint8_t pdrv);
// Multi-sector transfers (CMD18 / CMD25): one SD command per TinyUSB buffer (8 sectors)
// instead of one per sector. Same library file, same reason.
DRESULT ff_sd_read(uint8_t pdrv, uint8_t* buffer, DWORD sector, UINT count);
DRESULT ff_sd_write(uint8_t pdrv, const uint8_t* buffer, DWORD sector, UINT count);

namespace usbdrive {
namespace {

USBMSC g_msc;
uint8_t g_pdrv = 0xFF;
uint32_t g_sectors = 0;
volatile State g_state = OFF;
volatile uint32_t g_readKb = 0, g_writtenKb = 0, g_lastWriteMs = 0;
volatile uint32_t g_readB = 0, g_writtenB = 0;
const char* g_msg = "off";
constexpr uint32_t kSector = 512;

// TinyUSB asks for whole sectors (bufsize is a multiple of 512, offset 0 with a 512-byte
// block size): one SD multi-block transfer per call on the shared SPI bus.
int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    if (offset || (bufsize % kSector)) return -1;
    SharedSPILock lock(pdMS_TO_TICKS(2000));
    if (!lock.locked()) return -1;
    if (ff_sd_read(g_pdrv, static_cast<uint8_t*>(buffer), lba, bufsize / kSector) != RES_OK) return -1;
    g_readB += bufsize;
    g_readKb = g_readB >> 10;
    return (int32_t)bufsize;
}

int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    if (offset || (bufsize % kSector)) return -1;
    SharedSPILock lock(pdMS_TO_TICKS(2000));
    if (!lock.locked()) return -1;
    if (ff_sd_write(g_pdrv, buffer, lba, bufsize / kSector) != RES_OK) return -1;
    g_writtenB += bufsize;
    g_writtenKb = g_writtenB >> 10;
    g_lastWriteMs = millis();
    if (g_state == EJECTED) g_state = ACTIVE;     // the PC mounted it again
    return (int32_t)bufsize;
}

// "Eject" / "Safely remove" on the PC: stop with load_eject.
bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
    (void)power_condition;
    if (load_eject) {
        if (!start) { g_state = EJECTED; g_msg = "ejected"; }
        else        { g_state = ACTIVE;  g_msg = "on the PC"; }
    }
    return true;
}

}  // namespace

bool start(int csPin) {
    if (g_state == ACTIVE || g_state == EJECTED) return true;
    {
        SharedSPILock lock;
        pinMode(csPin, OUTPUT);
        digitalWrite(csPin, HIGH);
        g_pdrv = sdcard_init((uint8_t)csPin, &SPI, 16000000);
        if (g_pdrv == 0xFF) g_pdrv = sdcard_init((uint8_t)csPin, &SPI, 4000000);
        if (g_pdrv == 0xFF) { g_state = FAILED; g_msg = "no SD card"; return false; }
        if (ff_sd_initialize(g_pdrv) != 0) {          // 0 = ready (no STA_NOINIT / STA_NODISK)
            sdcard_uninit(g_pdrv);
            g_pdrv = 0xFF;
            g_state = FAILED;
            g_msg = "no SD card";
            return false;
        }
        g_sectors = sdcard_num_sectors(g_pdrv);
        if (!g_sectors || sdcard_sector_size(g_pdrv) != kSector) {
            sdcard_uninit(g_pdrv);
            g_pdrv = 0xFF;
            g_state = FAILED;
            g_msg = "card not readable";
            return false;
        }
    }
    g_msc.vendorID("RatSpeak");          // 8 characters max
    g_msc.productID("T-Deck SD card");   // 16 max
    g_msc.productRevision("1.0");
    g_msc.onRead(onRead);
    g_msc.onWrite(onWrite);
    g_msc.onStartStop(onStartStop);
    g_msc.mediaPresent(true);
    if (!g_msc.begin(g_sectors, kSector)) { g_state = FAILED; g_msg = "USB drive start failed"; return false; }
    USB.manufacturerName("RatSpeak");
    USB.productName("T-Deck Pro SD card");
    Serial.printf("[USB] drive mode: %lu MB card, the COM port goes away until restart\n",
                  (unsigned long)(g_sectors / 2048));
    Serial.flush();
    delay(50);
    if (!USB.begin()) { g_state = FAILED; g_msg = "USB start failed"; return false; }
    g_state = ACTIVE;
    g_msg = "on the PC";
    return true;
}

State state() { return g_state; }
const char* message() { return g_msg; }
uint32_t sectors() { return g_sectors; }
uint32_t readKb() { return g_readKb; }
uint32_t writtenKb() { return g_writtenKb; }
uint32_t lastWriteMs() { return g_lastWriteMs; }

void restartToNormal() {
    // Give the USB pins back to USB-Serial/JTAG (as the core does before a restart into the
    // ROM loader): detach OTG, hold D-/D+ low so the PC sees the drive leave, then enable
    // the Serial/JTAG pads. After the restart the PC finds the COM port again.
    periph_module_reset(PERIPH_USB_MODULE);
    periph_module_disable(PERIPH_USB_MODULE);
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                        RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PHY_SEL);
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
    pinMode(19, OUTPUT_OPEN_DRAIN);      // USB D- / D+ on the ESP32-S3
    pinMode(20, OUTPUT_OPEN_DRAIN);
    digitalWrite(19, LOW);
    digitalWrite(20, LOW);
    delay(200);
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
    esp_restart();
}

}  // namespace usbdrive
