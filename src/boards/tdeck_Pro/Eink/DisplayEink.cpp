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
    : framebuffer_(nullptr), shadow_(nullptr), lastRefreshMs_(0), ready_(false), dirty_(false), partialCount_(0) {}

void DisplayEink::parkSpiCs() {
    digitalWrite(LORA_CS, HIGH);
#ifdef SD_CS
    digitalWrite(SD_CS, HIGH);
#endif
}

bool DisplayEink::begin() {
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
    // Shadow of what the panel shows; lets partial updates send only changed rows.
    shadow_ = (uint8_t*)ps_malloc(fb_bytes);
    if (!shadow_) shadow_ = (uint8_t*)malloc(fb_bytes);
    if (shadow_) memset(shadow_, 0xFF, fb_bytes);   // unknown: forces first update
    Serial.printf("[EINK] framebuffer %u bytes OK free_int=%u\n",
                  (unsigned)fb_bytes,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    Serial.println("[EINK] init GxEPD2...");
    g_epd.init(0, true, 2, false);
    g_epd.setRotation(0);
    ready_ = true;
    dirty_ = false;
    partialCount_ = 0;
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
            bool black = (c.full != white.full);
            setPixel((uint16_t)(area->x1 + x),
                     (uint16_t)(area->y1 + y), black);
        }
    }
    dirty_ = true;
}

// Panel RAM uses 1 = white; framebuffer_ uses 1 = black, so images are written inverted.
// Writing straight to the controller replaces GxEPD2's paged drawBitmap path, which
// redrew all 76,800 pixels per 16-line page, twice per partial update (~3M drawPixel
// calls) before the panel even started refreshing.
static constexpr int kRowBytes = EPD_WIDTH / 8;
static constexpr size_t kFbBytes = (size_t)EPD_WIDTH * EPD_HEIGHT / 8;

void DisplayEink::setBusyCallback(void (*cb)(const void*), const void* param) {
    g_epd.epd2.setBusyCallback(cb, param);
}

void DisplayEink::fullRefresh() {
    if (!ready_ || !framebuffer_) return;
    const uint32_t t0 = millis();
    parkSpiCs();
    g_epd.epd2.writeImageForFullRefresh(framebuffer_, 0, 0, EPD_WIDTH, EPD_HEIGHT, true);
    g_epd.epd2.refresh(false);
    if (shadow_) memcpy(shadow_, framebuffer_, kFbBytes);
    dirty_ = false;
    partialCount_ = 0;
    lastRefreshMs_ = millis() - t0;
    Serial.printf("[EINK] full %ums\n", (unsigned)lastRefreshMs_);
}

void DisplayEink::partialRefresh(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!ready_ || !framebuffer_) return;
    if (w == 0 || h == 0 || x >= EPD_WIDTH || y >= EPD_HEIGHT) return;
    if (y + h > EPD_HEIGHT) h = EPD_HEIGHT - y;
    (void)x; (void)w;   // window is widened to full rows (byte-aligned, cheap)

    // Narrow to the rows that actually changed.
    int first = -1, last = -1;
    for (int row = y; row < y + h; row++) {
        const size_t off = (size_t)row * kRowBytes;
        if (!shadow_ || memcmp(framebuffer_ + off, shadow_ + off, kRowBytes) != 0) {
            if (first < 0) first = row;
            last = row;
        }
    }
    dirty_ = false;
    if (first < 0) { lastRefreshMs_ = 0; return; }   // identical: no panel I/O

    const uint32_t t0 = millis();
    parkSpiCs();
    const uint16_t rows = (uint16_t)(last - first + 1);
    const uint8_t* src = framebuffer_ + (size_t)first * kRowBytes;
    g_epd.epd2.writeImage(src, 0, first, EPD_WIDTH, rows, true);
    g_epd.epd2.refresh(0, first, EPD_WIDTH, rows);
    g_epd.epd2.writeImageAgain(src, 0, first, EPD_WIDTH, rows, true);
    if (shadow_) memcpy(shadow_ + (size_t)first * kRowBytes, src, (size_t)rows * kRowBytes);

    partialCount_++;
    lastRefreshMs_ = millis() - t0;
    Serial.printf("[EINK] partial rows %d-%d %ums\n", first, last, (unsigned)lastRefreshMs_);
}

void DisplayEink::refreshIfDirty() {
    if (!dirty_) return;
    partialRefresh(0, 0, EPD_WIDTH, EPD_HEIGHT);
}
