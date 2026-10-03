#include <Arduino.h>
#include <stdlib.h>
#include <lvgl.h>

// Custom LVGL memory hooks expected by this project's lv_conf / patched LVGL.
// Full UI provides richer versions; these are enough for e-ink bring-up.

extern "C" {

void* handheld_lvgl_alloc(size_t size) {
    // Prefer PSRAM when available
    void* p = ps_malloc(size);
    if (!p) p = malloc(size);
    return p;
}

void handheld_lvgl_free(void* p) {
    if (p) free(p);
}

void* handheld_lvgl_realloc(void* p, size_t size) {
    // Simple realloc path
    void* n = handheld_lvgl_alloc(size);
    if (n && p) {
        // best-effort copy; size of old block unknown — LVGL usually grows
        memcpy(n, p, size); // may read past old block briefly; OK for bring-up
        handheld_lvgl_free(p);
    } else if (p && size == 0) {
        handheld_lvgl_free(p);
        return nullptr;
    }
    return n;
}

void handheld_lvgl_fail(const char* msg) {
    Serial.printf("[LVGL-FAIL] %s\n", msg ? msg : "(null)");
    // Do not hang forever during bring-up
}

} // extern "C"

// Font symbol referenced as default in this tree's LVGL config.
// Alias to a built-in Montserrat font so the linker is satisfied.
extern const lv_font_t lv_font_montserrat_14;
const lv_font_t lv_font_rsdeck_14 = lv_font_montserrat_14;