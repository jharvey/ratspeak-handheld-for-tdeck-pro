#pragma once

#include <Arduino.h>

class AudioNotify {
public:
    void begin();
    void end();

    // Notification sounds
    void playMessage();
    void playAnnounce();
    void playError();
    void playBoot();        // Sci-fi boot sequence
    void requestMessage();  // Defer short alert out of packet callbacks
    void loop();

    // Settings
    void setEnabled(bool enabled) { _enabled = enabled; }
    bool isEnabled() const { return _enabled; }
    void setVolume(uint8_t vol) { _volume = vol; }
    uint8_t volume() const { return _volume; }

    // Direct tone helpers (used by serial tests)
    void writeTone(uint16_t freq, uint16_t durationMs);
    void writeSilence(uint16_t durationMs);

    bool isReady() const { return _i2sReady; }

    // Audio power gating. The DAC/amplifier rail (BOARD_6609_EN) and the I2S clocks + DMA
    // (which run continuously with tx_desc_auto_clear, even when silent) cost ~15-20 mA,
    // so the path is switched on only to play a sound and off again after 3 s of silence.
    void pause();      // path off now
    void resume();     // path on now (also done automatically before any sound)
    bool isPowered() const { return _i2sReady && _powered; }

private:
    bool _enabled = true;
    bool _i2sReady = false;
    volatile bool _messagePending = false;
    bool _powered = true;
    uint32_t _lastUseMs = 0;
    void wake();
    uint8_t _volume = 80;  // 0-100
};