#include "UsbDrive.h"

#include <Arduino.h>
#include <SPI.h>
#include <USB.h>
#include <USBMSC.h>
#include <USBCDC.h>
#include "tusb.h"
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

// ---- Drive + COM port ---------------------------------------------------------------------
// Serial is the chip's USB-Serial/JTAG port, which is off the pins in drive mode. Its output
// is sent to a TinyUSB COM port instead: Print's printf / print / println reach write()
// through the object's table of virtual functions, so Serial gets this class's table. Only
// done when the mode starts; the normal firmware never runs this.
USBCDC* g_cdc = nullptr;
bool g_com = false;
SemaphoreHandle_t g_comLock = nullptr;
// Counters of the last drive + COM port run; they survive the restart back to normal
// (console "usbdrive stat"): [0] magic, [1] bytes to the PC, [2] bytes from the PC,
// [3] writes while the PC had the port open, [4] writes that did not fit.
RTC_NOINIT_ATTR uint32_t g_comStat[5];
constexpr uint32_t kStatMagic = 0x44434F4D;

class ConsoleTee : public HWCDC {
public:
    size_t write(const uint8_t* b, size_t n) override {
        if (!g_com || !b || !n || xPortInIsrContext()) return n;
        // Only while a program on the PC has the port open (DTR), as the core's own COM port
        // class does: a transfer queued on a closed port is never collected.
        if (!tud_cdc_n_connected(0)) return n;
        if (xSemaphoreTake(g_comLock, pdMS_TO_TICKS(50)) != pdTRUE) return n;
        g_comStat[3]++;
        size_t done = 0;
        for (int tries = 0; done < n && tries < 20; tries++) {
            done += tud_cdc_n_write(0, b + done, n - done);
            tud_cdc_n_write_flush(0);
            if (done < n) {                          // buffer full: wait only for a PC that reads
                if (!tud_cdc_n_connected(0)) break;
                delay(1);
            }
        }
        g_comStat[1] += done;
        if (done < n) g_comStat[4]++;
        xSemaphoreGive(g_comLock);
        return n;
    }
    size_t write(uint8_t ch) override { return write(&ch, 1); }
};

void consoleToCom() {
    static ConsoleTee tee;
    memcpy(static_cast<void*>(&Serial), static_cast<void*>(&tee), sizeof(void*));
}

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

bool start(int csPin, bool withCom) {
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
    USB.productName(withCom ? "T-Deck Pro SD card + COM" : "T-Deck Pro SD card");
    Serial.printf(withCom ? "[USB] drive mode: %lu MB card, this COM port is replaced by a new one until restart\n"
                          : "[USB] drive mode: %lu MB card, the COM port goes away until restart\n",
                  (unsigned long)(g_sectors / 2048));
    Serial.flush();
    delay(50);
    if (withCom) {
        if (!g_comLock) g_comLock = xSemaphoreCreateMutex();
        if (!g_cdc) g_cdc = new USBCDC(0);           // adds the COM port to the USB description
        // No g_cdc->begin(): without it the core's COM port callbacks do nothing. With it, the
        // USB task stopped answering when the PC opened the port (COM9, 2026-11-27; the core
        // posts an event with no time limit from every callback) and it would restart into the
        // ROM loader on the DTR/RTS pattern of flashing tools. Bytes go through TinyUSB directly.
    }
    if (!USB.begin()) { g_state = FAILED; g_msg = "USB start failed"; return false; }
    if (withCom && g_cdc && g_comLock) {
        g_comStat[0] = kStatMagic; g_comStat[1] = g_comStat[2] = g_comStat[3] = g_comStat[4] = 0;
        g_com = true;
        consoleToCom();
    }
    g_state = ACTIVE;
    g_msg = "on the PC";
    return true;
}

State state() { return g_state; }
bool comOn() { return g_com; }
int comRead() {
    if (!g_com || !tud_cdc_n_available(0)) return -1;
    g_comStat[2]++;
    return tud_cdc_n_read_char(0);
}
const uint32_t* comStat() { return g_comStat[0] == kStatMagic ? g_comStat : nullptr; }
const char* message() { return g_msg; }
uint32_t sectors() { return g_sectors; }
uint32_t readKb() { return g_readKb; }
uint32_t writtenKb() { return g_writtenKb; }
uint32_t lastWriteMs() { return g_lastWriteMs; }

void recoverConsoleAtBoot() {
    // The USB pin choice survives every restart except a hardware reset. A crash or watchdog
    // restart out of drive mode would leave the pins on OTG with nothing behind them: no COM
    // port, no drive. One more restart through the normal hand-back fixes that.
    if (READ_PERI_REG(RTC_CNTL_USB_CONF_REG) & (RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL))
        restartToNormal();
}

void restartToNormal() {
    // Give the USB pins back to USB-Serial/JTAG (as the core does before a restart into the
    // ROM loader): detach OTG, hold D-/D+ low so the PC sees the drive leave, then enable
    // the Serial/JTAG pads. After the restart the PC finds the COM port again.
    if (g_com) {                         // stop console output to the OTG port, then leave the bus
        g_com = false;                   // cleanly while a PC program may still have the port open
        if (g_comLock) xSemaphoreTake(g_comLock, pdMS_TO_TICKS(200));
        tud_disconnect();
        delay(300);
    }
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
