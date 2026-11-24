// Home Assistant voice satellite: ESPHome native API server (TCP 6053), Noise encrypted.
// See HaVoice.h. Protocol references: esphome/components/api/api.proto (message ids and
// fields) and aioesphomeapi/_frame_helper/noise.py (framing and handshake).
#if !defined(SIMULATOR)

#include "HaVoice.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_random.h>
#include <math.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>

#include "ChaChaPoly.h"
#include "../PsramBuf.h"
#include "audio/AudioNotify.h"

namespace havoice {
namespace {

constexpr uint32_t kRate = 16000;
constexpr uint32_t kMaxTalkMs = 15000;      // push-to-talk safety stop
constexpr uint32_t kRunTimeoutMs = 30000;   // HA never finished the run: give up
constexpr size_t kRxMax = 4096;             // biggest frame we accept from HA
constexpr size_t kTxMax = 2200;

// Message ids (api.proto).
enum : uint16_t {
    HELLO_REQ = 1, HELLO_RESP = 2, AUTH_REQ = 3, AUTH_RESP = 4, DISCONNECT_REQ = 5, DISCONNECT_RESP = 6,
    PING_REQ = 7, PING_RESP = 8, DEVICE_INFO_REQ = 9, DEVICE_INFO_RESP = 10, LIST_ENTITIES_REQ = 11,
    LIST_ENTITIES_DONE = 19, SUBSCRIBE_STATES = 20, SUBSCRIBE_LOGS = 28, SUBSCRIBE_HA_SERVICES = 34,
    GET_TIME_REQ = 36, GET_TIME_RESP = 37, SUBSCRIBE_HA_STATES = 38, VA_SUBSCRIBE = 89, VA_REQUEST = 90,
    VA_RESPONSE = 91, VA_EVENT = 92, VA_AUDIO = 106, VA_ANNOUNCE_REQ = 119, VA_ANNOUNCE_DONE = 120,
    VA_CONFIG_REQ = 121, VA_CONFIG_RESP = 122,
    LIST_ENTITIES_SENSOR = 16, SENSOR_STATE = 25,
};
// Sensor entities offered to HA (opt-in): keys are fixed per entity.
constexpr uint32_t kKeyBattery = 0x52535001, kKeyLat = 0x52535002, kKeyLon = 0x52535003;
// VoiceAssistantFeature (aioesphomeapi): VOICE_ASSISTANT | SPEAKER | API_AUDIO | ANNOUNCE.
// Announcements: the unit shows the text (it cannot play HA's media URL: no MP3 decoder).
constexpr uint32_t kVaFeatures = 1 | 2 | 4 | 16;
enum VaEvent : uint32_t {
    EV_ERROR = 0, EV_RUN_START = 1, EV_RUN_END = 2, EV_STT_START = 3, EV_STT_END = 4, EV_INTENT_START = 5,
    EV_INTENT_END = 6, EV_TTS_START = 7, EV_TTS_END = 8, EV_WAKE_WORD_START = 9, EV_WAKE_WORD_END = 10,
    EV_STT_VAD_START = 11, EV_STT_VAD_END = 12,
    EV_TTS_STREAM_START = 98, EV_TTS_STREAM_END = 99,
};
// VoiceAssistantRequest.flags (aioesphomeapi VoiceAssistantCommandFlag).
constexpr uint32_t kFlagUseWakeWord = 2;

AudioNotify* g_audio = nullptr;
WiFiServer* g_server = nullptr;
WiFiClient g_client;
bool g_mdns = false;
char g_node[32] = "ratspeak";        // [a-z0-9-], the ESPHome node name
char g_friendly[32] = "RatSpeak";
char g_keyB64[kKeyB64Len + 1] = "";
uint8_t g_psk[32];
bool g_encrypted = false;

// Connection state.
enum class Conn { None, NoiseHello, NoiseHandshake, Ready, Plain };
Conn g_conn = Conn::None;
// Big buffers live in PSRAM (allocated in begin()): internal RAM is needed for DMA (mic,
// speaker) and task stacks (the walkie-talkie's Codec2 needs 20 KB).
constexpr size_t kRxBuf = kRxMax + 8;
uint8_t* g_rx = nullptr;
uint8_t* g_txBuf = nullptr;    // sendMessage: the frame
uint8_t* g_txPlain = nullptr;  // sendMessage: before encryption
uint8_t* g_rxPlain = nullptr;  // onNoiseFrame: after decryption
size_t g_rxLen = 0;
bool g_vaSubscribed = false;

// Noise state (responder).
uint8_t g_h[32], g_ck[32], g_k[32];
uint64_t g_n = 0;
bool g_hasK = false;
uint8_t g_kSend[32], g_kRecv[32];
uint64_t g_nSend = 0, g_nRecv = 0;
uint8_t g_prologue[64];
size_t g_prologueLen = 0;

// Voice run state.
bool g_talking = false;
bool g_streaming = false;           // HA answered the start: mic audio goes out
uint32_t g_talkStart = 0;
bool g_runActive = false;
uint32_t g_runAt = 0;
bool g_playing = false;
// Sensors for HA (0 off, 1 battery, 2 battery + position) and their values from the app.
uint8_t g_sensors = 0;
bool g_statesSubscribed = false;
float g_batPct = -1;
bool g_havePos = false;
double g_lat = 0, g_lon = 0;
uint32_t g_statesAt = 0;
float g_sentBat = -2;
double g_sentLat = 999, g_sentLon = 999;
bool g_quiet = false;                // quiet hours: an announcement shows, no chime
uint32_t g_announcedAt = 0;
// Wake word (Home Assistant's wake word engine, e.g. openWakeWord): while wanted and HA is
// connected, a run that starts at the wake word stage stays open and the mic streams to it.
bool g_wakeWanted = false;
bool g_wakeListen = false;          // a wake word run is open (mic streaming, no wake word yet)
uint32_t g_wakeRetryAt = 0;         // millis(): not before this
uint32_t g_wakeBackoff = 0;         // after an error before the wake word: 5 s, doubling to 60 s
bool g_pendingTalk = false;         // MIC pressed while listening: the push-to-talk run starts
uint32_t g_pendingAt = 0;           // once HA has ended the wake word run (or after 1.5 s)
uint8_t g_micGain = 4;
uint32_t g_micSamples = 0;
int32_t g_micPeak = 0;

char g_status[48] = "off";
char g_heard[96] = "";
char g_answer[96] = "";
uint32_t g_changed = 0;

void setStatus(const char* s) {
    if (strcmp(g_status, s) == 0) return;
    strlcpy(g_status, s, sizeof(g_status));
    g_changed = millis() | 1;
    Serial.printf("[HAV] %s\n", s);
}

// ---- crypto helpers ----------------------------------------------------------------------
int rng(void*, unsigned char* buf, size_t len) { esp_fill_random(buf, len); return 0; }

void sha256(const uint8_t* a, size_t na, uint8_t out[32]) {
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), a, na, out);
}
void hmac(const uint8_t key[32], const uint8_t* m, size_t n, uint8_t out[32]) {
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, 32, m, n, out);
}
// Noise HKDF: up to three 32-byte outputs.
void hkdf(const uint8_t ck[32], const uint8_t* ikm, size_t n, uint8_t o1[32], uint8_t o2[32], uint8_t* o3) {
    uint8_t tk[32], buf[33];
    hmac(ck, ikm, n, tk);
    buf[0] = 1;
    hmac(tk, buf, 1, o1);
    memcpy(buf, o1, 32); buf[32] = 2;
    hmac(tk, buf, 33, o2);
    if (o3) { memcpy(buf, o2, 32); buf[32] = 3; hmac(tk, buf, 33, o3); }
}

// X25519 (RFC 7748) with mbedTLS: out = clamp(scalar) * u.
bool x25519(const uint8_t scalar[32], const uint8_t u[32], uint8_t out[32]) {
    uint8_t s[32], uu[32];
    memcpy(s, scalar, 32);
    s[0] &= 248; s[31] &= 127; s[31] |= 64;
    memcpy(uu, u, 32);
    uu[31] &= 127;
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point P, R;
    mbedtls_ecp_group_init(&grp); mbedtls_mpi_init(&d); mbedtls_ecp_point_init(&P); mbedtls_ecp_point_init(&R);
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
              mbedtls_mpi_read_binary_le(&d, s, 32) == 0 &&
              mbedtls_mpi_read_binary_le(&P.X, uu, 32) == 0 &&
              mbedtls_mpi_lset(&P.Z, 1) == 0 &&
              mbedtls_ecp_mul(&grp, &R, &d, &P, rng, nullptr) == 0 &&
              mbedtls_mpi_write_binary_le(&R.X, out, 32) == 0;
    mbedtls_ecp_point_free(&R); mbedtls_ecp_point_free(&P); mbedtls_mpi_free(&d); mbedtls_ecp_group_free(&grp);
    return ok;
}

void mixHash(const uint8_t* d, size_t n) {
    uint8_t buf[32 + 128];
    if (n > 128) {                       // (never: handshake data is small)
        uint8_t t[32]; sha256(d, n, t); d = t; n = 32;
    }
    memcpy(buf, g_h, 32);
    memcpy(buf + 32, d, n);
    sha256(buf, 32 + n, g_h);
}
void mixKey(const uint8_t* ikm, size_t n) {
    uint8_t ck[32], k[32];
    hkdf(g_ck, ikm, n, ck, k, nullptr);
    memcpy(g_ck, ck, 32); memcpy(g_k, k, 32);
    g_n = 0; g_hasK = true;
}
void mixKeyAndHash(const uint8_t* ikm, size_t n) {
    uint8_t ck[32], th[32], k[32];
    hkdf(g_ck, ikm, n, ck, th, k);
    memcpy(g_ck, ck, 32);
    mixHash(th, 32);
    memcpy(g_k, k, 32);
    g_n = 0; g_hasK = true;
}
// Handshake payloads here are always empty: only the 16-byte tag travels.
void encryptAndHashEmpty(uint8_t tag[16]) {
    uint8_t nonce[12];
    chachapoly::noiseNonce(g_n++, nonce);
    chachapoly::seal(g_k, nonce, g_h, 32, nullptr, 0, tag);
    mixHash(tag, 16);
}
bool decryptAndHash(const uint8_t* ct, size_t n) {
    if (n < 16) return false;
    uint8_t nonce[12], plain[64];
    chachapoly::noiseNonce(g_n++, nonce);
    if (n - 16 > sizeof(plain) || !chachapoly::open(g_k, nonce, g_h, 32, ct, n - 16, plain)) return false;
    mixHash(ct, n);
    return true;
}

// ---- framing ----------------------------------------------------------------------------
// Bench tunnel (console "apitunnel"): the API byte stream over USB serial as hex lines, so
// the handshake and messages can be tested without WiFi (tools/test/esphome_api_test.py).
bool g_tunnel = false;

bool writeRaw(const uint8_t* d, size_t n) {
    if (g_tunnel) {                      // one write per line: other logs cannot cut into it
        static char (&line)[5 + 2 * (kTxMax + 40) + 2] = psram<char, 5 + 2 * (kTxMax + 40) + 2>();
        static const char* hx = "0123456789abcdef";
        size_t o = 0;
        memcpy(line, "@API ", 5); o = 5;
        for (size_t i = 0; i < n && o + 3 < sizeof(line); i++) { line[o++] = hx[d[i] >> 4]; line[o++] = hx[d[i] & 15]; }
        line[o++] = '\n';
        Serial.write((const uint8_t*)line, o);
        return true;
    }
    return g_client.connected() && g_client.write(d, n) == n;
}
bool writeNoiseFrame(const uint8_t* d, size_t n) {
    uint8_t hdr[3] = {0x01, (uint8_t)(n >> 8), (uint8_t)n};
    return writeRaw(hdr, 3) && (n == 0 || writeRaw(d, n));
}

void closeConn(const char* why);
void listSensors();

bool sendMessage(uint16_t type, const uint8_t* data, size_t n) {
    uint8_t* buf = g_txBuf;
    if (n + 4 + 16 > kTxMax) { Serial.printf("[HAV] message %u too big (%u)\n", type, (unsigned)n); return false; }
    if (g_conn == Conn::Plain) {
        size_t o = 0;
        buf[o++] = 0x00;
        for (uint32_t v = (uint32_t)n; ; ) { uint8_t b = v & 0x7f; v >>= 7; buf[o++] = b | (v ? 0x80 : 0); if (!v) break; }
        for (uint32_t v = type; ; ) { uint8_t b = v & 0x7f; v >>= 7; buf[o++] = b | (v ? 0x80 : 0); if (!v) break; }
        memcpy(buf + o, data, n);
        return writeRaw(buf, o + n);
    }
    if (g_conn != Conn::Ready) return false;
    uint8_t* plain = g_txPlain;
    plain[0] = (uint8_t)(type >> 8); plain[1] = (uint8_t)type;
    plain[2] = (uint8_t)(n >> 8);    plain[3] = (uint8_t)n;
    memcpy(plain + 4, data, n);
    uint8_t nonce[12];
    chachapoly::noiseNonce(g_nSend++, nonce);
    const size_t len = 4 + n;
    buf[0] = 0x01; buf[1] = (uint8_t)((len + 16) >> 8); buf[2] = (uint8_t)(len + 16);
    chachapoly::seal(g_kSend, nonce, nullptr, 0, plain, len, buf + 3);
    return writeRaw(buf, 3 + len + 16);
}

// ---- protobuf --------------------------------------------------------------------------------
struct PbOut {
    uint8_t b[1400];
    size_t n = 0;
    void varint(uint64_t v) { do { uint8_t x = v & 0x7f; v >>= 7; if (n < sizeof(b)) b[n++] = x | (v ? 0x80 : 0); } while (v); }
    void key(int f, int wt) { varint(((uint32_t)f << 3) | wt); }
    void u32(int f, uint32_t v) { if (v) { key(f, 0); varint(v); } }
    void boolean(int f, bool v) { if (v) { key(f, 0); varint(1); } }
    void bytes(int f, const void* d, size_t len) {
        key(f, 2); varint(len);
        if (n + len <= sizeof(b)) { memcpy(b + n, d, len); n += len; }
    }
    void str(int f, const char* s) { if (s && *s) bytes(f, s, strlen(s)); }
    void fixed32(int f, uint32_t v) { key(f, 5); for (int i = 0; i < 4; i++) if (n < sizeof(b)) b[n++] = (uint8_t)(v >> (8 * i)); }
    void f32(int f, float v) { key(f, 5); uint32_t u; memcpy(&u, &v, 4); for (int i = 0; i < 4; i++) if (n < sizeof(b)) b[n++] = (uint8_t)(u >> (8 * i)); }
};

struct PbIn {
    const uint8_t* p; size_t n; size_t i = 0;
    PbIn(const uint8_t* d, size_t len) : p(d), n(len) {}
    bool varint(uint64_t& v) {
        v = 0;
        for (int s = 0; s < 64 && i < n; s += 7) { const uint8_t b = p[i++]; v |= (uint64_t)(b & 0x7f) << s; if (!(b & 0x80)) return true; }
        return false;
    }
    // Next field: wire type 0 -> v, 2 -> d/len; 1 and 5 skipped (v = raw for 5).
    bool next(int& field, int& wt, uint64_t& v, const uint8_t*& d, size_t& len) {
        if (i >= n) return false;
        uint64_t k;
        if (!varint(k)) return false;
        field = (int)(k >> 3); wt = (int)(k & 7);
        if (wt == 0) return varint(v);
        if (wt == 2) { uint64_t l; if (!varint(l) || i + l > n) return false; d = p + i; len = (size_t)l; i += len; return true; }
        if (wt == 5) { if (i + 4 > n) return false; v = p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | ((uint32_t)p[i + 3] << 24); i += 4; return true; }
        if (wt == 1) { if (i + 8 > n) return false; i += 8; return true; }
        return false;
    }
};

// ---- messages ------------------------------------------------------------------------------
void sendEmpty(uint16_t type) { sendMessage(type, nullptr, 0); }

void sendDeviceInfo() {
    PbOut o;
    o.str(2, g_node);
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macs[18];
    snprintf(macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    o.str(3, macs);
    o.str(4, "2025.11.0");
    o.str(5, __DATE__ ", " __TIME__);
    o.str(6, "T-Deck Pro");
    o.str(12, "LilyGO");
    o.str(13, g_friendly);
    o.u32(17, kVaFeatures);
    o.boolean(19, g_encrypted);
    sendMessage(DEVICE_INFO_RESP, o.b, o.n);
}

void endRun(const char* why) {
    const bool wasWake = g_wakeListen;
    if (g_talking || g_wakeListen) { g_talking = false; g_wakeListen = false; g_audio->micStop(); }
    if (wasWake && !g_pendingTalk) {             // the run ended before a wake word: HA error?
        g_wakeBackoff = g_wakeBackoff ? (g_wakeBackoff * 2 > 60000 ? 60000 : g_wakeBackoff * 2) : 5000;
        g_wakeRetryAt = millis() + g_wakeBackoff;
    } else {
        g_wakeRetryAt = millis() + 1500;         // let the answer finish playing
    }
    g_streaming = false;
    g_runActive = false;
    g_playing = false;
    setStatus(why);
}

void handleEvent(const uint8_t* d, size_t n) {
    PbIn in(d, n);
    int f, wt; uint64_t v = 0; const uint8_t* s = nullptr; size_t len = 0;
    uint32_t type = 0;
    char text[96] = "", msg[64] = "";
    while (in.next(f, wt, v, s, len)) {
        if (f == 1 && wt == 0) type = (uint32_t)v;
        if (f == 2 && wt == 2) {                // VoiceAssistantEventData {name, value}
            PbIn e(s, len);
            int ef, ewt; uint64_t ev; const uint8_t* es; size_t elen;
            char name[24] = "", value[96] = "";
            while (e.next(ef, ewt, ev, es, elen)) {
                if (ewt != 2) continue;
                char* dst = ef == 1 ? name : ef == 2 ? value : nullptr;
                const size_t cap = ef == 1 ? sizeof(name) : sizeof(value);
                if (dst) { const size_t k = elen < cap - 1 ? elen : cap - 1; memcpy(dst, es, k); dst[k] = 0; }
            }
            if (!strcmp(name, "text")) strlcpy(text, value, sizeof(text));
            if (!strcmp(name, "message")) strlcpy(msg, value, sizeof(msg));
        }
    }
    switch (type) {
        case EV_RUN_START: g_runAt = millis(); break;
        case EV_WAKE_WORD_END:                   // HA heard the wake word: now it is a normal run
            if (g_wakeListen) {
                g_wakeListen = false;
                g_talking = true;
                g_talkStart = millis();
                g_wakeBackoff = 0;
                g_audio->writeTone(1200, 60);
                Serial.println("[HAV] wake word heard");
                setStatus("wake word: listening...");
            }
            break;
        case EV_STT_VAD_START: setStatus("listening (speech heard)"); break;
        case EV_STT_VAD_END: if (g_talking) stopTalk("end of speech"); break;
        case EV_STT_END:
            if (g_talking) stopTalk("HA has the words");
            strlcpy(g_heard, text, sizeof(g_heard));
            g_changed = millis() | 1;
            Serial.printf("[HAV] heard: %s\n", g_heard);
            setStatus("thinking...");
            break;
        case EV_TTS_START:
            strlcpy(g_answer, text, sizeof(g_answer));
            g_changed = millis() | 1;
            Serial.printf("[HAV] answer: %s\n", g_answer);
            break;
        case EV_TTS_STREAM_START: g_playing = true; g_runAt = millis(); setStatus("speaking"); break;
        case EV_TTS_STREAM_END: g_playing = false; break;
        case EV_RUN_END: endRun(g_wakeWanted ? "ready: wake word or MIC" : "ready: press MIC to talk"); break;
        case EV_ERROR: {
            char e[48];
            snprintf(e, sizeof(e), "HA: %.40s", msg[0] ? msg : "error");
            endRun(e);
            break;
        }
        default: break;
    }
}

void handleMessage(uint16_t type, const uint8_t* d, size_t n) {
    switch (type) {
        case HELLO_REQ: {
            PbOut o;
            o.u32(1, 1);                         // api_version 1.10
            o.u32(2, 10);
            o.str(3, "RatSpeak T-Deck Pro");
            o.str(4, g_node);
            sendMessage(HELLO_RESP, o.b, o.n);
            setStatus("HA connecting");
            break;
        }
        case AUTH_REQ: {                         // deprecated password login: no password here
            PbOut o;
            sendMessage(AUTH_RESP, o.b, o.n);
            break;
        }
        case DISCONNECT_REQ: sendEmpty(DISCONNECT_RESP); closeConn("HA disconnected"); break;
        case PING_REQ: sendEmpty(PING_RESP); break;
        case DEVICE_INFO_REQ: sendDeviceInfo(); break;
        case LIST_ENTITIES_REQ: listSensors(); sendEmpty(LIST_ENTITIES_DONE); break;
        case SUBSCRIBE_STATES: g_statesSubscribed = true; g_statesAt = 0; g_sentBat = -2; g_sentLat = 999; break;
        case SUBSCRIBE_LOGS: case SUBSCRIBE_HA_SERVICES: case SUBSCRIBE_HA_STATES:
        case GET_TIME_RESP: break;
        case VA_SUBSCRIBE: {
            PbIn in(d, n);
            int f, wt; uint64_t v = 0; const uint8_t* s; size_t len;
            bool sub = false;
            while (in.next(f, wt, v, s, len)) if (f == 1 && wt == 0) sub = v != 0;
            g_vaSubscribed = sub;
            setStatus(sub ? "ready: press MIC to talk" : "HA connected (voice off)");
            break;
        }
        case VA_RESPONSE: {
            PbIn in(d, n);
            int f, wt; uint64_t v = 0; const uint8_t* s; size_t len;
            bool err = false;
            while (in.next(f, wt, v, s, len)) if (f == 2 && wt == 0) err = v != 0;
            if (err) { endRun("HA could not start"); break; }
            if (g_talking) { g_streaming = true; setStatus("listening..."); }
            else if (g_wakeListen) { g_streaming = true; setStatus("listening for the wake word"); }
            break;
        }
        case VA_EVENT: handleEvent(d, n); break;
        case VA_AUDIO: {                         // Home Assistant's speech: 16 kHz mono PCM
            PbIn in(d, n);
            int f, wt; uint64_t v = 0; const uint8_t* s = nullptr; size_t len = 0;
            while (in.next(f, wt, v, s, len)) {
                if (f == 1 && wt == 2 && len >= 2) { g_playing = true; g_audio->writePcm((const int16_t*)s, len / 2); }
            }
            g_runAt = millis();
            break;
        }
        case VA_CONFIG_REQ: {
            PbOut o;                             // no wake words
            sendMessage(VA_CONFIG_RESP, o.b, o.n);
            break;
        }
        case VA_ANNOUNCE_REQ: {                  // HA announces: show the text (no media player here)
            PbIn in(d, n);
            int f, wt; uint64_t v = 0; const uint8_t* s = nullptr; size_t len = 0;
            char text[96] = "";
            while (in.next(f, wt, v, s, len))
                if (f == 2 && wt == 2) { const size_t k = len < sizeof(text) - 1 ? len : sizeof(text) - 1; memcpy(text, s, k); text[k] = 0; }
            strlcpy(g_answer, text[0] ? text : "(announcement without text)", sizeof(g_answer));
            g_heard[0] = 0;
            g_announcedAt = millis() | 1;
            g_changed = millis() | 1;
            Serial.printf("[HAV] announcement: %s%s\n", g_answer, g_quiet ? " (quiet hours: no chime)" : "");
            if (!g_quiet) g_audio->playMessage();
            PbOut o;
            o.boolean(1, true);                  // success
            sendMessage(VA_ANNOUNCE_DONE, o.b, o.n);
            break;
        }
        default:
            Serial.printf("[HAV] message %u ignored (%u bytes)\n", type, (unsigned)n);
            break;
    }
}

// ---- handshake -------------------------------------------------------------------------------
void noiseStart() {
    static const char* kName = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";
    sha256((const uint8_t*)kName, strlen(kName), g_h);   // name > 32 bytes: h = HASH(name)
    memcpy(g_ck, g_h, 32);
    g_hasK = false; g_n = 0;
}

// Client hello (any content): goes into the prologue; answer with the server hello.
void onClientHello(const uint8_t* d, size_t n) {
    static const char* kInit = "NoiseAPIInit";
    g_prologueLen = 0;
    memcpy(g_prologue, kInit, strlen(kInit)); g_prologueLen = strlen(kInit);
    g_prologue[g_prologueLen++] = (uint8_t)(n >> 8);
    g_prologue[g_prologueLen++] = (uint8_t)n;
    if (n && g_prologueLen + n <= sizeof(g_prologue)) { memcpy(g_prologue + g_prologueLen, d, n); g_prologueLen += n; }
    uint8_t hello[40];
    size_t o = 0;
    hello[o++] = 0x01;                           // Noise chosen
    const size_t nl = strlen(g_node);
    memcpy(hello + o, g_node, nl); o += nl;
    hello[o++] = 0;
    writeNoiseFrame(hello, o);
    g_conn = Conn::NoiseHandshake;
}

void handshakeFail(const char* why) {
    uint8_t m[40];
    m[0] = 0x01;
    const size_t k = strlen(why) < sizeof(m) - 1 ? strlen(why) : sizeof(m) - 1;
    memcpy(m + 1, why, k);
    writeNoiseFrame(m, k + 1);
    Serial.printf("[HAV] handshake failed: %s\n", why);
    closeConn("HA key does not match");
}

// Handshake message 1 from HA: [0x00] + e (32) + tag (16). Answer [0x00] + e (32) + tag (16).
void onHandshake(const uint8_t* d, size_t n) {
    if (n < 1 + 32 + 16 || d[0] != 0x00) { handshakeFail("Handshake error"); return; }
    noiseStart();
    mixHash(g_prologue, g_prologueLen);
    // <- psk, e
    mixKeyAndHash(g_psk, 32);
    const uint8_t* re = d + 1;
    mixHash(re, 32);
    mixKey(re, 32);
    if (!decryptAndHash(d + 33, n - 33)) { handshakeFail("Handshake MAC failure"); return; }
    // -> e, ee
    uint8_t ePriv[32], ePub[32], dh[32];
    static const uint8_t base[32] = {9};
    esp_fill_random(ePriv, 32);
    if (!x25519(ePriv, base, ePub) || !x25519(ePriv, re, dh)) { handshakeFail("Handshake error"); return; }
    uint8_t out[1 + 32 + 16];
    out[0] = 0x00;
    memcpy(out + 1, ePub, 32);
    mixHash(ePub, 32);
    mixKey(ePub, 32);
    mixKey(dh, 32);
    encryptAndHashEmpty(out + 33);
    writeNoiseFrame(out, sizeof(out));
    // Split: initiator -> responder uses k1, responder -> initiator k2.
    hkdf(g_ck, nullptr, 0, g_kRecv, g_kSend, nullptr);
    g_nSend = g_nRecv = 0;
    memset(ePriv, 0, sizeof(ePriv)); memset(dh, 0, sizeof(dh));
    g_conn = Conn::Ready;
    Serial.println("[HAV] encrypted session up");
}

void onNoiseFrame(const uint8_t* d, size_t n) {
    switch (g_conn) {
        case Conn::NoiseHello: onClientHello(d, n); break;
        case Conn::NoiseHandshake: onHandshake(d, n); break;
        case Conn::Ready: {
            uint8_t* plain = g_rxPlain;
            if (n < 16 + 4) { closeConn("bad frame"); return; }
            uint8_t nonce[12];
            chachapoly::noiseNonce(g_nRecv++, nonce);
            if (!chachapoly::open(g_kRecv, nonce, nullptr, 0, d, n - 16, plain)) { closeConn("decrypt failed"); return; }
            const uint16_t type = (uint16_t)((plain[0] << 8) | plain[1]);
            size_t len = (size_t)((plain[2] << 8) | plain[3]);
            if (len > n - 16 - 4) len = n - 16 - 4;
            handleMessage(type, plain + 4, len);
            break;
        }
        default: break;
    }
}

// Pull whole frames out of the receive buffer.
void processRx() {
    for (;;) {
        if (g_rxLen < 1) return;
        if (g_conn == Conn::Plain || (g_conn == Conn::NoiseHello && !g_encrypted)) {
            if (g_rx[0] != 0x00) { closeConn(g_rx[0] == 0x01 ? "HA wants encryption: set a key" : "bad frame"); return; }
            g_conn = Conn::Plain;
            size_t i = 1;
            uint64_t len = 0, type = 0;
            int s = 0;
            for (;; s += 7) { if (i >= g_rxLen) return; const uint8_t b = g_rx[i++]; len |= (uint64_t)(b & 0x7f) << s; if (!(b & 0x80)) break; }
            s = 0;
            for (;; s += 7) { if (i >= g_rxLen) return; const uint8_t b = g_rx[i++]; type |= (uint64_t)(b & 0x7f) << s; if (!(b & 0x80)) break; }
            if (len > kRxMax) { closeConn("frame too big"); return; }
            if (g_rxLen < i + len) return;
            handleMessage((uint16_t)type, g_rx + i, (size_t)len);
            if (g_conn == Conn::None) return;
            memmove(g_rx, g_rx + i + len, g_rxLen - i - len);
            g_rxLen -= i + len;
        } else {
            if (g_rx[0] != 0x01) {               // a plaintext client to an encrypted device
                const uint8_t err[] = {0x01, 'B', 'a', 'd', ' ', 'i', 'n', 'd', 'i', 'c', 'a', 't', 'o', 'r'};
                writeNoiseFrame(err, sizeof(err));
                closeConn("HA connected without the key");
                return;
            }
            if (g_rxLen < 3) return;
            const size_t len = (size_t)((g_rx[1] << 8) | g_rx[2]);
            if (len > kRxMax) { closeConn("frame too big"); return; }
            if (g_rxLen < 3 + len) return;
            onNoiseFrame(g_rx + 3, len);
            if (g_conn == Conn::None) return;
            memmove(g_rx, g_rx + 3 + len, g_rxLen - 3 - len);
            g_rxLen -= 3 + len;
        }
    }
}

// ListEntitiesSensorResponse for each offered sensor (before ListEntitiesDone).
void listSensor(uint32_t key, const char* id, const char* name, const char* unit, int decimals, const char* devClass,
                const char* icon) {
    PbOut o;
    o.str(1, id);
    o.fixed32(2, key);
    o.str(3, name);
    char uid[64];
    snprintf(uid, sizeof(uid), "%s%s", g_node, id);
    o.str(4, uid);
    o.str(5, icon);
    o.str(6, unit);
    o.u32(7, (uint32_t)decimals);
    o.str(9, devClass);
    o.u32(10, 1);                                // state_class measurement
    sendMessage(LIST_ENTITIES_SENSOR, o.b, o.n);
}

void listSensors() {
    if (g_sensors >= 1) listSensor(kKeyBattery, "battery", "Battery", "%", 0, "battery", "");
    if (g_sensors >= 2) {
        listSensor(kKeyLat, "latitude", "Latitude", "\xc2\xb0", 5, "", "mdi:latitude");
        listSensor(kKeyLon, "longitude", "Longitude", "\xc2\xb0", 5, "", "mdi:longitude");
    }
}

void sendState(uint32_t key, float v, bool missing) {
    PbOut o;
    o.fixed32(1, key);
    o.f32(2, v);
    o.boolean(3, missing);
    sendMessage(SENSOR_STATE, o.b, o.n);
}

// States: when HA subscribes, then on a change (1 point battery, ~20 m) or every 5 minutes.
void sendStates() {
    if (!g_sensors || !g_statesSubscribed) return;
    const bool due = !g_statesAt || millis() - g_statesAt > 300000;
    const bool batCh = g_batPct >= 0 && (g_sentBat < -1 || fabsf(g_batPct - g_sentBat) >= 1.0f);
    const bool posCh = g_sensors >= 2 && g_havePos && (fabs(g_lat - g_sentLat) > 0.0002 || fabs(g_lon - g_sentLon) > 0.0002);
    if (!due && !batCh && !posCh) return;
    if (!due && g_statesAt && millis() - g_statesAt < 10000) return;   // at most every 10 s
    sendState(kKeyBattery, g_batPct < 0 ? 0 : g_batPct, g_batPct < 0);
    g_sentBat = g_batPct;
    if (g_sensors >= 2) {
        sendState(kKeyLat, (float)g_lat, !g_havePos);
        sendState(kKeyLon, (float)g_lon, !g_havePos);
        g_sentLat = g_lat; g_sentLon = g_lon;
    }
    g_statesAt = millis() | 1;
    Serial.printf("[HAV] sensors sent: battery %.0f%%%s\n", g_batPct, g_sensors >= 2 ? (g_havePos ? ", position" : ", no position") : "");
}

void closeConn(const char* why) {
    g_statesSubscribed = false;
    if (g_talking || g_wakeListen) { g_talking = false; g_wakeListen = false; g_audio->micStop(); }
    g_pendingTalk = false;
    if (g_client) g_client.stop();
    if (g_tunnel) Serial.println("@API closed");
    g_conn = Conn::None;
    g_rxLen = 0;
    g_vaSubscribed = false;
    g_streaming = false;
    g_runActive = false;
    g_playing = false;
    setStatus(why);
}

void streamMic() {
  // Drain what the mic DMA holds (the loop may have been away longer than one 32 ms block).
  for (int round = 0; round < 16; round++) {
    static int16_t buf[512];
    const size_t n = g_audio->micRead(buf, 512, g_micGain);
    if (!n) return;
    if (!g_streaming) continue;                  // before HA answers the start: dropped
    for (size_t i = 0; i < n; i++) {
        const int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
        if (a > g_micPeak) g_micPeak = a;
    }
    g_micSamples += n;
    PbOut o;
    o.bytes(1, buf, n * 2);
    if (!sendMessage(VA_AUDIO, o.b, o.n)) {
        if (g_wakeListen) closeConn("connection lost"); else stopTalk("connection lost");
        return;
    }
  }
}

void serverStop() {
    closeConn("off");
    if (g_server) { g_server->end(); delete g_server; g_server = nullptr; }
    if (g_mdns) { MDNS.end(); g_mdns = false; }
    WiFi.setSleep(true);
}

void makeNodeName(const char* deviceName) {
    size_t j = 0;
    bool dash = false;
    for (const char* p = deviceName && deviceName[0] ? deviceName : "ratspeak"; *p && j < sizeof(g_node) - 1; p++) {
        const char c = (char)tolower((unsigned char)*p);
        if (isalnum((unsigned char)c)) { g_node[j++] = c; dash = false; }
        else if (!dash && j) { g_node[j++] = '-'; dash = true; }
    }
    while (j && g_node[j - 1] == '-') j--;
    g_node[j] = 0;
    if (!j) strlcpy(g_node, "ratspeak", sizeof(g_node));
    strlcpy(g_friendly, deviceName && deviceName[0] ? deviceName : "RatSpeak", sizeof(g_friendly));
}

}  // namespace

void begin(AudioNotify* audio) {
    g_audio = audio;
    if (!g_rx) g_rx = (uint8_t*)ps_malloc(kRxBuf);
    if (!g_txBuf) g_txBuf = (uint8_t*)ps_malloc(kTxMax + 32);
    if (!g_txPlain) g_txPlain = (uint8_t*)ps_malloc(kTxMax);
    if (!g_rxPlain) g_rxPlain = (uint8_t*)ps_malloc(kRxMax);
}

void tunnelStart(const char* keyB64) {
    strlcpy(g_keyB64, keyB64 ? keyB64 : "", sizeof(g_keyB64));
    g_encrypted = keyDecode(g_keyB64, g_psk);
    makeNodeName("ratspeak-tunnel");
    g_tunnel = true;
    g_rxLen = 0;
    g_conn = Conn::NoiseHello;
    g_vaSubscribed = false;
    Serial.printf("@API ready %s%s", g_encrypted ? "encrypted" : "plain", "\n");
}
void tunnelFeed(const char* hex) {
    if (!g_tunnel) return;
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (; hex[0] && hex[1] && g_rxLen < kRxBuf; hex += 2) g_rx[g_rxLen++] = (uint8_t)(v(hex[0]) << 4 | v(hex[1]));
    processRx();
}
void tunnelStop() { if (g_tunnel) { closeConn("tunnel off"); g_tunnel = false; } }

bool beginTalk();
bool startWakeListen();

// The voice run: mic streaming, time limits, the push-to-talk that waits for the wake word
// run to end, and listening for the wake word again (WiFi and the USB tunnel alike).
void runTick() {
    sendStates();
    if (g_talking) {
        streamMic();
        if (millis() - g_talkStart > kMaxTalkMs) stopTalk("15 s limit");
    } else if (g_wakeListen) {
        streamMic();
    }
    if (g_runActive && !g_talking && !g_wakeListen && !g_playing && !g_pendingTalk &&
        millis() - g_runAt > kRunTimeoutMs) endRun("no answer from HA");
    if (g_pendingTalk && (!g_runActive || millis() - g_pendingAt > 1500)) {
        g_pendingTalk = false;
        g_runActive = false;
        beginTalk();
    }
    if (g_wakeWanted && connected() && !g_runActive && !g_talking && !g_playing && !g_pendingTalk &&
        (int32_t)(millis() - g_wakeRetryAt) >= 0)
        startWakeListen();
}

void tick(bool wifiUp, bool enabled, const char* deviceName, const char* keyB64) {
    if (!g_audio || !g_rx || !g_txBuf || !g_txPlain || !g_rxPlain) return;
    if (g_tunnel) {
        runTick();
        return;
    }
    const char* key = keyB64 ? keyB64 : "";
    if (strcmp(key, g_keyB64) != 0) {            // key changed: drop the server, start again
        if (g_server) serverStop();
        strlcpy(g_keyB64, key, sizeof(g_keyB64));
        g_encrypted = keyDecode(g_keyB64, g_psk);
    }
    if (!enabled || !wifiUp) {
        if (g_server || g_mdns) serverStop();
        setStatus(enabled ? "waiting for WiFi" : "off");
        return;
    }
    if (!g_server) {
        makeNodeName(deviceName);
        g_server = new WiFiServer(kPort);
        g_server->begin();
        g_server->setNoDelay(true);
        g_mdns = MDNS.begin(g_node);
        if (g_mdns) {
            MDNS.addService("esphomelib", "tcp", kPort);
            uint8_t mac[6];
            WiFi.macAddress(mac);
            char macs[13];
            snprintf(macs, sizeof(macs), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            MDNS.addServiceTxt("esphomelib", "tcp", "mac", String(macs));
            MDNS.addServiceTxt("esphomelib", "tcp", "version", "2025.11.0");
            MDNS.addServiceTxt("esphomelib", "tcp", "friendly_name", String(g_friendly));
            MDNS.addServiceTxt("esphomelib", "tcp", "platform", "ESP32");
            MDNS.addServiceTxt("esphomelib", "tcp", "board", "esp32-s3-devkitc-1");
            MDNS.addServiceTxt("esphomelib", "tcp", "network", "wifi");
            if (g_encrypted) MDNS.addServiceTxt("esphomelib", "tcp", "api_encryption", "Noise_NNpsk0_25519_ChaChaPoly_SHA256");
        }
        Serial.printf("[HAV] ESPHome API on %s:%u, node %s, %s (mDNS %s)\n", WiFi.localIP().toString().c_str(),
                      (unsigned)kPort, g_node, g_encrypted ? "encrypted" : "NO encryption key", g_mdns ? "ok" : "failed");
        setStatus("waiting for Home Assistant");
    }
    if (g_server->hasClient()) {
        WiFiClient c = g_server->available();
        if (g_client && g_client.connected()) closeConn("HA reconnected");
        g_client = c;
        g_client.setNoDelay(true);
        g_rxLen = 0;
        g_conn = Conn::NoiseHello;              // the first byte tells encrypted / plain
        g_vaSubscribed = false;
        Serial.printf("[HAV] connection from %s\n", g_client.remoteIP().toString().c_str());
    }
    if (g_client && !g_client.connected() && g_conn != Conn::None) closeConn("waiting for Home Assistant");
    if (!g_client || g_conn == Conn::None) return;

    // Read what has arrived (bounded per call).
    for (int k = 0; k < 8 && g_client.available() && g_conn != Conn::None; k++) {
        const int room = (int)(kRxBuf - g_rxLen);
        if (room <= 0) { closeConn("receive overflow"); return; }
        const int got = g_client.read(g_rx + g_rxLen, room);
        if (got <= 0) break;
        g_rxLen += got;
        processRx();
    }
    runTick();
    WiFi.setSleep(!busy());
}

bool connected() { return (g_tunnel || (g_client && g_client.connected())) && (g_conn == Conn::Ready || g_conn == Conn::Plain) && g_vaSubscribed; }
bool talking() { return g_talking; }
bool busy() { return g_talking || g_runActive || g_playing || g_wakeListen || g_pendingTalk; }
bool wakeListening() { return g_wakeListen; }

// Abort the open wake word run (MIC pressed, or the wake word turned off).
void stopWakeListen(const char* why) {
    if (!g_wakeListen) return;
    g_wakeListen = false;
    g_streaming = false;
    g_audio->micStop();
    PbOut o;
    o.boolean(1, false);                         // start = false: end the run
    sendMessage(VA_REQUEST, o.b, o.n);
    Serial.printf("[HAV] wake word listening stopped (%s)\n", why);
}

bool startWakeListen() {
    if (!g_audio->micStart()) { setStatus("microphone failed"); g_wakeRetryAt = millis() + 30000; return false; }
    PbOut o;
    o.boolean(1, true);                          // start
    o.u32(3, kFlagUseWakeWord);                  // HA starts the run at its wake word stage
    PbOut as;
    as.f32(3, 1.0f);
    o.bytes(4, as.b, as.n);
    sendMessage(VA_REQUEST, o.b, o.n);
    g_wakeListen = true;
    g_streaming = false;
    g_runActive = true;
    g_runAt = millis();
    g_micSamples = 0;
    g_micPeak = 0;
    Serial.println("[HAV] listening for the wake word");
    setStatus("starting wake word...");
    return true;
}

bool beginTalk() {
    if (!g_audio->micStart()) { setStatus("microphone failed"); return false; }
    PbOut o;
    o.boolean(1, true);                          // start
    PbOut as;                                    // audio settings: volume x1
    as.f32(3, 1.0f);
    o.bytes(4, as.b, as.n);
    sendMessage(VA_REQUEST, o.b, o.n);
    g_talking = true;
    g_streaming = false;
    g_talkStart = millis();
    g_runActive = true;
    g_runAt = millis();
    g_micSamples = 0;
    g_micPeak = 0;
    g_heard[0] = 0;
    g_answer[0] = 0;
    setStatus("starting...");
    return true;
}

bool startTalk() {
    if (!connected()) { setStatus(g_client ? "HA has not turned voice on" : "Home Assistant not connected"); return false; }
    if (g_talking || g_pendingTalk) return true;
    if (g_wakeListen) {                          // MIC while listening for the wake word: end that
        stopWakeListen("MIC pressed");           // run, start push-to-talk once HA has ended it
        g_pendingTalk = true;
        g_pendingAt = millis();
        setStatus("starting...");
        return true;
    }
    if (g_runActive) { setStatus("busy: HA is still answering"); return false; }   // one run at a time
    return beginTalk();
}

void setSensors(uint8_t mode) {
    if (mode > 2) mode = 0;
    if (mode == g_sensors) return;
    g_sensors = mode;
    Serial.printf("[HAV] sensors for HA: %s\n", mode == 0 ? "off" : mode == 1 ? "battery" : "battery + position");
    // The entity list changes: drop the connection, HA reconnects and lists them again.
    if (g_conn != Conn::None && !g_tunnel) closeConn("sensors changed: HA reconnects");
}
void setSensorValues(float batPct, bool havePos, double lat, double lon) {
    g_batPct = batPct; g_havePos = havePos; g_lat = lat; g_lon = lon;
}
void setQuiet(bool on) { g_quiet = on; }
uint32_t announcedAt() { return g_announcedAt; }

void setWakeWord(bool on) {
    if (on == g_wakeWanted) return;
    g_wakeWanted = on;
    g_wakeBackoff = 0;
    g_wakeRetryAt = millis();
    Serial.printf("[HAV] wake word %s\n", on ? "on" : "off");
    if (!on && g_wakeListen) { stopWakeListen("wake word off"); g_runActive = false; setStatus("ready: press MIC to talk"); }
}

void stopTalk(const char* why) {
    if (!g_talking) return;
    g_talking = false;
    g_streaming = false;
    g_audio->micStop();
    // End of speech: the last VoiceAssistantAudio with end=true, so HA finishes the run (as an
    // ESPHome device does). VoiceAssistantRequest start=false would abort it (nothing heard).
    PbOut o;
    o.boolean(2, true);
    sendMessage(VA_AUDIO, o.b, o.n);
    g_runAt = millis();
    Serial.printf("[HAV] mic stopped (%s): %lu ms of audio sent in %lu ms, peak %ld\n", why,
                  (unsigned long)(g_micSamples * 1000ULL / kRate), (unsigned long)(millis() - g_talkStart),
                  (long)g_micPeak);
    setStatus("thinking...");
}

const char* status() { return g_status; }
const char* heard() { return g_heard; }
const char* answer() { return g_answer; }
uint32_t changedAt() { return g_changed; }
void setMicGain(uint8_t g) { g_micGain = g ? g : 1; }
const char* nodeName() { return g_node; }

bool selfTest() {
    auto hex = [](const char* s, uint8_t* out) {
        for (int i = 0; i < 32; i++) {
            auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            out[i] = (uint8_t)(v(s[2 * i]) << 4 | v(s[2 * i + 1]));
        }
    };
    uint8_t a[32], b[32], aPub[32], bPub[32], s1[32], s2[32], want[32], base[32] = {9};
    hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a);
    hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b);
    bool ok = x25519(a, base, aPub) && x25519(b, base, bPub) && x25519(a, bPub, s1) && x25519(b, aPub, s2);
    hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", want);
    const bool pubOk = ok && !memcmp(aPub, want, 32);
    hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", want);
    const bool shOk = ok && !memcmp(s1, want, 32) && !memcmp(s2, want, 32);
    // ChaCha20-Poly1305 (RFC 8439 2.8.2) tag of the sunscreen text.
    uint8_t key[32], nonce[12] = {0x07, 0, 0, 0, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47}, out[160];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
    const uint8_t ad[12] = {0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7};
    const char* pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    const size_t n = strlen(pt);
    chachapoly::seal(key, nonce, ad, 12, (const uint8_t*)pt, n, out);
    const uint8_t tag[16] = {0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a, 0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91};
    const bool aeadOk = !memcmp(out + n, tag, 16);
    Serial.printf("[HAV] self-test: X25519 public %s, shared %s; ChaCha20-Poly1305 %s\n", pubOk ? "ok" : "FAIL",
                  shOk ? "ok" : "FAIL", aeadOk ? "ok" : "FAIL");
    return pubOk && shOk && aeadOk;
}

}  // namespace havoice

#endif  // !SIMULATOR
