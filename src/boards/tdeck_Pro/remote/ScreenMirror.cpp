#include "remote/ScreenMirror.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

namespace mirror {
namespace {

constexpr uint32_t kHostTimeoutMs = 6000;   // no heartbeat for this long -> stop
constexpr uint32_t kTickMs = 60;
constexpr uint32_t kStatusMs = 2000;
constexpr int kBandRows = 16;               // rows per @FB line
constexpr int kMaxBandsPerTick = 6;         // bound the time one loop pass can spend

bool g_on = false;
bool g_resync = true;
uint32_t g_lastHost = 0, g_lastTick = 0, g_lastStatus = 0;
uint8_t* g_sent = nullptr;                  // what the host has
size_t g_sentSize = 0;

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64(const uint8_t* in, size_t n, char* out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) |
                           (i + 2 < n ? in[i + 2] : 0);
        out[o++] = kB64[v >> 18 & 63];
        out[o++] = kB64[v >> 12 & 63];
        out[o++] = i + 1 < n ? kB64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? kB64[v & 63] : '=';
    }
    return o;
}

void sendBand(const uint8_t* fb, int stride, int y, int rows) {
    static char line[48 + (kBandRows * 64 * 4) / 3 + 8];
    const uint8_t* p = fb + (size_t)y * stride;
    const size_t n = (size_t)rows * stride;
    int len = snprintf(line, sizeof(line), "@FB %d %d %04X ", y, rows, crc16(p, n));
    len += (int)b64(p, n, line + len);
    line[len++] = '\n';
    Serial.write((const uint8_t*)line, len);
    memcpy(g_sent + (size_t)y * stride, p, n);
}

}  // namespace

uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*d++) << 8;
        for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)(c << 1 ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

bool active() { return g_on; }

void hostCommand(const char* arg) {
    while (*arg == ' ') arg++;
    if (!strcmp(arg, "on")) {
        if (!g_on) g_resync = true;
        g_on = true;
        g_lastHost = millis();
    } else if (!strcmp(arg, "off")) {
        g_on = false;
    } else if (!strcmp(arg, "sync")) {
        g_resync = true;
        g_lastHost = millis();
    }
}

void tick(const uint8_t* fb, uint16_t width, uint16_t height, const char* status) {
    if (!g_on || !fb) return;
    const uint32_t now = millis();
    if (now - g_lastHost > kHostTimeoutMs) {   // host went away: stop writing frames
        g_on = false;
        Serial.println("[MIRROR] host timeout - stopped");
        return;
    }
    if (now - g_lastTick < kTickMs) return;
    g_lastTick = now;

    const int stride = width / 8;
    const size_t size = (size_t)stride * height;
    if (g_sentSize != size) {
        free(g_sent);
        g_sent = (uint8_t*)malloc(size);
        g_sentSize = g_sent ? size : 0;
        if (!g_sent) { g_on = false; return; }
        g_resync = true;
    }
    if (g_resync) {
        g_resync = false;
        // Make every row differ from fb so the whole screen goes out (a few bands per tick).
        for (size_t i = 0; i < size; i++) g_sent[i] = (uint8_t)~fb[i];
        Serial.printf("@HELLO %u %u\n", (unsigned)width, (unsigned)height);
        g_lastStatus = 0;
    }

    int bands = 0;
    for (int y = 0; y < height && bands < kMaxBandsPerTick;) {
        if (!memcmp(fb + (size_t)y * stride, g_sent + (size_t)y * stride, stride)) { y++; continue; }
        int rows = 1;
        while (y + rows < height && rows < kBandRows &&
               memcmp(fb + (size_t)(y + rows) * stride, g_sent + (size_t)(y + rows) * stride, stride))
            rows++;
        sendBand(fb, stride, y, rows);
        bands++;
        y += rows;
    }

    if (status && now - g_lastStatus >= kStatusMs) {
        g_lastStatus = now;
        Serial.printf("@ST %s\n", status);
    }
}

}  // namespace mirror
