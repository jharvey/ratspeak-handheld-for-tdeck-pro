#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include <lvgl.h>
#include "ui/lvgl/LvMemory.h"
#include "ui/lvgl/LvFailure.h"

// Minimal implementations so standalone bring-up can link without the full UI.
// Signatures must match LvMemory.h / LvFailure.h exactly.

static size_t s_retained = 0;
static size_t s_peak = 0;
static uint32_t s_allocations = 0;
static uint32_t s_refused = 0;

extern "C" {

void* handheld_lvgl_alloc(size_t size) {
    if (size == 0) return nullptr;
    void* p = ps_malloc(size);
    if (!p) p = malloc(size);
    if (p) {
        s_allocations++;
        s_retained += size;
        if (s_retained > s_peak) s_peak = s_retained;
    } else {
        s_refused++;
    }
    return p;
}

void handheld_lvgl_free(void* pointer) {
    if (!pointer) return;
    free(pointer);
    // retained is approximate for bring-up
}

void* handheld_lvgl_realloc(void* pointer, size_t size) {
    if (size == 0) {
        handheld_lvgl_free(pointer);
        return nullptr;
    }
    if (!pointer) return handheld_lvgl_alloc(size);

    void* n = ps_realloc(pointer, size);
    if (!n) n = realloc(pointer, size);
    if (!n) s_refused++;
    return n;
}

handheld_lvgl_memory_t handheld_lvgl_memory_stats(void) {
    handheld_lvgl_memory_t s;
    s.retained = s_retained;
    s.peak = s_peak;
    s.allocations = s_allocations;
    s.refused = s_refused;
    return s;
}

void handheld_lvgl_failure_display(void (*show)(void*), void* display) {
    (void)show;
    (void)display;
}

void handheld_lvgl_fail(void) {
    Serial.println("[LVGL] assertion / fail (standalone bring-up)");
    // Do not hard-hang forever during bring-up; soft loop so serial stays alive
    for (;;) {
        delay(2000);
        Serial.println("[LVGL] still stopped after fail");
    }
}

} // extern "C"

// lv_conf.h sets LV_FONT_DEFAULT to &lv_font_rsdeck_14 and disables MONTSERRAT_14.
// Montserrat 16 is enabled — use it as the stand-in until real rsDeck fonts are linked.
extern const lv_font_t lv_font_montserrat_16;
const lv_font_t lv_font_rsdeck_14 = lv_font_montserrat_16;