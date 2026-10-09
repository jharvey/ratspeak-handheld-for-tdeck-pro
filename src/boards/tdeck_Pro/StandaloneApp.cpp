#include "StandaloneApp.h"

#include <SPI.h>
#include <Wire.h>
#include <lvgl.h>
#include <Preferences.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <esp_heap_caps.h>

#include "config/BoardConfig.h"
#include "Eink/DisplayEink.h"
#include "Eink/LvglPort.h"
#include "hal/Keyboard.h"
#include "radio/SX1262.h"
#include "storage/FlashStore.h"
#include "storage/MessageStore.h"
#include "storage/ConversationView.h"
#include "reticulum/IdentityManager.h"
#include "reticulum/AnnounceManager.h"
#include "transport/LoRaInterface.h"
#include "protocol/ProtocolRuntime.h"
#include "protocol/OutgoingContract.h"
#include "runtime/TaskOwner.h"
#include "ratspeak_protocol.h"
#include "util/Bytes.h"

namespace standalone {
namespace {

DisplayEink g_display;
Keyboard g_kb;
bool g_kbOk = false;

FlashStore g_flash;
IdentityManager g_idMgr;
MessageStore g_msgStore;
AnnounceManager g_announceMgr;
ProtocolRuntime g_proto;
bool g_storeReady = false;
bool g_protoReady = false;

SX1262* g_radio = nullptr;
LoRaInterface* g_loraIf = nullptr;

enum ScreenId : uint8_t { SCR_HOME = 0, SCR_MESSAGES, SCR_SETTINGS, SCR_COUNT };
ScreenId g_screen = SCR_HOME;
lv_obj_t* g_root = nullptr;

char g_batStr[20]    = "n/a";
char g_pctStr[8]     = "--%";
char g_destHex[17]   = "................";
char g_loraStr[12]   = "off";
char g_rssiStr[16]   = "-";
char g_snrStr[12]    = "-";
char g_pathsStr[8]   = "0";
char g_linksStr[8]   = "0";
char g_lxmfqStr[8]   = "0";
char g_convStr[24]   = "0 conv";
char g_unreadStr[16] = "0 unread";
char g_announceStr[20] = "never";
char g_txStr[24]     = "-";

constexpr int MSG_ROWS = 4;
struct MsgRow {
    char line1[28];
    char line2[28];
    uint8_t peer[16];
    uint32_t unread;
    bool used;
};
MsgRow g_msgRows[MSG_ROWS];
int g_msgRowCount = 0;
char g_msgNote[40] = "";
bool g_msgRowsLoaded = false;
uint32_t g_msgCacheRevision = 0;

constexpr int HIST_ROWS = 4;
struct HistRow {
    char line[28];
    bool used;
};
HistRow g_histRows[HIST_ROWS];
int g_histRowCount = 0;
bool g_showHistory = false;
char g_histNote[40] = "";

bool g_bootAnnDone = false;
uint32_t g_bootAnnAt = 0;

uint32_t g_lastUiRevision = 0;
uint32_t g_lastMsgRedrawMs = 0;
constexpr uint32_t MSG_REDRAW_MIN_MS = 2500;

void peerToHex32(const uint8_t peer[16], char out[33]);
void show_screen(ScreenId id);
void doAnnounce();
void doSendTest(const char* body);
void loadMessageRows(bool force);
void loadHistoryRows();

void spiBusIdle() {
    pinMode(EPD_CS, OUTPUT); digitalWrite(EPD_CS, HIGH);
    pinMode(LORA_CS, OUTPUT); digitalWrite(LORA_CS, HIGH);
#ifdef SD_CS
    pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH);
#endif
}

bool readBattery(float& volts, int& pct) {
    Wire.beginTransmission(BQ27220_I2C_ADDR);
    Wire.write(0x08);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_I2C_ADDR, 2) != 2) return false;
    uint16_t mv = Wire.read() | (Wire.read() << 8);
    volts = mv / 1000.0f;
    Wire.beginTransmission(BQ27220_I2C_ADDR);
    Wire.write(0x2C);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)BQ27220_I2C_ADDR, 2) != 2) return false;
    pct = Wire.read() | (Wire.read() << 8);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return true;
}

bool fillDestFromNvs() {
    Preferences prefs;
    uint8_t priv[64] = {0};
    if (!prefs.begin(NVS_NS_IDENTITY, true)) return false;
    size_t n = prefs.getBytes("privkey", priv, 64);
    prefs.end();
    if (n != 64) return false;
    rs_handheld_rns_t* ctx = nullptr;
    if (rs_handheld_rns_init(&ctx) != RS_HANDHELD_OK || !ctx) return false;
    if (rs_handheld_rns_load_identity(ctx, priv) != RS_HANDHELD_OK) {
        rs_handheld_rns_shutdown(ctx);
        return false;
    }
    uint8_t dest[16];
    if (rs_handheld_rns_destination_hash(ctx, dest) != RS_HANDHELD_OK) {
        rs_handheld_rns_shutdown(ctx);
        return false;
    }
    for (int i = 0; i < 16; i++) sprintf(g_destHex + i * 2, "%02x", dest[i]);
    g_destHex[16] = '\0';
    rs_handheld_rns_shutdown(ctx);
    Serial.printf("[DEST] NVS %s\n", g_destHex);
    return true;
}

bool initRadio() {
    spiBusIdle();
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    delay(10);

    g_radio = new SX1262(&SPI, LORA_CS, SPI_SCK, SPI_MOSI, SPI_MISO,
                         LORA_RST, LORA_IRQ, LORA_BUSY, LORA_RXEN,
                         LORA_HAS_TCXO, LORA_DIO2_AS_RF_SWITCH);
    if (!g_radio || !g_radio->begin(LORA_DEFAULT_FREQ)) {
        if (g_radio) { delete g_radio; g_radio = nullptr; }
        return false;
    }
    g_radio->setSpreadingFactor(LORA_DEFAULT_SF);
    g_radio->setSignalBandwidth(LORA_DEFAULT_BW);
    g_radio->setCodingRate4(LORA_DEFAULT_CR);
    g_radio->setTxPower(LORA_DEFAULT_TX_POWER);
    g_radio->setPreambleLength(LORA_DEFAULT_PREAMBLE);
    g_radio->receive();
    g_loraIf = new LoRaInterface(g_radio, "LoRa");
    if (!g_loraIf || !g_loraIf->start()) return false;
    Serial.printf("[LORA] online freq=%lu SF=%d\n",
                  (unsigned long)LORA_DEFAULT_FREQ, LORA_DEFAULT_SF);
    return true;
}

void logPsram() {
    Serial.printf("[PSRAM] found=%d size=%u free=%u  caps_spiram=%u caps_int=%u\n",
                  (int)psramFound(), (unsigned)ESP.getPsramSize(),
                  (unsigned)ESP.getFreePsram(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

bool tryProto(uint32_t caps, const char* label) {
    Serial.printf("[PROTO] try %s free_int=%u\n", label,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return g_proto.begin(&g_flash, nullptr, &g_idMgr, &g_msgStore,
                         &g_announceMgr, RS_HANDHELD_PROFILE_SMALL, caps);
}

bool initStorageAndProto() {
    if (!g_flash.begin()) return false;
    g_idMgr.begin(&g_flash, nullptr);
    if (!g_msgStore.begin(&g_flash, nullptr, false)) return false;
    g_storeReady = true;
    g_announceMgr.setStorage(nullptr, &g_flash);
    g_announceMgr.loadNameCache();

    logPsram();

    const size_t psSize = ESP.getPsramSize();
    const size_t psFree = ESP.getFreePsram();
    if (psramFound() && psSize > 0 && psFree > 250000) {
        if (tryProto(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, "SPIRAM|8BIT") ||
            tryProto(MALLOC_CAP_DEFAULT, "DEFAULT"))
            g_protoReady = true;
    }
    if (!g_protoReady) {
        if (tryProto(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "INTERNAL|8BIT") ||
            tryProto(MALLOC_CAP_INTERNAL, "INTERNAL") ||
            tryProto(MALLOC_CAP_8BIT, "8BIT"))
            g_protoReady = true;
    }

    if (g_protoReady) {
        if (g_loraIf) {
            g_proto.pump().attachLoRa(g_loraIf);
            g_announceMgr.setLoRaInterface(g_loraIf);
        }
        const uint8_t* dest = g_proto.localDestHash();
        if (dest) {
            for (int i = 0; i < 16; i++) sprintf(g_destHex + i * 2, "%02x", dest[i]);
            g_destHex[16] = '\0';
            g_announceMgr.setLocalDestHash(rs::Bytes(dest, 16));
        }
        Serial.printf("[PROTO] up dest=%s free_int=%u\n", g_destHex,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    } else {
        Serial.println("[PROTO] off — store-only");
        fillDestFromNvs();
    }
    return true;
}

void doAnnounce() {
    if (!g_protoReady) {
        strcpy(g_announceStr, "proto off");
        Serial.println("[ANN] proto off");
        return;
    }
    static unsigned long lastMs = 0;
    if (lastMs != 0 && (millis() - lastMs) < 5000UL) {
        strcpy(g_announceStr, "wait 5s");
        Serial.println("[ANN] rate limited (5s)");
        return;
    }
    auto r = g_proto.announce(nullptr, 0);
    lastMs = millis();
    switch (r) {
        case ProtocolBackend::AnnounceResult::Sent:
            strcpy(g_announceStr, "sent");
            Serial.printf("[ANN] sent dest=%s\n", g_destHex);
            break;
        case ProtocolBackend::AnnounceResult::Deferred:
            strcpy(g_announceStr, "deferred");
            Serial.println("[ANN] deferred");
            break;
        default:
            strcpy(g_announceStr, "failed");
            Serial.println("[ANN] failed");
            break;
    }
}

void doSendTest(const char* body) {
    if (!g_protoReady) {
        strcpy(g_txStr, "proto off");
        Serial.println("[TX] proto off");
        return;
    }
    if (g_msgRowCount == 0 || !g_msgRows[0].used) {
        strcpy(g_txStr, "no peer");
        Serial.println("[TX] no peer");
        return;
    }
    static unsigned long lastTx = 0;
    if (lastTx && millis() - lastTx < 3000UL) {
        strcpy(g_txStr, "wait 3s");
        Serial.println("[TX] rate limited");
        return;
    }

    if (!body || !body[0]) body = "ping from tdeck-pro";
    char local[161];
    size_t n = strlen(body);
    if (n >= sizeof(local)) n = sizeof(local) - 1;
    memcpy(local, body, n);
    local[n] = '\0';
    while (n > 0 && (local[n - 1] == '\r' || local[n - 1] == '\n' || local[n - 1] == ' '))
        local[--n] = '\0';
    if (n == 0) {
        strcpy(local, "ping from tdeck-pro");
        n = strlen(local);
    }

    const uint8_t* dest = g_msgRows[0].peer;
    char hex[33];
    peerToHex32(dest, hex);
    Serial.printf("[TX] to %s body=\"%s\" free_int=%u\n", hex, local,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    auto sub = g_proto.lxmfSubmit(dest, nullptr, 0,
                                  (const uint8_t*)local, n, false);
    if (!sub.accepted()) {
        snprintf(g_txStr, sizeof(g_txStr), "rej %u", (unsigned)sub.rejection);
        Serial.printf("[TX] rejected %u\n", (unsigned)sub.rejection);
        return;
    }
    lastTx = millis();
    strcpy(g_txStr, "pending");

    for (int i = 0; i < 800; i++) {
        g_proto.loop();
        if (g_storeReady) g_msgStore.poll();
        handheld::outgoing::InitialResult ir{};
        auto p = g_proto.lxmfPoll(sub.ticket, ir);
        if (p == handheld::outgoing::Poll::Pending) {
            delay(10);
            continue;
        }
        if (p == handheld::outgoing::Poll::Ready) {
            Serial.printf("[TX] outcome=%u err=%u suppressed=%d\n",
                          (unsigned)ir.outcome, (unsigned)ir.error,
                          (int)ir.txSuppressed);
            g_proto.lxmfAcknowledge(sub.ticket);
            if (ir.outcome == handheld::storage::Outcome::Committed)
                strcpy(g_txStr, ir.txSuppressed ? "saved" : "sent");
            else
                snprintf(g_txStr, sizeof(g_txStr), "fail %u", (unsigned)ir.error);
            return;
        }
        strcpy(g_txStr, "invalid");
        Serial.println("[TX] poll invalid");
        return;
    }
    strcpy(g_txStr, "timeout");
    Serial.println("[TX] timeout");
}

bool waitResult(MessageStore::Ticket ticket, MessageStore::Result& res, int maxSpin = 600) {
    for (int spin = 0; spin < maxSpin; spin++) {
        g_msgStore.poll();
        if (g_msgStore.peekResult(ticket, res)) return true;
        delay(5);
    }
    return false;
}

void peerToHex32(const uint8_t peer[16], char out[33]) {
    for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", peer[i]);
    out[32] = '\0';
}

std::string resolvePeerName(const uint8_t peer[16]) {
    char hex[33];
    peerToHex32(peer, hex);
    std::string name = g_announceMgr.lookupName(hex);
    if (!name.empty()) return name;
    const DiscoveredNode* n = g_announceMgr.findNode(rs::Bytes(peer, 16));
    if (n && !n->name.empty()) return n->name;
    n = g_announceMgr.findNodeByHex(hex);
    if (n && !n->name.empty()) return n->name;
    for (const auto& node : g_announceMgr.nodes()) {
        if (node.hash.size() == 16 && memcmp(node.hash.data(), peer, 16) == 0 &&
            !node.name.empty())
            return node.name;
    }
    return {};
}

void formatPeerLine(char* out, size_t outLen, const uint8_t peer[16], uint32_t unread) {
    std::string name = resolvePeerName(peer);
    if (!name.empty()) {
        if (unread > 0)
            snprintf(out, outLen, "%.18s *%u", name.c_str(), (unsigned)unread);
        else
            snprintf(out, outLen, "%.20s", name.c_str());
        return;
    }
    if (unread > 0)
        snprintf(out, outLen, "%02x%02x%02x%02x%02x%02x%02x%02x *%u",
                 peer[0], peer[1], peer[2], peer[3],
                 peer[4], peer[5], peer[6], peer[7], (unsigned)unread);
    else
        snprintf(out, outLen, "%02x%02x%02x%02x%02x%02x%02x%02x",
                 peer[0], peer[1], peer[2], peer[3],
                 peer[4], peer[5], peer[6], peer[7]);
}

void applyView(const handheld::storage::ConversationView& v) {
    if (g_msgRowCount >= MSG_ROWS) return;
    MsgRow& r = g_msgRows[g_msgRowCount];
    r.used = true;
    memcpy(r.peer, v.peer, 16);
    r.unread = v.unreadCount;
    formatPeerLine(r.line1, sizeof(r.line1), r.peer, r.unread);
    size_t pl = v.previewLength;
    if (pl >= sizeof(r.line2)) pl = sizeof(r.line2) - 1;
    memcpy(r.line2, v.preview, pl);
    r.line2[pl] = '\0';
    for (size_t k = 0; k < pl; k++) {
        if ((unsigned char)r.line2[k] < 32 || (unsigned char)r.line2[k] > 126)
            r.line2[k] = '?';
    }
    char hex[33];
    peerToHex32(r.peer, hex);
    Serial.printf("[MSG] row peer=%s name=\"%s\" unread=%u\n",
                  hex, resolvePeerName(r.peer).c_str(), (unsigned)r.unread);
    g_msgRowCount++;
}

void applySelectorOnly(const handheld::storage::ConversationSelector& s) {
    if (g_msgRowCount >= MSG_ROWS) return;
    MsgRow& r = g_msgRows[g_msgRowCount];
    r.used = true;
    memcpy(r.peer, s.cursor.peer, 16);
    r.unread = 0;
    formatPeerLine(r.line1, sizeof(r.line1), r.peer, 0);
    r.line2[0] = '\0';
    g_msgRowCount++;
}

bool tryConversationPage(uint8_t limit) {
    using namespace handheld::storage;
    auto sub = g_msgStore.requestConversationPage(
        {}, false, ConversationOrder::Recent, ConversationDirection::After, limit);
    if (!sub.accepted()) {
        Serial.printf("[MSG] page rejected rejection=%u limit=%u\n",
                      (unsigned)sub.rejection, (unsigned)limit);
        return false;
    }
    Result res;
    if (!waitResult(sub.ticket, res)) {
        Serial.println("[MSG] page timeout");
        g_msgStore.releaseResult(sub.ticket);
        return false;
    }
    Serial.printf("[MSG] page outcome=%u err=%u len=%u total=%u limit=%u\n",
                  (unsigned)res.outcome, (unsigned)res.error,
                  (unsigned)res.length, (unsigned)res.total, (unsigned)limit);

    ConversationSelector selectors[MSG_ROWS];
    size_t nSel = 0;
    if (res.outcome == Outcome::Committed && res.error == Error::None &&
        res.length >= sizeof(ConversationSelector)) {
        nSel = res.length / sizeof(ConversationSelector);
        if (nSel > (size_t)MSG_ROWS) nSel = (size_t)MSG_ROWS;
        if (!g_msgStore.readPayload(sub.ticket, selectors,
                                    nSel * sizeof(ConversationSelector)))
            nSel = 0;
    }
    g_msgStore.releaseResult(sub.ticket);

    for (size_t i = 0; i < nSel && g_msgRowCount < MSG_ROWS; i++) {
        const ConversationSelector& sel = selectors[i];
        if (!sel.counter || sel.error != Error::None) {
            applySelectorOnly(sel);
            continue;
        }
        auto dsub = g_msgStore.requestConversation(sel);
        if (!dsub.accepted()) {
            applySelectorOnly(sel);
            continue;
        }
        Result dres;
        if (!waitResult(dsub.ticket, dres)) {
            g_msgStore.releaseResult(dsub.ticket);
            applySelectorOnly(sel);
            continue;
        }
        ConversationView view;
        bool got = (dres.outcome == Outcome::Committed &&
                    dres.error == Error::None &&
                    dres.length >= sizeof(ConversationView) &&
                    g_msgStore.readPayload(dsub.ticket, &view, sizeof(view)));
        g_msgStore.releaseResult(dsub.ticket);
        if (got) applyView(view);
        else applySelectorOnly(sel);
    }
    return g_msgRowCount > 0;
}

void loadHistoryRows() {
    g_histRowCount = 0;
    g_histNote[0] = '\0';
    for (int i = 0; i < HIST_ROWS; i++) g_histRows[i].used = false;

    if (!g_storeReady || g_msgRowCount == 0 || !g_msgRows[0].used) {
        snprintf(g_histNote, sizeof(g_histNote), "(no peer)");
        return;
    }

    char hex[33];
    peerToHex32(g_msgRows[0].peer, hex);
    std::string peerHex(hex);
    Serial.printf("[HIST] load peer=%s free_int=%u\n", hex,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    using namespace handheld::storage;
    auto sub = g_msgStore.requestHistoryPage(
        peerHex, HistoryEntry{}, (uint8_t)HIST_ROWS, HistoryDirection::Before);
    if (!sub.accepted()) {
        Serial.printf("[HIST] page rejected %u\n", (unsigned)sub.rejection);
        snprintf(g_histNote, sizeof(g_histNote), "(hist rejected)");
        return;
    }

    Result res;
    if (!waitResult(sub.ticket, res)) {
        g_msgStore.releaseResult(sub.ticket);
        snprintf(g_histNote, sizeof(g_histNote), "(hist timeout)");
        return;
    }
    Serial.printf("[HIST] page outcome=%u err=%u len=%u total=%u\n",
                  (unsigned)res.outcome, (unsigned)res.error,
                  (unsigned)res.length, (unsigned)res.total);

    HistoryEntry entries[HIST_ROWS];
    size_t nEnt = 0;
    if (res.outcome == Outcome::Committed && res.error == Error::None &&
        res.length >= sizeof(HistoryEntry)) {
        nEnt = res.length / sizeof(HistoryEntry);
        if (nEnt > (size_t)HIST_ROWS) nEnt = (size_t)HIST_ROWS;
        if (!g_msgStore.readPayload(sub.ticket, entries, nEnt * sizeof(HistoryEntry)))
            nEnt = 0;
    }
    g_msgStore.releaseResult(sub.ticket);

    for (size_t i = 0; i < nEnt && g_histRowCount < HIST_ROWS; i++) {
        RecordKey key{};
        memcpy(key.peer, g_msgRows[0].peer, 16);
        key.counter = entries[i].counter;
        key.incoming = entries[i].incoming;

        auto rsub = g_msgStore.requestRecord(key, 0, 96);
        if (!rsub.accepted()) {
            HistRow& h = g_histRows[g_histRowCount++];
            h.used = true;
            snprintf(h.line, sizeof(h.line), "%s#%u",
                     entries[i].incoming ? "<" : ">", (unsigned)entries[i].counter);
            continue;
        }
        Result rres;
        if (!waitResult(rsub.ticket, rres) ||
            rres.outcome != Outcome::Committed || rres.error != Error::None) {
            g_msgStore.releaseResult(rsub.ticket);
            HistRow& h = g_histRows[g_histRowCount++];
            h.used = true;
            snprintf(h.line, sizeof(h.line), "%s#%u",
                     entries[i].incoming ? "<" : ">", (unsigned)entries[i].counter);
            continue;
        }

        uint8_t buf[160];
        size_t want = rres.length < sizeof(buf) ? rres.length : sizeof(buf);
        bool ok = g_msgStore.readPayload(rsub.ticket, buf, want);
        g_msgStore.releaseResult(rsub.ticket);

        HistRow& h = g_histRows[g_histRowCount++];
        h.used = true;
        const char* arrow = entries[i].incoming ? "<" : ">";
        if (!ok || want < sizeof(StoredRecordHeader)) {
            snprintf(h.line, sizeof(h.line), "%s#%u", arrow, (unsigned)entries[i].counter);
            continue;
        }
        StoredRecordHeader hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        size_t off = sizeof(StoredRecordHeader);
        if (off + hdr.titleLength <= want) off += hdr.titleLength;
        size_t clen = hdr.contentLength;
        if (off + clen > want) clen = want > off ? want - off : 0;
        char snip[20];
        size_t copy = clen < sizeof(snip) - 1 ? clen : sizeof(snip) - 1;
        memcpy(snip, buf + off, copy);
        snip[copy] = '\0';
        for (size_t k = 0; k < copy; k++) {
            if ((unsigned char)snip[k] < 32 || (unsigned char)snip[k] > 126)
                snip[k] = '?';
        }
        if (copy)
            snprintf(h.line, sizeof(h.line), "%s %s", arrow, snip);
        else
            snprintf(h.line, sizeof(h.line), "%s#%u", arrow, (unsigned)entries[i].counter);
    }

    if (g_histRowCount == 0)
        snprintf(g_histNote, sizeof(g_histNote), "(no history)");
    else
        Serial.printf("[HIST] loaded %d rows\n", g_histRowCount);
}

void loadMessageRows(bool force) {
    if (g_storeReady) g_msgStore.poll();
    const uint32_t rev = g_storeReady ? g_msgStore.revision() : 0;

    if (g_msgRowsLoaded && !force && g_msgRowCount > 0 && rev == g_msgCacheRevision) {
        Serial.printf("[MSG] using cached %d rows rev=%u\n", g_msgRowCount, (unsigned)rev);
        return;
    }
    if (g_msgRowsLoaded && rev != g_msgCacheRevision)
        Serial.printf("[MSG] cache stale rev %u -> %u\n",
                      (unsigned)g_msgCacheRevision, (unsigned)rev);

    g_msgRowCount = 0;
    g_msgNote[0] = '\0';
    for (int i = 0; i < MSG_ROWS; i++) g_msgRows[i].used = false;
    if (!g_storeReady) {
        snprintf(g_msgNote, sizeof(g_msgNote), "(no store)");
        return;
    }

    Serial.printf("[MSG] load free_int=%u rev=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)rev);

    if (!tryConversationPage(MSG_ROWS)) tryConversationPage(1);

    if (g_msgRowCount == 0) {
        auto ids = g_msgStore.startupRecentMessageIds(MSG_ROWS);
        for (size_t i = 0; i < ids.size() && g_msgRowCount < MSG_ROWS; i++) {
            MsgRow& r = g_msgRows[g_msgRowCount];
            r.used = true;
            memset(r.peer, 0, 16);
            r.unread = 0;
            snprintf(r.line1, sizeof(r.line1), "msg %.12s", ids[i].c_str());
            r.line2[0] = '\0';
            g_msgRowCount++;
        }
    }

    if (g_msgRowCount == 0) {
        uint32_t c = g_msgStore.totalConversations();
        int u = g_msgStore.totalUnreadCount();
        if (c > 0)
            snprintf(g_msgNote, sizeof(g_msgNote), "(%u stored, list empty)", (unsigned)c);
        else if (u > 0)
            snprintf(g_msgNote, sizeof(g_msgNote), "(%d unread, list empty)", u);
        else
            snprintf(g_msgNote, sizeof(g_msgNote), "(no conversations)");
        g_msgRowsLoaded = false;
    } else {
        g_msgRowsLoaded = true;
        g_msgCacheRevision = g_msgStore.revision();
        Serial.printf("[MSG] loaded %d rows rev=%u\n",
                      g_msgRowCount, (unsigned)g_msgCacheRevision);
    }
}

void refreshLiveData() {
    float v = 0; int pct = -1;
    if (readBattery(v, pct)) {
        snprintf(g_batStr, sizeof(g_batStr), "%.2f V", v);
        snprintf(g_pctStr, sizeof(g_pctStr), "%d%%", pct);
    } else {
        strcpy(g_batStr, "n/a"); strcpy(g_pctStr, "--%");
    }
    if (g_loraIf && g_loraIf->isOnline()) {
        strcpy(g_loraStr, "online");
        int rssi = g_loraIf->lastRxRssi();
        if (rssi == 0 && g_radio) rssi = g_radio->currentRssi();
        snprintf(g_rssiStr, sizeof(g_rssiStr), "%d dBm", rssi);
        float snr = g_loraIf->lastRxSnr();
        if (snr != 0.0f) snprintf(g_snrStr, sizeof(g_snrStr), "%.1f dB", snr);
        else strcpy(g_snrStr, "-");
    } else if (g_radio && g_radio->isRadioOnline()) {
        strcpy(g_loraStr, "online");
        snprintf(g_rssiStr, sizeof(g_rssiStr), "%d dBm", g_radio->currentRssi());
        strcpy(g_snrStr, "-");
    } else {
        strcpy(g_loraStr, "off"); strcpy(g_rssiStr, "-"); strcpy(g_snrStr, "-");
    }
    if (g_protoReady) {
        snprintf(g_pathsStr, sizeof(g_pathsStr), "%u", (unsigned)g_proto.pathCount());
        snprintf(g_linksStr, sizeof(g_linksStr), "%u", (unsigned)g_proto.linkCount());
        snprintf(g_lxmfqStr, sizeof(g_lxmfqStr), "%d", g_proto.lxmfQueuedCount());
    }
    if (g_storeReady) {
        g_msgStore.poll();
        snprintf(g_convStr, sizeof(g_convStr), "%u conv",
                 (unsigned)g_msgStore.totalConversations());
        snprintf(g_unreadStr, sizeof(g_unreadStr), "%d unread",
                 g_msgStore.totalUnreadCount());
    }
}

void fat_label(lv_obj_t* parent, const char* txt, lv_coord_t x, lv_coord_t y) {
    lv_obj_t* o = lv_label_create(parent);
    lv_label_set_text(o, txt);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(o, lv_color_black(), 0);
    lv_obj_set_pos(o, x, y);
}

void clear_screen() {
    g_display.fillScreen(false);
    if (g_root) lv_obj_clean(g_root);
    else {
        g_root = lv_scr_act();
        lv_obj_set_style_bg_color(g_root, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(g_root, 0, 0);
        lv_obj_clear_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    }
}

void make_header(const char* title) {
    fat_label(g_root, title, 8, 6);
    lv_obj_t* bar = lv_obj_create(g_root);
    lv_obj_set_size(bar, EPD_WIDTH, 3);
    lv_obj_set_pos(bar, 0, 30);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
}

void make_footer(const char* text) {
    fat_label(g_root, text, 8, EPD_HEIGHT - 22);
}

void add_row(int& y, const char* left, const char* right) {
    fat_label(g_root, left, 8, y);
    fat_label(g_root, right, 110, y);
    y += 28;
}

void build_home() {
    clear_screen();
    make_header("Ratspeak");
    int y = 40;
    char batLine[28];
    snprintf(batLine, sizeof(batLine), "%s %s", g_batStr, g_pctStr);
    add_row(y, "Battery", batLine);
    add_row(y, "LoRa", g_loraStr);
    add_row(y, "RSSI", g_rssiStr);
    add_row(y, "SNR", g_snrStr);
    y += 6;
    add_row(y, "Paths", g_pathsStr);
    add_row(y, "Links", g_linksStr);
    add_row(y, "LXMF Q", g_lxmfqStr);
    y += 10;
    fat_label(g_root, "LOCAL_DEST", 8, y);
    y += 26;
    fat_label(g_root, g_destHex, 8, y);
    make_footer("1/3 Home  Enter/touch next");
}

void build_messages() {
    for (int i = 0; i < g_msgRowCount; i++) {
        if (g_msgRows[i].used)
            formatPeerLine(g_msgRows[i].line1, sizeof(g_msgRows[i].line1),
                           g_msgRows[i].peer, g_msgRows[i].unread);
    }
    clear_screen();
    if (g_showHistory) {
        make_header("History");
        int y = 40;
        if (g_msgRowCount > 0 && g_msgRows[0].used) {
            fat_label(g_root, g_msgRows[0].line1, 8, y);
            y += 26;
        }
        if (g_histRowCount == 0) {
            fat_label(g_root, g_histNote[0] ? g_histNote : "(empty)", 8, y);
        } else {
            for (int i = 0; i < g_histRowCount; i++) {
                fat_label(g_root, g_histRows[i].line, 8, y);
                y += 26;
                if (y > EPD_HEIGHT - 40) break;
            }
        }
        make_footer("H=list  t=send");
        return;
    }
    make_header("Messages");
    int y = 40;
    add_row(y, "Convs", g_convStr);
    add_row(y, "Unread", g_unreadStr);
    y += 8;
    if (g_msgRowCount == 0) {
        fat_label(g_root, g_msgNote[0] ? g_msgNote : "(empty)", 8, y);
    } else {
        for (int i = 0; i < g_msgRowCount; i++) {
            fat_label(g_root, g_msgRows[i].line1, 8, y);
            y += 22;
            if (g_msgRows[i].line2[0]) {
                fat_label(g_root, g_msgRows[i].line2, 8, y);
                y += 24;
            } else {
                y += 4;
            }
            if (y > EPD_HEIGHT - 40) break;
        }
    }
    make_footer("2/3 Msgs  H=history");
}

void build_settings() {
    clear_screen();
    make_header("Settings");
    int y = 40;
    char freq[20], txp[12];
    snprintf(freq, sizeof(freq), "%.1f MHz", LORA_DEFAULT_FREQ / 1e6f);
    snprintf(txp, sizeof(txp), "%d dBm", LORA_DEFAULT_TX_POWER);
    add_row(y, "Radio", "Long Fast");
    add_row(y, "Freq", freq);
    add_row(y, "TX power", txp);
    add_row(y, "Announce", g_announceStr);
    add_row(y, "Last TX", g_txStr);
    y += 8;
    fat_label(g_root, g_protoReady ? "(protocol up)" : "(protocol off)", 8, y);
    y += 24;
    fat_label(g_root, "a=ann  t=tx text", 8, y);
    y += 24;
    char ps[40];
    snprintf(ps, sizeof(ps), "PSRAM %uK", (unsigned)(ESP.getPsramSize() / 1024));
    fat_label(g_root, ps, 8, y);
    make_footer("3/3 Setup Enter/touch next");
}

void show_screen(ScreenId id) {
    g_screen = id;
    refreshLiveData();
    if (id == SCR_MESSAGES) loadMessageRows(false);
    switch (id) {
        case SCR_HOME: build_home(); break;
        case SCR_MESSAGES: build_messages(); break;
        case SCR_SETTINGS: build_settings(); break;
        default: build_home(); break;
    }
    lv_refr_now(nullptr);
    g_display.refreshIfDirty();
    if (g_storeReady) g_lastUiRevision = g_msgStore.revision();
    Serial.printf("[UI] screen %u drawn\n", (unsigned)id);
}

void next_screen() {
    show_screen((ScreenId)((g_screen + 1) % SCR_COUNT));
}

void maybeAutoRedrawMessages() {
    if (g_screen != SCR_MESSAGES || !g_storeReady) return;
    g_msgStore.poll();
    const uint32_t rev = g_msgStore.revision();
    if (rev == g_lastUiRevision) return;
    if (millis() - g_lastMsgRedrawMs < MSG_REDRAW_MIN_MS) return;
    g_lastMsgRedrawMs = millis();
    Serial.printf("[UI] auto Messages redraw rev %u -> %u\n",
                  (unsigned)g_lastUiRevision, (unsigned)rev);
    show_screen(SCR_MESSAGES);
}

}  // namespace

bool begin() {
    Serial.println();
    Serial.println("========================================");
    Serial.println(" RATSPEAK  T-Deck Pro  Phase E chunk 17");
    Serial.println(" history: serial H");
    Serial.println("========================================");

    handheld::bindDeviceOwner();

    pinMode(BOARD_1V8_EN, OUTPUT);
    pinMode(BOARD_LORA_EN, OUTPUT);
    digitalWrite(BOARD_1V8_EN, HIGH);
    digitalWrite(BOARD_LORA_EN, HIGH);
    delay(30);

    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW); delay(10);
    digitalWrite(TOUCH_RST, HIGH); delay(50);
    pinMode(TOUCH_INT, INPUT_PULLUP);

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(I2C_FREQUENCY);

    spiBusIdle();
    logPsram();

    if (!initRadio()) Serial.println("[BOOT] Radio failed");
    initStorageAndProto();
    if (g_destHex[0] == '.') fillDestFromNvs();
    logPsram();

    if (g_storeReady) {
        Serial.printf("[MSG] preload free_int=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        loadMessageRows(true);
    }

    spiBusIdle();
    Serial.printf("[BOOT] before display free_int=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (!g_display.begin()) {
        Serial.println("[BOOT] Display FAILED");
        return false;
    }
    Serial.println("[BOOT] display OK");
    if (!LvglPort::begin(g_display)) {
        Serial.println("[BOOT] LVGL FAILED");
        return false;
    }
    Serial.println("[BOOT] LVGL OK");

    g_kbOk = g_kb.begin();
    if (g_kbOk) Serial.println("[KEYBOARD] TCA8418 keyboard ready");

    show_screen(SCR_HOME);
    Serial.println("[BOOT] ready");
    g_bootAnnAt = millis() + 3000;
    Serial.println("[HINT] t | t hello | H=history | a | r | n/m/h/s");
    return true;
}

void loop() {
    if (g_protoReady) g_proto.loop();
    if (g_storeReady) g_msgStore.poll();
    g_announceMgr.loop();

    if (!g_bootAnnDone && g_bootAnnAt != 0 && millis() >= g_bootAnnAt) {
        g_bootAnnDone = true;
        Serial.println("[ANN] boot announce");
        doAnnounce();
    }

    maybeAutoRedrawMessages();

    if (g_kbOk) {
        g_kb.update();
        if (g_kb.hasEvent()) {
            const KeyEvent& ev = g_kb.getEvent();
            if (ev.enter) {
                g_kb.discardPending();
                next_screen();
            } else if (ev.character == 'a' || ev.character == 'A') {
                g_kb.discardPending();
                doAnnounce();
                if (g_screen == SCR_SETTINGS) show_screen(SCR_SETTINGS);
            } else if (ev.character == 't' || ev.character == 'T') {
                g_kb.discardPending();
                doSendTest(nullptr);
                if (g_screen == SCR_SETTINGS || g_screen == SCR_MESSAGES)
                    show_screen(g_screen);
            } else {
                g_kb.discardPending();
            }
        }
    }

    static int lastInt = HIGH;
    static uint32_t lastTouchMs = 0;
    int tInt = digitalRead(TOUCH_INT);
    if (lastInt == HIGH && tInt == LOW && (millis() - lastTouchMs > 400)) {
        lastTouchMs = millis();
        Serial.println("[TOUCH] INT next");
        next_screen();
    }
    lastInt = tInt;

    static char line[180];
    static size_t lineLen = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            if (lineLen == 0) continue;
            line[lineLen] = '\0';
            lineLen = 0;
            char* p = line;
            while (*p == ' ') p++;
            if (p[0] == 't' || p[0] == 'T') {
                const char* body = nullptr;
                if (p[1] == ':' || p[1] == ' ') {
                    body = p + 2;
                    while (*body == ' ') body++;
                    if (!*body) body = nullptr;
                }
                doSendTest(body);
                if (g_screen == SCR_SETTINGS || g_screen == SCR_MESSAGES)
                    show_screen(g_screen);
            } else if (p[0] == 'a' || p[0] == 'A') {
                doAnnounce();
                if (g_screen == SCR_SETTINGS) show_screen(SCR_SETTINGS);
            } else if (p[0] == 'r' || p[0] == 'R') {
                loadMessageRows(true);
                show_screen(SCR_MESSAGES);
            } else if (p[0] == 'H') {
                g_showHistory = !g_showHistory;
                if (g_showHistory) loadHistoryRows();
                show_screen(SCR_MESSAGES);
            } else if (p[0] == 'n' || p[0] == 'N' || p[0] == '>') {
                g_showHistory = false;
                next_screen();
            } else if (p[0] == 'h' || p[0] == '1') {
                g_showHistory = false;
                show_screen(SCR_HOME);
            } else if (p[0] == 'm' || p[0] == 'M' || p[0] == '2') {
                g_showHistory = false;
                show_screen(SCR_MESSAGES);
            } else if (p[0] == 's' || p[0] == 'S' || p[0] == '3') {
                g_showHistory = false;
                show_screen(SCR_SETTINGS);
            } else {
                Serial.printf("[CMD] unknown \"%s\" (t/a/r/n/m/h/s/H)\n", p);
            }
        } else if (c >= 32 && c < 127) {
            if (lineLen + 1 < sizeof(line)) line[lineLen++] = c;
        }
    }
    delay(5);
}

}  // namespace standalone