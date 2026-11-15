// Live walkie-talkie over WiFi. See Walkie.h.
#if !defined(SIMULATOR)

#include "Walkie.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <string.h>

#include "ChaChaPoly.h"
#include "HaVoice.h"          // base64 key helpers
#include "VoiceCodec.h"
#include "audio/AudioNotify.h"

namespace walkie {
namespace {

// Packet: [type][seq u32 LE][ciphertext + 16-byte tag]; additional data = the first 5 bytes.
// Nonce: 4 bytes direction ("CALR" caller->callee, "CALE" callee->caller) + seq as u64 LE.
enum : uint8_t { PKT_HELLO = 1, PKT_AUDIO = 2, PKT_BYE = 3, PKT_KEEP = 4 };
constexpr int kFramesPerPacket = 5;          // 5 x 20 ms = 100 ms of Codec2 3200 per packet
constexpr uint32_t kCallTimeoutMs = 60000;   // the callee did not answer
constexpr uint32_t kLostMs = 10000;          // nothing heard: the call is gone
constexpr uint32_t kKeepMs = 2000;
constexpr int kJitterFrames = 10;            // start playing after 200 ms is buffered
constexpr int kRingFrames = 150;             // 3 s of frames waiting to be played

AudioNotify* g_audio = nullptr;
TaskHandle_t g_task = nullptr;
int g_sock = -1;
uint8_t g_key[32];
bool g_isCaller = false;
volatile State g_state = State::Idle;
volatile bool g_talk = false;
volatile bool g_hangReq = false;
sockaddr_in g_peer = {};
volatile bool g_havePeer = false;
uint32_t g_txSeq = 0, g_rxSeq = 0;
volatile uint32_t g_lastRx = 0, g_lastTx = 0, g_lastAudioRx = 0, g_startAt = 0;
char g_status[48] = "";
volatile uint32_t g_changed = 0;

// Received Codec2 frames waiting for the speaker (8 bytes each), single producer/consumer
// (both in the task).
volatile uint32_t g_stTx = 0, g_stRx = 0, g_stPlayed = 0, g_stDropped = 0, g_stBad = 0;
uint8_t g_ring[kRingFrames][8];
int g_rHead = 0, g_rTail = 0;
bool g_playing = false;

void setStatus(const char* s) {
    strlcpy(g_status, s, sizeof(g_status));
    g_changed = millis() | 1;
    Serial.printf("[WT] %s\n", s);
}

void nonceFor(bool fromCaller, uint32_t seq, uint8_t n[12]) {
    memcpy(n, fromCaller ? "CALR" : "CALE", 4);
    for (int i = 0; i < 8; i++) n[4 + i] = (uint8_t)((uint64_t)seq >> (8 * i));
}

bool sendPkt(uint8_t type, const uint8_t* data, size_t n) {
    if (g_sock < 0 || !g_havePeer) return false;
    uint8_t pkt[5 + 64 + 16];
    if (n > 64) return false;
    const uint32_t seq = ++g_txSeq;
    pkt[0] = type;
    for (int i = 0; i < 4; i++) pkt[1 + i] = (uint8_t)(seq >> (8 * i));
    uint8_t nonce[12];
    nonceFor(g_isCaller, seq, nonce);
    chachapoly::seal(g_key, nonce, pkt, 5, data, n, pkt + 5);
    const int r = sendto(g_sock, pkt, 5 + n + 16, 0, (const sockaddr*)&g_peer, sizeof(g_peer));
    if (r > 0) { g_lastTx = millis(); g_stTx++; }
    return r > 0;
}

void ringPush(const uint8_t* f) {
    const int next = (g_rHead + 1) % kRingFrames;
    if (next == g_rTail) { g_rTail = (g_rTail + 1) % kRingFrames; g_stDropped++; }   // full: drop the oldest
    memcpy(g_ring[g_rHead], f, 8);
    g_rHead = next;
}
int ringCount() { return (g_rHead - g_rTail + kRingFrames) % kRingFrames; }

void onPacket(const uint8_t* p, int n, const sockaddr_in& from) {
    if (n < 5 + 16) return;
    const uint8_t type = p[0];
    const uint32_t seq = p[1] | (p[2] << 8) | (p[3] << 16) | ((uint32_t)p[4] << 24);
    if (seq <= g_rxSeq) return;                  // old or replayed
    uint8_t nonce[12], plain[64];
    nonceFor(!g_isCaller, seq, nonce);
    const size_t len = (size_t)n - 5 - 16;
    if (len > sizeof(plain) || !chachapoly::open(g_key, nonce, p, 5, p + 5, len, plain)) { g_stBad++; return; }
    g_stRx++;
    g_rxSeq = seq;
    g_lastRx = millis();
    if (g_isCaller && !g_havePeer) {             // the callee's first packet: its address
        g_peer = from;
        g_havePeer = true;
    }
    if (g_state == State::Calling) {
        g_state = State::Connected;
        setStatus("connected");
        sendPkt(PKT_KEEP, nullptr, 0);
    }
    if (type == PKT_BYE) { g_state = State::Ended; setStatus("the other side hung up"); return; }
    if (type == PKT_AUDIO && !g_talk) {
        for (size_t i = 0; i + 8 <= len; i += 8) ringPush(plain + i);
        g_lastAudioRx = millis();
    }
}

void closeSock() {
    if (g_sock >= 0) { close(g_sock); g_sock = -1; }
}

void taskFn(void*) {
    static int16_t pcm[320];
    static uint8_t frames[kFramesPerPacket * 8];
    int nFrames = 0, have = 0;
    bool micOn = false;
    voicecodec::begin(3200);
    for (;;) {
        if (g_hangReq || g_state == State::Ended) break;
        // network in
        uint8_t buf[128];
        sockaddr_in from;
        socklen_t fl = sizeof(from);
        for (int k = 0; k < 8; k++) {
            const int r = recvfrom(g_sock, buf, sizeof(buf), MSG_DONTWAIT, (sockaddr*)&from, &fl);
            if (r <= 0) break;
            onPacket(buf, r, from);
        }
        // talk: mic -> Codec2 -> packets
        if (g_talk && g_state == State::Connected) {
            if (!micOn) {
                static uint32_t triedAt = 0;
                if (triedAt && millis() - triedAt < 1000) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
                triedAt = millis();
                micOn = g_audio->micStart();
                if (!micOn) { setStatus("microphone failed"); g_talk = false; continue; }
                triedAt = 0;
                have = 0; nFrames = 0; g_rHead = g_rTail = 0; g_playing = false;
            }
            have += (int)g_audio->micRead(pcm + have, 320 - have, 4);
            if (have == 320) {
                voicecodec::encode(pcm, frames + 8 * nFrames);
                have = 0;
                if (++nFrames == kFramesPerPacket) { sendPkt(PKT_AUDIO, frames, sizeof(frames)); nFrames = 0; }
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            continue;
        }
        if (micOn) { g_audio->micStop(); micOn = false; }
        // listen: frames -> Codec2 -> speaker (writePcm paces this at real time)
        const int count = ringCount();
        if (!g_playing && count >= kJitterFrames) g_playing = true;
        if (g_playing && count == 0) g_playing = false;      // ran dry: buffer again
        if (g_playing) {
            voicecodec::decode(g_ring[g_rTail], pcm);
            g_rTail = (g_rTail + 1) % kRingFrames;
            g_audio->writePcm(pcm, 320);
            g_stPlayed++;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (g_state != State::Idle && millis() - g_lastTx > kKeepMs) sendPkt(PKT_KEEP, nullptr, 0);
    }
    if (micOn) g_audio->micStop();
    if (g_havePeer) { sendPkt(PKT_BYE, nullptr, 0); sendPkt(PKT_BYE, nullptr, 0); }
    voicecodec::end();
    closeSock();
    g_task = nullptr;
    vTaskDelete(nullptr);
}

bool openCall(bool caller, const uint8_t key[32]) {
    if (g_task) return false;
    if (WiFi.status() != WL_CONNECTED && !(WiFi.getMode() & WIFI_MODE_AP)) { setStatus("needs WiFi"); return false; }
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock < 0) { setStatus("no socket"); return false; }
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(kPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, (sockaddr*)&a, sizeof(a)) < 0) { closeSock(); setStatus("port busy"); return false; }
    memcpy(g_key, key, 32);
    g_isCaller = caller;
    g_txSeq = g_rxSeq = 0;
    g_talk = false;
    g_hangReq = false;
    g_rHead = g_rTail = 0;
    g_playing = false;
    g_startAt = g_lastRx = g_lastTx = millis();
    g_stTx = g_stRx = g_stPlayed = g_stDropped = g_stBad = 0;
    g_state = State::Calling;
    // 20 KB: Codec2 encode (its pitch detector) uses ~14 KB of stack; 12 KB overflowed.
    if (xTaskCreatePinnedToCore(taskFn, "walkie", 20480, nullptr, 4, &g_task, 0) != pdPASS) {
        closeSock();
        g_state = State::Idle;
        setStatus("no task");
        return false;
    }
    return true;
}

}  // namespace

void begin(AudioNotify* audio) { g_audio = audio; }

bool startCall(char* invite, size_t cap, uint32_t myIp, const char* apSsid, const char* apPass) {
    uint8_t key[32];
    esp_fill_random(key, sizeof(key));
    g_havePeer = false;
    if (!openCall(true, key)) return false;
    char k64[havoice::kKeyB64Len + 1];
    havoice::keyEncode(key, k64);
    int n = snprintf(invite, cap, "%s%u.%u.%u.%u:%u %s", kInvitePrefix, (unsigned)(myIp >> 24), (unsigned)(myIp >> 16 & 255),
                     (unsigned)(myIp >> 8 & 255), (unsigned)(myIp & 255), (unsigned)kPort, k64);
    if (apSsid && apSsid[0] && n > 0 && (size_t)n < cap)
        snprintf(invite + n, cap - n, " ap=%s %s", apSsid, apPass ? apPass : "");
    setStatus("calling...");
    return true;
}

bool parseInvite(const char* text, uint32_t& ip, uint16_t& port, uint8_t key[32], char* ssid, size_t ssidCap,
                 char* pass, size_t passCap) {
    if (strncmp(text, kInvitePrefix, strlen(kInvitePrefix)) != 0) return false;
    const char* p = text + strlen(kInvitePrefix);
    unsigned a, b, c, d, pt;
    char k64[64], s[40] = "", w[40] = "";
    if (sscanf(p, "%u.%u.%u.%u:%u %63s ap=%39s %39s", &a, &b, &c, &d, &pt, k64, s, w) < 6) return false;
    if (ssid && ssidCap) strlcpy(ssid, s, ssidCap);
    if (pass && passCap) strlcpy(pass, w, passCap);
    if (a > 255 || b > 255 || c > 255 || d > 255 || !pt || pt > 65535) return false;
    if (!havoice::keyDecode(k64, key)) return false;
    ip = (uint32_t)a << 24 | b << 16 | c << 8 | d;
    port = (uint16_t)pt;
    return true;
}

bool answer(uint32_t ip, uint16_t port, const uint8_t key[32]) {
    g_peer = {};
    g_peer.sin_family = AF_INET;
    g_peer.sin_port = htons(port);
    g_peer.sin_addr.s_addr = htonl(ip);
    g_havePeer = true;
    if (!openCall(false, key)) { g_havePeer = false; return false; }
    setStatus("answering...");
    for (int i = 0; i < 3; i++) sendPkt(PKT_HELLO, (const uint8_t*)"hi", 2);   // the caller learns our address
    return true;
}

void hangUp(const char* why) {
    if (!g_task) { if (g_state != State::Idle) { g_state = State::Idle; setStatus(why); } return; }
    g_hangReq = true;
    for (int i = 0; i < 50 && g_task; i++) delay(10);
    g_state = State::Idle;
    setStatus(why);
}

void setTalking(bool on) {
    if (g_state != State::Connected) return;
    g_talk = on;
    setStatus(on ? "TALKING" : "listening");
}

bool talking() { return g_talk; }
bool peerTalking() { return g_lastAudioRx && millis() - g_lastAudioRx < 500; }
State state() { return g_state; }
const char* status() { return g_status; }
uint32_t changedAt() { return g_changed; }

void tick() {
    const uint32_t now = millis();
    if (g_state == State::Calling && now - g_startAt > kCallTimeoutMs) hangUp("no answer");
    else if (g_state == State::Connected && now - g_lastRx > kLostMs) hangUp("call lost");
    else if (g_state == State::Ended) hangUp("call ended");
    static uint32_t lastLog = 0;
    if (g_state == State::Connected && now - lastLog > 5000) {
        lastLog = now;
        Serial.printf("[WT] %s: packets tx %lu rx %lu bad %lu, frames played %lu dropped %lu, buffered %d\n",
                      g_talk ? "talking" : "listening", (unsigned long)g_stTx, (unsigned long)g_stRx,
                      (unsigned long)g_stBad, (unsigned long)g_stPlayed, (unsigned long)g_stDropped, ringCount());
    }
    static bool lastPeer = false;
    const bool p = peerTalking();
    if (p != lastPeer) { lastPeer = p; g_changed = now | 1; }
}

}  // namespace walkie

#endif  // !SIMULATOR
