// Audio output for T-Deck Pro via I2S → PCM5102A DAC → speaker / jack
#include "AudioNotify.h"
#include "config/BoardConfig.h"
#include <driver/i2s.h>
#include <math.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE  16000
#define I2S_PORT           I2S_NUM_0

void AudioNotify::begin() {
    if (I2S_BCK < 0 || I2S_DOUT < 0 || I2S_WS < 0) {
        Serial.println("[AUDIO] I2S pins not configured (check BoardConfig.h)");
        return;
    }

    // Ensure audio-path power is present (LilyGO drives BOARD_6609_EN for audio)
    if (BOARD_6609_EN >= 0) {
        pinMode(BOARD_6609_EN, OUTPUT);
        digitalWrite(BOARD_6609_EN, HIGH);
        delay(20);
    }

    i2s_config_t i2s_config = {};
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_config.sample_rate = AUDIO_SAMPLE_RATE;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = 4;
    i2s_config.dma_buf_len = 256;
    i2s_config.use_apll = false;
    i2s_config.tx_desc_auto_clear = true;

    i2s_pin_config_t pin_config = {};
    pin_config.mck_io_num = (I2S_MCLK >= 0) ? I2S_MCLK : I2S_PIN_NO_CHANGE;
    pin_config.bck_io_num = I2S_BCK;
    pin_config.ws_io_num = I2S_WS;
    pin_config.data_out_num = I2S_DOUT;
    pin_config.data_in_num = I2S_PIN_NO_CHANGE;

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] I2S install failed: %d\r\n", (int)err);
        return;
    }

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] I2S pin config failed: %d\r\n", (int)err);
        i2s_driver_uninstall(I2S_PORT);
        return;
    }

    i2s_zero_dma_buffer(I2S_PORT);
    _i2sReady = true;
    Serial.printf(
        "[AUDIO] I2S TX ready  BCK=%d DOUT=%d WS=%d  rate=%d\r\n",
        I2S_BCK, I2S_DOUT, I2S_WS, AUDIO_SAMPLE_RATE);
}

void AudioNotify::end() {
    if (_i2sReady) {
        i2s_driver_uninstall(I2S_PORT);
        _i2sReady = false;
        Serial.println("[AUDIO] I2S TX stopped");
    }
}

void AudioNotify::writeTone(uint16_t freq, uint16_t durationMs) {
    if (!_enabled || !_i2sReady) return;

    int numSamples = (AUDIO_SAMPLE_RATE * durationMs) / 1000;
    if (numSamples <= 0) return;

    int16_t* buf = (int16_t*)ps_malloc(numSamples * sizeof(int16_t));
    if (!buf) buf = (int16_t*)malloc(numSamples * sizeof(int16_t));
    if (!buf) return;

    float vol = (_volume / 100.0f) * 16000.0f;
    int fadeN = AUDIO_SAMPLE_RATE / 100; // 10 ms fade
    if (fadeN < 1) fadeN = 1;

    for (int i = 0; i < numSamples; i++) {
        float t = (float)i / AUDIO_SAMPLE_RATE;
        float s = sinf(2.0f * M_PI * freq * t) * 0.70f
                + sinf(2.0f * M_PI * freq * 2.0f * t) * 0.20f
                + sinf(2.0f * M_PI * freq * 3.0f * t) * 0.10f;
        float env = 1.0f;
        if (i < fadeN) env = (float)i / fadeN;
        if (i > numSamples - fadeN) env = (float)(numSamples - i) / fadeN;
        buf[i] = (int16_t)(s * env * vol);
    }

    size_t written = 0;
    i2s_write(I2S_PORT, buf, numSamples * sizeof(int16_t), &written, pdMS_TO_TICKS(200));
    free(buf);
}

void AudioNotify::writeSilence(uint16_t durationMs) {
    if (!_i2sReady) return;
    int numSamples = (AUDIO_SAMPLE_RATE * durationMs) / 1000;
    if (numSamples <= 0) return;

    size_t bufSize = numSamples * sizeof(int16_t);
    int16_t* buf = (int16_t*)ps_malloc(bufSize);
    if (!buf) buf = (int16_t*)malloc(bufSize);
    if (!buf) return;
    memset(buf, 0, bufSize);
    size_t written = 0;
    i2s_write(I2S_PORT, buf, bufSize, &written, pdMS_TO_TICKS(200));
    free(buf);
}

void AudioNotify::playMessage() {
    if (!_enabled || !_i2sReady) return;

    const int sr = AUDIO_SAMPLE_RATE;
    const int toneMs = 34;
    const int gapMs = 28;
    const int tailMs = 16;
    const int totalMs = toneMs + gapMs + toneMs + tailMs;
    const int totalSamples = sr * totalMs / 1000;

    int16_t* buf = (int16_t*)ps_malloc(totalSamples * sizeof(int16_t));
    if (!buf) buf = (int16_t*)malloc(totalSamples * sizeof(int16_t));
    if (!buf) return;
    memset(buf, 0, totalSamples * sizeof(int16_t));

    float vol = (_volume / 100.0f) * 12000.0f;
    int pos = 0;

    auto addTone = [&](float freq, int ms) {
        int n = sr * ms / 1000;
        int fadeN = sr * 5 / 1000;
        if (fadeN < 1) fadeN = 1;
        for (int i = 0; i < n && (pos + i) < totalSamples; i++) {
            float t = (float)i / sr;
            float s = sinf(2.0f * M_PI * freq * t) * 0.80f
                    + sinf(2.0f * M_PI * freq * 2.0f * t) * 0.15f
                    + sinf(2.0f * M_PI * freq * 3.0f * t) * 0.05f;
            float env = 1.0f;
            if (i < fadeN) env = (float)i / fadeN;
            if (i > n - fadeN) env = (float)(n - i) / fadeN;
            buf[pos + i] = (int16_t)(s * env * vol);
        }
        pos += n;
    };

    addTone(1000.0f, toneMs);
    pos += sr * gapMs / 1000;
    addTone(1000.0f, toneMs);
    pos += sr * tailMs / 1000;

    size_t written = 0;
    i2s_write(I2S_PORT, buf, totalSamples * sizeof(int16_t), &written, pdMS_TO_TICKS(150));
    free(buf);
}

void AudioNotify::requestMessage() {
    if (!_enabled) return;
    _messagePending = true;
}

void AudioNotify::loop() {
    if (!_messagePending) return;
    _messagePending = false;
    playMessage();
}

void AudioNotify::playAnnounce() {
    if (!_enabled) return;
    writeTone(800, 30);
    writeSilence(20);
}

void AudioNotify::playError() {
    if (!_enabled) return;
    for (int i = 0; i < 3; i++) {
        writeTone(400, 100);
        if (i < 2) writeSilence(50);
    }
    writeSilence(30);
}

void AudioNotify::playBoot() {
    if (!_enabled || !_i2sReady) return;

    // === RSDECK BOOT SEQUENCE ===
    // Sci-fi computer startup: sweep -> digital arpeggio -> confirmation
    // Total ~550 ms

    const int sr = AUDIO_SAMPLE_RATE;
    const int totalMs = 560;
    const int totalSamples = sr * totalMs / 1000;

    int16_t* buf = (int16_t*)ps_malloc(totalSamples * sizeof(int16_t));
    if (!buf) {
        buf = (int16_t*)malloc(totalSamples * sizeof(int16_t));
        if (!buf) return;
    }
    memset(buf, 0, totalSamples * sizeof(int16_t));

    float vol = (_volume / 100.0f) * 16000.0f;
    int pos = 0;

    auto addTone = [&](float freq, int ms) {
        int n = sr * ms / 1000;
        int fadeN = sr * 8 / 1000;
        if (fadeN < 1) fadeN = 1;
        for (int i = 0; i < n && (pos + i) < totalSamples; i++) {
            float t = (float)i / sr;
            float s = sinf(2.0f * M_PI * freq * t) * 0.65f
                    + sinf(2.0f * M_PI * freq * 2.0f * t) * 0.22f
                    + sinf(2.0f * M_PI * freq * 3.0f * t) * 0.13f;
            float env = 1.0f;
            if (i < fadeN) env = (float)i / fadeN;
            if (i > n - fadeN) env = (float)(n - i) / fadeN;
            buf[pos + i] = (int16_t)(s * env * vol);
        }
        pos += n;
    };

    auto addSweep = [&](float startF, float endF, int ms) {
        int n = sr * ms / 1000;
        int fadeN = sr * 8 / 1000;
        if (fadeN < 1) fadeN = 1;
        float phase = 0;
        for (int i = 0; i < n && (pos + i) < totalSamples; i++) {
            float t = (float)i / n;
            float freq = startF + (endF - startF) * t * t;
            phase += 2.0f * M_PI * freq / sr;
            float s = sinf(phase) * 0.65f
                    + sinf(phase * 2.0f) * 0.22f
                    + sinf(phase * 3.0f) * 0.08f;
            float env = 1.0f;
            if (i < fadeN) env = (float)i / fadeN;
            if (i > n - fadeN) env = (float)(n - i) / fadeN;
            buf[pos + i] = (int16_t)(s * env * vol);
        }
        pos += n;
    };

    auto addSilence = [&](int ms) {
        pos += sr * ms / 1000;
    };

    // Phase 1: Rising power sweep 300→1200 Hz
    addSweep(300, 1200, 160);
    addSilence(25);

    // Phase 2: Three quick ascending notes — E5, G#5, B5
    addTone(659,  45);
    addSilence(12);
    addTone(831,  45);
    addSilence(12);
    addTone(988,  45);
    addSilence(25);

    // Phase 3: Descending glitch sweep 2400→1600 Hz
    addSweep(2400, 1600, 60);
    addSilence(20);

    // Phase 4: Final confirmation — E6
    addTone(1319, 100);

    size_t written = 0;
    i2s_write(I2S_PORT, buf, pos * sizeof(int16_t), &written, pdMS_TO_TICKS(200));

    memset(buf, 0, 512 * sizeof(int16_t));
    i2s_write(I2S_PORT, buf, 512 * sizeof(int16_t), &written, pdMS_TO_TICKS(200));

    free(buf);
}