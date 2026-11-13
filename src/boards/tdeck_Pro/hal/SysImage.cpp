#include "SysImage.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

// Arduino's start-up confirms a new image at once unless this says "later". The app calls
// sysimg::markValid() itself once the new firmware has booted and run for a while, so a
// firmware that crashes early is rolled back by the bootloader.
extern "C" bool verifyRollbackLater() { return true; }

namespace sysimg {
namespace {

constexpr const char* kOtaNs = "rs_ota";
esp_ota_handle_t g_ota = 0;
const esp_partition_t* g_target = nullptr;

void say(char* why, size_t cap, const char* s) { if (why && cap) snprintf(why, cap, "%s", s); }

const esp_partition_t* appSlot(esp_partition_subtype_t sub) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, sub, nullptr);
}

}  // namespace

// ---- NVS --------------------------------------------------------------------------------
int nvsForEach(NvsVisit visit, void* ctx) {
    nvs_iterator_t it = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY);
    int n = 0;
    nvs_handle_t h = 0;
    char openNs[NVS_KEY_NAME_MAX_SIZE] = "";
    uint8_t* buf = nullptr;
    size_t bufCap = 0;
    bool stop = false;
    while (it && !stop) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strcmp(openNs, info.namespace_name)) {
            if (h) { nvs_close(h); h = 0; }
            snprintf(openNs, sizeof(openNs), "%s", info.namespace_name);
            if (nvs_open(openNs, NVS_READONLY, &h) != ESP_OK) h = 0;
        }
        if (h) {
            uint8_t num[8] = {};
            size_t len = 0;
            const uint8_t* data = num;
            esp_err_t err = ESP_FAIL;
            switch (info.type) {
            case NVS_TYPE_U8:  err = nvs_get_u8(h, info.key, (uint8_t*)num);   len = 1; break;
            case NVS_TYPE_I8:  err = nvs_get_i8(h, info.key, (int8_t*)num);    len = 1; break;
            case NVS_TYPE_U16: err = nvs_get_u16(h, info.key, (uint16_t*)num); len = 2; break;
            case NVS_TYPE_I16: err = nvs_get_i16(h, info.key, (int16_t*)num);  len = 2; break;
            case NVS_TYPE_U32: err = nvs_get_u32(h, info.key, (uint32_t*)num); len = 4; break;
            case NVS_TYPE_I32: err = nvs_get_i32(h, info.key, (int32_t*)num);  len = 4; break;
            case NVS_TYPE_U64: err = nvs_get_u64(h, info.key, (uint64_t*)num); len = 8; break;
            case NVS_TYPE_I64: err = nvs_get_i64(h, info.key, (int64_t*)num);  len = 8; break;
            case NVS_TYPE_STR:
            case NVS_TYPE_BLOB: {
                size_t need = 0;
                err = info.type == NVS_TYPE_STR ? nvs_get_str(h, info.key, nullptr, &need)
                                                : nvs_get_blob(h, info.key, nullptr, &need);
                if (err != ESP_OK) break;
                if (need + 1 > bufCap) {
                    free(buf);
                    bufCap = need + 1;
                    buf = (uint8_t*)malloc(bufCap);
                    if (!buf) { bufCap = 0; err = ESP_ERR_NO_MEM; break; }
                }
                len = need;
                err = info.type == NVS_TYPE_STR ? nvs_get_str(h, info.key, (char*)buf, &len)
                                                : nvs_get_blob(h, info.key, buf, &len);
                if (info.type == NVS_TYPE_STR && len) len--;      // drop the NUL
                data = buf;
                break;
            }
            default: break;
            }
            if (err == ESP_OK) {
                n++;
                if (!visit(info.namespace_name, info.key, (uint8_t)info.type, data, len, ctx)) stop = true;
            }
        }
        it = nvs_entry_next(it);
    }
    if (it) nvs_release_iterator(it);
    if (h) nvs_close(h);
    free(buf);
    return n;
}

bool nvsEraseNamespace(const char* ns) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool nvsPut(const char* ns, const char* key, uint8_t type, const uint8_t* d, size_t len) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    uint64_t v = 0;
    if (len <= 8 && type != T_STR && type != T_BLOB) memcpy(&v, d, len);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    switch (type) {
    case T_U8:  if (len == 1) err = nvs_set_u8(h, key, (uint8_t)v); break;
    case T_I8:  if (len == 1) err = nvs_set_i8(h, key, (int8_t)v); break;
    case T_U16: if (len == 2) err = nvs_set_u16(h, key, (uint16_t)v); break;
    case T_I16: if (len == 2) err = nvs_set_i16(h, key, (int16_t)v); break;
    case T_U32: if (len == 4) err = nvs_set_u32(h, key, (uint32_t)v); break;
    case T_I32: if (len == 4) err = nvs_set_i32(h, key, (int32_t)v); break;
    case T_U64: if (len == 8) err = nvs_set_u64(h, key, v); break;
    case T_I64: if (len == 8) err = nvs_set_i64(h, key, (int64_t)v); break;
    case T_STR: {
        char* s = (char*)malloc(len + 1);
        if (!s) break;
        memcpy(s, d, len);
        s[len] = 0;
        err = nvs_set_str(h, key, s);
        free(s);
        break;
    }
    case T_BLOB: err = nvs_set_blob(h, key, d, len); break;
    default: break;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

// ---- Firmware image -----------------------------------------------------------------------
const char* runningSlot() {
    const esp_partition_t* r = esp_ota_get_running_partition();
    return (r && r->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) ? "app1" : "app0";
}

bool otaCheck(uint32_t size, char* why, size_t cap) {
    // The rsDeck launcher layout keeps the RNode firmware in a third slot: the "other" slot
    // there is not a spare, so only the plain two-slot layout is updated.
    if (appSlot(ESP_PARTITION_SUBTYPE_APP_OTA_2)) { say(why, cap, "launcher layout: use USB"); return false; }
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (!next) { say(why, cap, "no second app slot"); return false; }
    if (size < 64 * 1024) { say(why, cap, "file too small"); return false; }
    if (size > next->size) { say(why, cap, "file too big"); return false; }
    return true;
}

bool otaBegin(uint32_t size, char* why, size_t cap) {
    if (!otaCheck(size, why, cap)) return false;
    g_target = esp_ota_get_next_update_partition(nullptr);
    const esp_err_t err = esp_ota_begin(g_target, size, &g_ota);
    if (err != ESP_OK) { say(why, cap, esp_err_to_name(err)); g_ota = 0; return false; }
    Serial.printf("[OTA] writing %lu bytes to %s\n", (unsigned long)size, g_target->label);
    return true;
}

bool otaWrite(const uint8_t* d, size_t n) { return g_ota && esp_ota_write(g_ota, d, n) == ESP_OK; }

void otaAbort() {
    if (g_ota) esp_ota_abort(g_ota);
    g_ota = 0;
}

bool otaEnd(char* why, size_t cap) {
    if (!g_ota) { say(why, cap, "not started"); return false; }
    esp_err_t err = esp_ota_end(g_ota);          // checks the image (header, chip, SHA-256)
    g_ota = 0;
    if (err != ESP_OK) { say(why, cap, err == ESP_ERR_OTA_VALIDATE_FAILED ? "not a valid image" : esp_err_to_name(err)); return false; }
    // Remember app0 as it is now when the new image goes to app1: if app0 changes later,
    // it was flashed over USB and bootCheck() boots it.
    Preferences p;
    if (p.begin(kOtaNs, false)) {
        uint8_t sha[32];
        const esp_partition_t* a0 = appSlot(ESP_PARTITION_SUBTYPE_APP_OTA_0);
        if (g_target->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 && a0 &&
            esp_partition_get_sha256(a0, sha) == ESP_OK)
            p.putBytes("app0sha", sha, sizeof(sha));
        else
            p.remove("app0sha");
        p.end();
    }
    err = esp_ota_set_boot_partition(g_target);
    if (err != ESP_OK) { say(why, cap, esp_err_to_name(err)); return false; }
    Serial.printf("[OTA] %s boots next\n", g_target->label);
    return true;
}

void bootCheck() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    Serial.printf("[OTA] running from %s%s\n", run ? run->label : "?", pendingVerify() ? " (new, not confirmed yet)" : "");
    if (!run || run->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1) return;
    Preferences p;
    uint8_t want[32], have[32];
    bool known = false;
    if (p.begin(kOtaNs, true)) { known = p.getBytes("app0sha", want, sizeof(want)) == sizeof(want); p.end(); }
    if (!known) return;
    const esp_partition_t* a0 = appSlot(ESP_PARTITION_SUBTYPE_APP_OTA_0);
    if (!a0 || esp_partition_get_sha256(a0, have) != ESP_OK) return;    // app0 not a valid image
    if (!memcmp(want, have, sizeof(want))) return;
    Serial.println("[OTA] app0 was flashed over USB after the SD update: booting it");
    if (esp_ota_set_boot_partition(a0) != ESP_OK) return;
    if (p.begin(kOtaNs, false)) { p.remove("app0sha"); p.end(); }
    Serial.flush();
    esp_restart();
}

bool pendingVerify() {
    const esp_partition_t* run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

void markValid() {
    if (!pendingVerify()) return;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) Serial.println("[OTA] new firmware confirmed");
}

}  // namespace sysimg
