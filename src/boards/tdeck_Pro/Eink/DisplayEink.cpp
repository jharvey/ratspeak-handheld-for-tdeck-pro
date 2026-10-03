#include "DisplayEink.h"
#include <string.h>
#include <GxEPD2_BW.h>
#include <gdeq/GxEPD2_310_GDEQ031T10.h>

// Single display instance for this board
static GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT>
    g_epd(GxEPD2_310_GDEQ031T10(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

DisplayEink::DisplayEink() : framebuffer_(nullptr), ready_(false) {}

bool DisplayEink::begin() {
    pinMode(BOARD_1V8_EN, OUTPUT);
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_1V8_EN, HIGH);
    digitalWrite(BOARD_LORA_EN, HIGH);
    delay(50);

    // Isolate other SPI devices
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);
#ifdef SD_CS
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
#endif

    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    const size_t fb_bytes = (size_t)EPD_WIDTH * EPD_HEIGHT / 8;
    framebuffer_ = (uint8_t*)ps_malloc(fb_bytes);
    if (!framebuffer_) {
        framebuffer_ = (uint8_t*)malloc(fb_bytes);
        if (framebuffer_) Serial.println("[EINK] using internal heap for FB");
    }
    if (!framebuffer_) {
        Serial.printf("[EINK] framebuffer alloc failed (%u bytes)\n", (unsigned)fb_bytes);
        return false;
    }
    memset(framebuffer_, 0xFF, fb_bytes); // white
    Serial.printf("[EINK] framebuffer %u bytes OK\n", (unsigned)fb_bytes);

    Serial.println("[EINK] init GxEPD2...");
    g_epd.init(115200, true, 50, false);
    g_epd.setRotation(0);
    ready_ = true;
    Serial.println("[EINK] DisplayEink ready (GxEPD2)");
    return true;
}

void DisplayEink::fillScreen(bool black) {
    if (!framebuffer_) return;
    memset(framebuffer_, black ? 0x00 : 0xFF, (size_t)EPD_WIDTH * EPD_HEIGHT / 8);
}

void DisplayEink::setPixel(uint16_t x, uint16_t y, bool black) {
    if (!framebuffer_ || x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
    uint32_t idx = (y * EPD_WIDTH + x) / 8;
    uint8_t  mask = 0x80 >> (x % 8);
    if (black) framebuffer_[idx] &= ~mask;
    else       framebuffer_[idx] |=  mask;
}

void DisplayEink::fullRefresh() {
    if (!ready_ || !framebuffer_) return;

    g_epd.setFullWindow();
    g_epd.firstPage();
    do {
        // GxEPD2 drawBitmap: 1-bit, MSB first, black=0
        g_epd.fillScreen(GxEPD_WHITE);
        g_epd.drawBitmap(0, 0, framebuffer_, EPD_WIDTH, EPD_HEIGHT, GxEPD_BLACK);
    } while (g_epd.nextPage());
}

void DisplayEink::flush(const lv_area_t* area, lv_color_t* color_map) {
    if (!framebuffer_) return;

    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;

    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            lv_color_t c = color_map[y * w + x];
            // Treat near-black as black for mono e-ink
#if LV_COLOR_DEPTH == 1
            bool black = (c.full == 0);
#else
            bool black = (c.ch.red < 16 && c.ch.green < 32 && c.ch.blue < 16);
#endif
            setPixel((uint16_t)(area->x1 + x), (uint16_t)(area->y1 + y), black);
        }
    }

    // Bring-up: always full refresh (partial later)
    fullRefresh();
}