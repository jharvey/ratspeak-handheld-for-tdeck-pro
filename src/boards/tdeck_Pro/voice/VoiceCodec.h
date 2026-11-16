#pragma once
// Codec2 speech codec for voice messages and the walkie-talkie (lib/esp32_codec2, LGPL-2.1
// Codec2 packaged for ESP32). Codec2 runs at 8 kHz; the mic and speaker run at 16 kHz, so
// this halves the rate before encoding and doubles it after decoding.
//
// Modes (bits per second -> bytes per frame, frame = 20 or 40 ms):
//   3200 -> 8 B / 20 ms   (best; WiFi walkie-talkie)
//   1600 -> 8 B / 40 ms
//    700 -> 4 B / 40 ms   (700C: LoRa voice messages)
#include <stddef.h>
#include <stdint.h>

namespace voicecodec {

#if !defined(SIMULATOR) || defined(SIM_HAS_CODEC2)   // the simulator builds Codec2 too
bool begin(int bps);                 // 3200, 1600 or 700; false: unknown mode / no memory
void end();
int bps();
int frameMs();                       // 20 or 40
int frameBytes();                    // encoded bytes per frame
int frameSamples16k();               // 16 kHz samples per frame (320 or 640)
// One frame: frameSamples16k() samples in, frameBytes() bytes out.
void encode(const int16_t* pcm16k, uint8_t* bits);
// One frame: frameBytes() bytes in, frameSamples16k() samples out.
void decode(const uint8_t* bits, int16_t* pcm16k);
#else
inline bool begin(int) { return false; }
inline void end() {}
inline int bps() { return 0; }
inline int frameMs() { return 20; }
inline int frameBytes() { return 8; }
inline int frameSamples16k() { return 320; }
inline void encode(const int16_t*, uint8_t*) {}
inline void decode(const uint8_t*, int16_t*) {}
#endif

}  // namespace voicecodec
