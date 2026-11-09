// Simulated speaker: alerts are counted and logged (the window flashes a speaker icon).
#include "audio/AudioNotify.h"
#include "hw/SimHw.h"

static void beep(const char* what, bool enabled) {
    if (!enabled) return;
    { std::lock_guard<std::mutex> l(sim::hw().mu); sim::hw().beepCount++; }
    Serial.printf("[SIM-AUDIO] %s\n", what);
}

void AudioNotify::begin() { _i2sReady = true; }
void AudioNotify::end() { _i2sReady = false; }
void AudioNotify::playMessage() { beep("message tone", _enabled); }
void AudioNotify::playAnnounce() { beep("announce tone", _enabled); }
void AudioNotify::playError() { beep("error tone", _enabled); }
void AudioNotify::playBoot() { beep("boot tone", _enabled); }
void AudioNotify::requestMessage() { _messagePending = true; }
void AudioNotify::loop() { if (_messagePending) { _messagePending = false; playMessage(); } }
void AudioNotify::writeTone(uint16_t, uint16_t) {}
void AudioNotify::writeSilence(uint16_t) {}
