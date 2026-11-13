// Simulator stand-in for hal/SysImage.cpp: NVS comes from the Preferences shim's files;
// the "firmware update" reads the whole image like the unit does and checks it the same
// cheap way (ESP32 image magic byte), then restarts the sim (no second app slot to boot).
#include "hal/SysImage.h"

#include <Arduino.h>
#include <string.h>

namespace sim {
int nvsForEach(bool (*)(const char*, const char*, uint8_t, const uint8_t*, size_t, void*), void*);
bool nvsEraseNamespace(const char* ns);
bool nvsPut(const char* ns, const char* key, uint8_t type, const uint8_t* d, size_t len);
}

namespace sysimg {
namespace {
bool g_open = false;
uint32_t g_size = 0, g_got = 0;
bool g_magicOk = false;
void say(char* why, size_t cap, const char* s) { if (why && cap) snprintf(why, cap, "%s", s); }
}

int nvsForEach(NvsVisit visit, void* ctx) { return sim::nvsForEach(visit, ctx); }
bool nvsEraseNamespace(const char* ns) { return sim::nvsEraseNamespace(ns); }
bool nvsPut(const char* ns, const char* key, uint8_t type, const uint8_t* d, size_t len) {
    return sim::nvsPut(ns, key, type, d, len);
}

const char* runningSlot() { return "app0"; }
bool otaCheck(uint32_t size, char* why, size_t cap) {
    if (size < 64 * 1024) { say(why, cap, "file too small"); return false; }
    if (size > 0x400000) { say(why, cap, "file too big"); return false; }
    return true;
}
bool otaBegin(uint32_t size, char* why, size_t cap) {
    if (!otaCheck(size, why, cap)) return false;
    g_open = true; g_size = size; g_got = 0; g_magicOk = false;
    Serial.printf("[OTA] writing %lu bytes to app1 (sim)\n", (unsigned long)size);
    return true;
}
bool otaWrite(const uint8_t* d, size_t n) {
    if (!g_open) return false;
    if (g_got == 0 && n) g_magicOk = d[0] == 0xE9;
    g_got += n;
    return true;
}
void otaAbort() { g_open = false; }
bool otaEnd(char* why, size_t cap) {
    if (!g_open) { say(why, cap, "not started"); return false; }
    g_open = false;
    if (g_got != g_size || !g_magicOk) { say(why, cap, "not a valid image"); return false; }
    Serial.println("[OTA] app1 boots next (sim)");
    return true;
}
void bootCheck() { Serial.println("[OTA] running from app0"); }
bool pendingVerify() { return false; }
void markValid() {}
}  // namespace sysimg
