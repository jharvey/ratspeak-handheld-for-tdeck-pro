// Home Assistant voice satellite (Wyoming protocol). See HaVoice.h.
#if !defined(SIMULATOR)

#include "HaVoice.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "audio/AudioNotify.h"

namespace havoice {
namespace {

constexpr uint32_t kRate = 16000;
constexpr uint32_t kMaxTalkMs = 15000;      // push-to-talk safety stop
constexpr uint32_t kRunTimeoutMs = 30000;   // HA never answered: give up on the run
constexpr size_t kHeaderMax = 1024, kDataMax = 4096;

AudioNotify* g_audio = nullptr;
WiFiServer* g_server = nullptr;
WiFiClient g_client;
bool g_mdns = false;
char g_name[32] = "";

bool g_satRunning = false;      // HA sent run-satellite (and not pause-satellite)
bool g_talking = false;
uint32_t g_talkStart = 0;
bool g_runActive = false;       // an Assist run is in progress (until its audio ends / error)
uint32_t g_runAt = 0;
bool g_playing = false;
uint32_t g_playRate = kRate;
uint8_t g_playChannels = 1;
uint8_t g_micGain = 8;
uint32_t g_micSamples = 0;
int32_t g_micPeak = 0;

char g_status[48] = "off";
char g_heard[96] = "";
char g_answer[96] = "";
uint32_t g_changed = 0;

// Receive state machine.
enum class Rx { Header, Data, Payload };
Rx g_rx = Rx::Header;
char g_hdr[kHeaderMax + 1];
size_t g_hdrLen = 0;
char g_type[32] = "";
char g_data[kDataMax + 1];
size_t g_dataLen = 0, g_dataNeed = 0;
size_t g_payNeed = 0;
uint8_t g_carry[4];
size_t g_carryLen = 0;
bool g_payAudio = false;

void setStatus(const char* s) {
    if (strcmp(g_status, s) == 0) return;
    strlcpy(g_status, s, sizeof(g_status));
    g_changed = millis() | 1;
    Serial.printf("[HAV] %s\n", s);
}

// ---- send ------------------------------------------------------------------------------
bool sendEvent(const char* type, const JsonDocument* data, const uint8_t* payload = nullptr, size_t payLen = 0) {
    if (!g_client.connected()) return false;
    char dataBuf[768];
    size_t dataLen = 0;
    if (data) dataLen = serializeJson(*data, dataBuf, sizeof(dataBuf));
    char hdr[160];
    int n = snprintf(hdr, sizeof(hdr), "{\"type\":\"%s\",\"version\":\"1.5.4\"", type);
    if (dataLen) n += snprintf(hdr + n, sizeof(hdr) - n, ",\"data_length\":%u", (unsigned)dataLen);
    if (payLen) n += snprintf(hdr + n, sizeof(hdr) - n, ",\"payload_length\":%u", (unsigned)payLen);
    n += snprintf(hdr + n, sizeof(hdr) - n, "}\n");
    if (g_client.write((const uint8_t*)hdr, n) != (size_t)n) return false;
    if (dataLen && g_client.write((const uint8_t*)dataBuf, dataLen) != dataLen) return false;
    if (payLen && g_client.write(payload, payLen) != payLen) return false;
    return true;
}

void sendInfo() {
    JsonDocument d;
    JsonObject sat = d["satellite"].to<JsonObject>();
    sat["name"] = g_name;
    JsonObject att = sat["attribution"].to<JsonObject>();
    att["name"] = "RatSpeak";
    att["url"] = "https://github.com/";
    sat["installed"] = true;
    sat["description"] = "RatSpeak T-Deck Pro (push-to-talk)";
    sat["version"] = "1.0";
    sat["area"] = nullptr;
    sat["has_vad"] = false;
    sat["active_wake_words"].to<JsonArray>();
    sat["max_active_wake_words"] = 0;
    sat["supports_trigger"] = false;
    for (const char* k : {"asr", "tts", "handle", "intent", "wake", "mic", "snd"}) d[k].to<JsonArray>();
    sendEvent("info", &d);
}

void audioFormat(JsonDocument& d) {
    d["rate"] = kRate;
    d["width"] = 2;
    d["channels"] = 1;
}

// ---- receive -----------------------------------------------------------------------------
void playChunk(const uint8_t* p, size_t n) {
    // 16-bit samples; channels > 1: the first; rates other than 16 kHz: nearest sample.
    const size_t frame = 2 * (g_playChannels ? g_playChannels : 1);
    const size_t frames = n / frame;
    static int16_t out[1024];
    size_t o = 0;
    if (g_playRate == kRate) {
        for (size_t i = 0; i < frames; i++) {
            out[o++] = (int16_t)(p[i * frame] | (p[i * frame + 1] << 8));
            if (o == 1024) { g_audio->writePcm(out, o); o = 0; }
        }
    } else {
        const size_t outFrames = (size_t)((uint64_t)frames * kRate / g_playRate);
        for (size_t j = 0; j < outFrames; j++) {
            const size_t i = (size_t)((uint64_t)j * g_playRate / kRate);
            out[o++] = (int16_t)(p[i * frame] | (p[i * frame + 1] << 8));
            if (o == 1024) { g_audio->writePcm(out, o); o = 0; }
        }
    }
    if (o) g_audio->writePcm(out, o);
}

void endRun(const char* why) {
    if (g_talking) {
        g_talking = false;
        g_audio->micStop();
    }
    g_runActive = false;
    g_playing = false;
    setStatus(why);
}

void handleEvent() {
    JsonDocument d;
    if (g_dataLen) deserializeJson(d, g_data, g_dataLen);
    const char* t = g_type;
    if (!strcmp(t, "describe")) {
        sendInfo();
    } else if (!strcmp(t, "ping")) {
        JsonDocument r;
        if (d["text"].is<const char*>()) r["text"] = d["text"];
        sendEvent("pong", &r);
    } else if (!strcmp(t, "run-satellite")) {
        g_satRunning = true;
        setStatus("ready: hold MIC to talk");
    } else if (!strcmp(t, "pause-satellite")) {
        g_satRunning = false;
        if (g_talking) stopTalk("paused by HA");
        setStatus("paused by Home Assistant");
    } else if (!strcmp(t, "voice-started")) {
        setStatus("listening (speech heard)");
    } else if (!strcmp(t, "voice-stopped")) {
        if (g_talking) stopTalk("end of speech");
    } else if (!strcmp(t, "transcript")) {
        strlcpy(g_heard, d["text"] | "", sizeof(g_heard));
        g_changed = millis() | 1;
        Serial.printf("[HAV] heard: %s\n", g_heard);
        setStatus("thinking...");
    } else if (!strcmp(t, "synthesize")) {
        strlcpy(g_answer, d["text"] | "", sizeof(g_answer));
        g_changed = millis() | 1;
        Serial.printf("[HAV] answer: %s\n", g_answer);
    } else if (!strcmp(t, "audio-start")) {
        g_playRate = d["rate"] | kRate;
        g_playChannels = d["channels"] | 1;
        if ((d["width"] | 2) != 2) Serial.println("[HAV] audio width not 16-bit: not played");
        g_playing = (d["width"] | 2) == 2;
        g_runAt = millis();
        setStatus("speaking");
    } else if (!strcmp(t, "audio-stop")) {
        g_playing = false;
        sendEvent("played", nullptr);
        endRun("ready: hold MIC to talk");
    } else if (!strcmp(t, "error")) {
        char e[48];
        snprintf(e, sizeof(e), "HA: %.40s", d["text"] | "error");
        endRun(e);
    } else if (!strcmp(t, "run-end") || !strcmp(t, "intent")) {
        // nothing: the reply audio (or its absence) ends the run
    }
}

// Payload bytes: audio-chunk payloads go to the speaker as they arrive; others are skipped.
void handlePayload(const uint8_t* p, size_t n) {
    if (!g_payAudio || !g_playing) return;
    uint8_t tmp[2048 + 4];
    size_t k = 0;
    if (g_carryLen) { memcpy(tmp, g_carry, g_carryLen); k = g_carryLen; g_carryLen = 0; }
    memcpy(tmp + k, p, n);
    k += n;
    const size_t frame = 2 * (g_playChannels ? g_playChannels : 1);
    const size_t whole = k - k % frame;
    if (whole) playChunk(tmp, whole);
    g_carryLen = k - whole;
    if (g_carryLen) memcpy(g_carry, tmp + whole, g_carryLen);
}

void beginEvent() {
    JsonDocument h;
    if (deserializeJson(h, g_hdr, g_hdrLen)) { Serial.println("[HAV] bad header"); g_rx = Rx::Header; return; }
    strlcpy(g_type, h["type"] | "", sizeof(g_type));
    g_dataNeed = h["data_length"] | 0;
    g_payNeed = h["payload_length"] | 0;
    g_dataLen = 0;
    if (h["data"].is<JsonObject>()) g_dataLen = serializeJson(h["data"], g_data, kDataMax);   // inline data
    g_payAudio = !strcmp(g_type, "audio-chunk");
    g_carryLen = 0;
    if (g_dataNeed > kDataMax) { Serial.printf("[HAV] data too big (%u)\n", (unsigned)g_dataNeed); }
    g_rx = g_dataNeed ? Rx::Data : (g_payNeed ? Rx::Payload : Rx::Header);
    if (g_rx == Rx::Header) handleEvent();
}

// Read what has arrived (at most ~8 KB per call so the loop keeps going).
void pump() {
    size_t budget = 8192;
    while (budget && g_client.connected() && g_client.available()) {
        if (g_rx == Rx::Header) {
            const int c = g_client.read();
            if (c < 0) break;
            budget--;
            if (c == '\n') {
                g_hdr[g_hdrLen] = 0;
                if (g_hdrLen) beginEvent();
                g_hdrLen = 0;
            } else if (g_hdrLen < kHeaderMax) {
                g_hdr[g_hdrLen++] = (char)c;
            }
        } else if (g_rx == Rx::Data) {
            uint8_t buf[512];
            const size_t want = std::min(g_dataNeed, sizeof(buf));
            const int got = g_client.read(buf, want);
            if (got <= 0) break;
            budget -= std::min(budget, (size_t)got);
            for (int i = 0; i < got; i++) if (g_dataLen < kDataMax) g_data[g_dataLen++] = (char)buf[i];
            g_dataNeed -= got;
            if (!g_dataNeed) {
                g_data[g_dataLen] = 0;
                if (g_payNeed) {
                    if (g_payAudio) {           // the chunk's format may differ from audio-start
                        JsonDocument d;
                        if (!deserializeJson(d, g_data, g_dataLen)) {
                            g_playRate = d["rate"] | g_playRate;
                            g_playChannels = d["channels"] | g_playChannels;
                        }
                    }
                    g_rx = Rx::Payload;
                } else {
                    g_rx = Rx::Header;
                    handleEvent();
                }
            }
        } else {
            uint8_t buf[2048];
            const size_t want = std::min(g_payNeed, sizeof(buf));
            const int got = g_client.read(buf, want);
            if (got <= 0) break;
            budget -= std::min(budget, (size_t)got);
            handlePayload(buf, got);
            g_payNeed -= got;
            if (!g_payNeed) { g_rx = Rx::Header; handleEvent(); }
        }
    }
}

void streamMic() {
    static int16_t buf[512];
    const size_t n = g_audio->micRead(buf, 512, g_micGain);
    if (!n) return;
    for (size_t i = 0; i < n; i++) {
        const int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > g_micPeak) g_micPeak = a;
    }
    JsonDocument d;
    audioFormat(d);
    d["timestamp"] = (uint32_t)(g_micSamples * 1000ULL / kRate);
    g_micSamples += n;
    if (!sendEvent("audio-chunk", &d, (const uint8_t*)buf, n * 2)) stopTalk("connection lost");
}

void closeClient(const char* why) {
    if (g_talking) { g_talking = false; g_audio->micStop(); }
    if (g_client) g_client.stop();
    g_satRunning = false;
    g_runActive = false;
    g_playing = false;
    g_rx = Rx::Header;
    g_hdrLen = 0;
    setStatus(why);
}

void serverStop() {
    closeClient("off");
    if (g_server) { g_server->end(); delete g_server; g_server = nullptr; }
    if (g_mdns) { MDNS.end(); g_mdns = false; }
    WiFi.setSleep(true);
}

}  // namespace

void begin(AudioNotify* audio) { g_audio = audio; }

void tick(bool wifiUp, bool enabled, const char* deviceName) {
    if (!g_audio) return;
    if (!enabled || !wifiUp) {
        if (g_server || g_mdns) serverStop();
        setStatus(enabled ? "waiting for WiFi" : "off");
        return;
    }
    if (!g_server) {
        strlcpy(g_name, deviceName && deviceName[0] ? deviceName : "RatSpeak", sizeof(g_name));
        g_server = new WiFiServer(kPort);
        g_server->begin();
        g_server->setNoDelay(true);
        char host[32];
        size_t j = 0;
        for (const char* p = g_name; *p && j < sizeof(host) - 1; p++)
            host[j++] = isalnum((unsigned char)*p) ? (char)tolower((unsigned char)*p) : '-';
        host[j] = 0;
        g_mdns = MDNS.begin(host);
        if (g_mdns) MDNS.addService("wyoming", "tcp", kPort);
        Serial.printf("[HAV] listening on %s:%u (mDNS %s.local %s)\n", WiFi.localIP().toString().c_str(),
                      (unsigned)kPort, host, g_mdns ? "ok" : "failed");
        setStatus("waiting for Home Assistant");
    }
    if (g_server->hasClient()) {
        WiFiClient c = g_server->available();
        if (g_client && g_client.connected()) closeClient("HA reconnected");
        g_client = c;
        g_client.setNoDelay(true);
        g_rx = Rx::Header;
        g_hdrLen = 0;
        Serial.printf("[HAV] Home Assistant connected from %s\n", g_client.remoteIP().toString().c_str());
        setStatus("HA connected");
    }
    if (g_client && !g_client.connected()) closeClient("waiting for Home Assistant");
    if (!g_client) return;

    pump();
    if (g_talking) {
        streamMic();
        if (millis() - g_talkStart > kMaxTalkMs) stopTalk("15 s limit");
    }
    if (g_runActive && !g_talking && !g_playing && millis() - g_runAt > kRunTimeoutMs)
        endRun("no answer from HA");
    WiFi.setSleep(!busy());           // modem sleep off while talking / answering (latency)
}

bool connected() { return g_client && g_client.connected() && g_satRunning; }
bool talking() { return g_talking; }
bool busy() { return g_talking || g_runActive || g_playing; }

bool startTalk() {
    if (!connected()) { setStatus(g_client ? "HA has not started the satellite" : "Home Assistant not connected"); return false; }
    if (g_talking) return true;
    if (!g_audio->micStart()) { setStatus("microphone failed"); return false; }
    JsonDocument r;
    r["start_stage"] = "asr";
    r["end_stage"] = "tts";
    r["restart_on_end"] = false;
    JsonObject snd = r["snd_format"].to<JsonObject>();
    snd["rate"] = kRate;
    snd["width"] = 2;
    snd["channels"] = 1;
    sendEvent("run-pipeline", &r);
    JsonDocument a;
    audioFormat(a);
    a["timestamp"] = 0;
    sendEvent("audio-start", &a);
    g_talking = true;
    g_talkStart = millis();
    g_runActive = true;
    g_runAt = millis();
    g_micSamples = 0;
    g_micPeak = 0;
    g_heard[0] = 0;
    g_answer[0] = 0;
    setStatus("listening...");
    return true;
}

void stopTalk(const char* why) {
    if (!g_talking) return;
    g_talking = false;
    g_audio->micStop();
    JsonDocument d;
    d["timestamp"] = (uint32_t)(g_micSamples * 1000ULL / kRate);
    sendEvent("audio-stop", &d);
    g_runAt = millis();
    Serial.printf("[HAV] mic stopped (%s): %lu ms, peak %ld\n", why,
                  (unsigned long)(g_micSamples * 1000ULL / kRate), (long)g_micPeak);
    setStatus("thinking...");
}

const char* status() { return g_status; }
const char* heard() { return g_heard; }
const char* answer() { return g_answer; }
uint32_t changedAt() { return g_changed; }
void setMicGain(uint8_t g) { g_micGain = g ? g : 1; }

}  // namespace havoice

#endif  // !SIMULATOR
