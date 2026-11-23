#include <Preferences.h>
#include <esp_heap_caps.h>
#include <Arduino.h>
#include "StandaloneApp.h"

void setup() {
#if !defined(SIMULATOR)
    // Allocations from 1024 bytes up go to PSRAM instead of the scarce internal RAM (framework
    // default 4096): on COM8 the lowest internal free went 5.9 KB -> 33.9 KB under the ATAK /
    // marker stress, same UI speed (Stress_test_plan.md, 2026-11-23). Console "psramth <bytes>"
    // overrides it (4096 = the framework default, 0 = back to 1024); read before anything
    // else allocates.
    {
        uint32_t th = 1024;
        Preferences p;
        if (p.begin("rs_mem", true)) {
            const uint32_t v = p.getUInt("extmem", 0);
            p.end();
            if (v) th = v;
        }
        if (th >= 256 && th < 4096) heap_caps_malloc_extmem_enable(th);
    }
#endif
#if !defined(SIMULATOR) && ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    // USB Serial/JTAG drops bytes once its RX queue (default 256) is full; the console's
    // `sdput` file upload sends up to 16 KB at once (tools/maptiles/push_tiles.py).
    Serial.setRxBufferSize(17 * 1024);
#endif
    Serial.begin(115200);
    // No wait for the PC here: after a reset the PC misses the first seconds of output
    // anyway while USB comes back, and the boot screen should show as soon as it can.
    // Boot phase times stay in RAM ("boottime").
    Serial.println();
    Serial.println("[MAIN] setup");
    Serial.flush();

    if (!standalone::begin()) {
        Serial.println("[MAIN] begin FAILED — looping without UI");
        Serial.flush();
    } else {
        Serial.println("[MAIN] begin OK");
        Serial.flush();
    }
}

void loop() {
    static uint32_t lastBeat = 0;
    if (millis() - lastBeat > 60000UL) {
        lastBeat = millis();
        Serial.printf("[MAIN] beat free_int=%u\n",
                      (unsigned)ESP.getFreeHeap());
        Serial.flush();
    }
    standalone::loop();
}