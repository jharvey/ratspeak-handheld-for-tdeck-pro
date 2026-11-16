// Codec2 wrapper. See VoiceCodec.h.
#if !defined(SIMULATOR) || defined(SIM_HAS_CODEC2)

#include "VoiceCodec.h"

#include <Arduino.h>
#include <codec2.h>
#include <string.h>

namespace voicecodec {
namespace {
CODEC2* g_c2 = nullptr;
int g_bps = 0;
int g_samples8k = 160;   // codec2_samples_per_frame
int g_bytes = 8;         // codec2_bytes_per_frame
int16_t* g_buf8k = nullptr;
int16_t g_prevIn = 0;    // low-pass state across frames
int16_t g_prevOut = 0;   // interpolation state across frames
}  // namespace

bool begin(int b) {
    int mode;
    switch (b) {
        case 3200: mode = CODEC2_MODE_3200; break;
        case 1600: mode = CODEC2_MODE_1600; break;
        case 700:  mode = CODEC2_MODE_700C; break;
        default: return false;
    }
    if (g_c2 && g_bps == b) return true;
    end();
    g_c2 = codec2_create(mode);
    if (!g_c2) { Serial.printf("[C2] create %d failed\n", b); return false; }
    g_bps = b;
    g_samples8k = codec2_samples_per_frame(g_c2);
    g_bytes = codec2_bytes_per_frame(g_c2);
    g_buf8k = (int16_t*)malloc(sizeof(int16_t) * g_samples8k);
    g_prevIn = g_prevOut = 0;
    if (!g_buf8k) { end(); return false; }
    Serial.printf("[C2] %d bps: %d samples (8 kHz) -> %d bytes per %d ms\n", b, g_samples8k, g_bytes,
                  g_samples8k / 8);
    return true;
}

void end() {
    if (g_c2) codec2_destroy(g_c2);
    g_c2 = nullptr;
    free(g_buf8k);
    g_buf8k = nullptr;
    g_bps = 0;
}

int bps() { return g_bps; }
int frameMs() { return g_samples8k / 8; }
int frameBytes() { return g_bytes; }
int frameSamples16k() { return g_samples8k * 2; }

void encode(const int16_t* in, uint8_t* bits) {
    if (!g_c2) return;
    // 16 -> 8 kHz: [1 2 1]/4 low-pass, keep every second sample.
    for (int i = 0; i < g_samples8k; i++) {
        const int32_t a = i ? in[2 * i - 1] : g_prevIn, b = in[2 * i], c = in[2 * i + 1];
        g_buf8k[i] = (int16_t)((a + 2 * b + c) / 4);
    }
    g_prevIn = in[2 * g_samples8k - 1];
    codec2_encode(g_c2, bits, g_buf8k);
}

void decode(const uint8_t* bits, int16_t* out) {
    if (!g_c2) return;
    codec2_decode(g_c2, g_buf8k, bits);
    // 8 -> 16 kHz: each sample, then the midpoint to the next.
    for (int i = 0; i < g_samples8k; i++) {
        const int16_t prev = i ? g_buf8k[i - 1] : g_prevOut;
        out[2 * i] = (int16_t)((prev + g_buf8k[i]) / 2);
        out[2 * i + 1] = g_buf8k[i];
    }
    g_prevOut = g_buf8k[g_samples8k - 1];
}

}  // namespace voicecodec

#endif  // !SIMULATOR || SIM_HAS_CODEC2
