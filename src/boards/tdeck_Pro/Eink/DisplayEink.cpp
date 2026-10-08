#include "DisplayEink.h"
#include <string.h>
#include <GxEPD2_BW.h>
#include <gdeq/GxEPD2_310_GDEQ031T10.h>
#include <esp_heap_caps.h>

// Small page height keeps GxEPD2 static RAM tiny after the 200KB protocol node.
// Full frame lives in framebuffer_; we push it with firstPage/drawBitmap.
static GxEPD2_BW<GxEPD2_310_GDEQ031T10, 16>
    g_epd(GxEPD2_310_GDEQ031T10(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

DisplayEink::DisplayEink()
    : framebuffer_(nullptr), ready_(false), dirty_(false) {}

bool DisplayEink::begin() {
    // Power + SPI already brought up by main (radio path).
    // Only park CS lines so the panel owns the bus for init/refresh.
    pinMode(EPD_CS, OUTPUT);
    digitalWrite(EPD_CS, HIGH);
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);
#ifdef SD_CS
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
#endif

    const size_t fb_bytes = (size_t)EPD_WIDTH * EPD_HEIGHT / 8;  // 9600
    framebuffer_ = (uint8_t*)ps_malloc(fb_bytes);
    if (!framebuffer_) {
        framebuffer_ = (uint8_t*)malloc(fb_bytes);
        if (framebuffer_) {
            Serial.println("[EINK] using internal heap for FB");
        }
    }
    if (!framebuffer_) {
        Serial.printf("[EINK] framebuffer alloc failed (%u bytes)\n",
                      (unsigned)fb_bytes);
        return false;
    }
    // bit 1 = black (Adafruit_GFX / GxEPD2 drawBitmap)
    memset(framebuffer_, 0x00, fb_bytes);
    Serial.printf("[EINK] framebuffer %u bytes OK free_int=%u\n",
                  (unsigned)fb_bytes,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    Serial.println("[EINK] init GxEPD2...");
    // quiet init, 2 ms reset pulse
    g_epd.init(0, true, 2, false);
    g_epd.setRotation(0);
    ready_ = true;
    dirty_ = false;
    Serial.printf("[EINK] DisplayEink ready free_int=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

void DisplayEink::fillScreen(bool black) {
    if (!framebuffer_) return;
    memset(framebuffer_, black ? 0xFF : 0x00,
           (size_t)EPD_WIDTH * EPD_HEIGHT / 8);
    dirty_ = true;
}

void DisplayEink::setPixel(uint16_t x, uint16_t y, bool black) {
    if (!framebuffer_ || x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
    uint32_t idx = (y * EPD_WIDTH + x) / 8;
    uint8_t  mask = 0x80 >> (x % 8);
    if (black) framebuffer_[idx] |=  mask;
    else       framebuffer_[idx] &= ~mask;
}

void DisplayEink::flush(const lv_area_t* area, lv_color_t* color_map) {
    if (!framebuffer_) return;

    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    const lv_color_t white = lv_color_white();

    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            lv_color_t c = color_map[y * w + x];
            // Anything not pure white → black. Catches AA font edges under
            // LV_COLOR_16_SWAP; pure black (0) still works.
            bool black = (c.full != white.full);
            setPixel((uint16_t)(area->x1 + x),
                     (uint16_t)(area->y1 + y), black);
        }
    }
    dirty_ = true;
}

void DisplayEink::fullRefresh() {
    if (!ready_ || !framebuffer_) return;

    digitalWrite(LORA_CS, HIGH);
#ifdef SD_CS
    digitalWrite(SD_CS, HIGH);
#endif

    // Diagnostic: how many black bits did LVGL paint?
    const size_t fb_bytes = (size_t)EPD_WIDTH * EPD_HEIGHT / 8;
    size_t bits = 0;
    for (size_t i = 0; i < fb_bytes; i++) {
        bits += (size_t)__builtin_popcount((unsigned)framebuffer_[i]);
    }
    Serial.printf("[EINK] fullRefresh bits_set=%u/%u\n",
                  (unsigned)bits, (unsigned)(fb_bytes * 8));

    // Phase D path: firstPage + drawBitmap (1 = black). Works with page_height 16.
    g_epd.setFullWindow();
    g_epd.firstPage();
    do {
        g_epd.fillScreen(GxEPD_WHITE);
        g_epd.drawBitmap(0, 0, framebuffer_, EPD_WIDTH, EPD_HEIGHT, GxEPD_BLACK);
    } while (g_epd.nextPage());

    dirty_ = false;
    Serial.println("[EINK] fullRefresh done");
}

void DisplayEink::refreshIfDirty() {
    if (dirty_) fullRefresh();
}