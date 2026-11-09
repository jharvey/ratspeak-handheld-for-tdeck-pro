#pragma once
// Runs the firmware's own setup()/loop() (src/boards/tdeck_Pro/main.cpp) on a
// dedicated thread, exactly like the ESP32 Arduino loop task.
#include <atomic>
#include <thread>

class FirmwareRunner {
public:
    void start();
    bool running() const { return running_; }
private:
    std::thread thread_;
    std::atomic<bool> running_{false};
};
