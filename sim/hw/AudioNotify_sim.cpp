// Simulated speaker: alerts are counted and logged (the window flashes a speaker icon).
#include "audio/AudioNotify.h"
#include "hw/SimHw.h"

#include <chrono>
#include <cstdio>
#include <thread>

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
void AudioNotify::pause() {}
void AudioNotify::resume() {}
// ---- speaker and microphone (voice messages, walkie-talkie) ---------------------------
// The speaker takes as long as the sound lasts (like the real I2S write) and, while the
// window records, logs each chunk at the wall-clock time it plays. The microphone hears the
// clip loaded with "mic <file.wav>", in real time, then silence; what it hears is logged too
// (the person speaking, for the video).
static int64_t wallMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// mic = true: the microphone's file (mic.pcm beside audio.pcm), so a video can leave the
// clean voice out (the walkie-talkie: only what the other radio's speaker plays, no echo).
static void logAudio(int64_t atWallMs, const int16_t* pcm, size_t n, bool mic = false) {
    std::string path;
    { std::lock_guard<std::mutex> l(sim::hw().mu); path = sim::hw().audioLog; }
    if (path.empty() || !n) return;
    if (mic) {
        const size_t slash = path.find_last_of("/\\");
        path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "mic.pcm";
    }
    if (FILE* f = fopen(path.c_str(), "ab")) {
        const uint32_t cnt = (uint32_t)n;
        fwrite(&atWallMs, sizeof(atWallMs), 1, f);
        fwrite(&cnt, sizeof(cnt), 1, f);
        fwrite(pcm, sizeof(int16_t), n, f);
        fclose(f);
    }
}

void AudioNotify::writePcm(const int16_t* pcm, size_t n) {
    if (!n) return;
    const int64_t now = wallMs();
    int64_t at;
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        auto& h = sim::hw();
        at = h.speakerEndWallMs > now ? h.speakerEndWallMs : now;
        h.speakerEndWallMs = at + (int64_t)(n / 16);
    }
    std::vector<int16_t> v(pcm, pcm + n);
    const int vol = _volume;
    for (auto& x : v) x = (int16_t)((int32_t)x * vol / 100);
    logAudio(at, v.data(), n);
    const int64_t wait = at + (int64_t)(n / 16) - wallMs() - 20;   // keep ~20 ms queued, as the DMA does
    if (wait > 0) std::this_thread::sleep_for(std::chrono::milliseconds(wait));
}

static int64_t g_micStartWall = 0;
static size_t g_micFed = 0;

bool AudioNotify::micStart() {
    g_micStartWall = wallMs();
    g_micFed = 0;
    _micOn = true;
    return true;
}

void AudioNotify::micStop() { _micOn = false; }

size_t AudioNotify::micRead(int16_t* out, size_t maxSamples, uint8_t) {
    if (!_micOn) return 0;
    const int64_t due = (wallMs() - g_micStartWall) * 16 - (int64_t)g_micFed;
    if (due <= 0) return 0;
    const size_t n = (size_t)due < maxSamples ? (size_t)due : maxSamples;
    {
        std::lock_guard<std::mutex> l(sim::hw().mu);
        const auto& clip = sim::hw().micClip;
        for (size_t i = 0; i < n; i++) out[i] = g_micFed + i < clip.size() ? clip[g_micFed + i] : 0;
    }
    logAudio(g_micStartWall + (int64_t)(g_micFed / 16), out, n, true);
    g_micFed += n;
    return n;
}
