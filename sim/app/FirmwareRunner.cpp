#include "app/FirmwareRunner.h"

// The firmware's own entry points (src/boards/tdeck_Pro/main.cpp).
void setup();
void loop();
#include "hw/SimHw.h"

void FirmwareRunner::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] {
        setup();
        for (;;) {
            { std::lock_guard<std::mutex> l(sim::hw().mu); if (sim::hw().quit) break; }
            loop();
        }
        running_ = false;
    });
    thread_.detach();
}
