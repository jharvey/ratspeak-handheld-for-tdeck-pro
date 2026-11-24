#pragma once
// Big scratch buffers in PSRAM instead of internal RAM (internal RAM is what WiFi, Bluetooth
// and the crypto need; the firmware's static data alone took 234 KB of it). A function-local
//     static uint8_t (&buf)[4096] = psram<uint8_t, 4096>();
// keeps its type, so sizeof(buf) is still 4096. Zeroed; falls back to internal RAM when there
// is no PSRAM. PSRAM is up only from setup() on (CONFIG_SPIRAM_BOOT_INIT is off), so use it for
// function-local statics (made on first call) or allocate globals in setup, never at static init.
#include <stddef.h>
#include <stdlib.h>
#include <esp_heap_caps.h>

template <typename T, size_t N>
T (&psram())[N] {
    void* p = heap_caps_calloc(1, sizeof(T) * N, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = calloc(1, sizeof(T) * N);
    return *static_cast<T(*)[N]>(p);
}
