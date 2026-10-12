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
#include "audio/AudioNotify.h"
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
bool g_touchOk = false;  // E22: CST328 present
AudioNotify g_audio;
bool g_featAudio = true;   // beep+vibe on inbound when true
bool g_featWifi  = false;
bool g_featTcp   = false;
bool g_featGps   = false;
bool g_featLora  = true;   // display/radio intent (radio stays up for mesh)
uint32_t g_lastPeerAutoMs = 0;
size_t   g_lastPeerCount  = 0;
bool     g_peerDirty      = false;
bool     g_composeMode    = false;  // true while typing a message

FlashStore g_flash;
IdentityManager g_idMgr;
MessageStore g_msgStore;
AnnounceManager g_announceMgr;
ProtocolRuntime g_proto;
bool g_storeReady = false;
bool g_protoReady = false;

SX1262* g_radio = nullptr;
LoRaInterface* g_loraIf = nullptr;

enum ScreenId : uint8_t {
    SCR_HOME = 0,
    SCR_CHATS,      // Plus: Chats (conversations + compose + history)
    SCR_CONTACTS,   // Plus: Contacts (messaged / named peers)
    SCR_PEERS,      // Plus: Peers (announced nodes)
    SCR_SETTINGS,
    SCR_COUNT
};
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
int g_selPeer = 0;  // E20: selected MsgRow for TX / History / mark-read
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

// E19: keyboard compose buffer (no per-key e-ink refresh).
constexpr size_t COMPOSE_CAP = 120;
char g_compose[COMPOSE_CAP + 1] = "";
size_t g_composeLen = 0;
bool g_composeDirty = false;
uint32_t g_lastComposeMs = 0;
constexpr uint32_t COMPOSE_IDLE_MS = 1600;  // E21: redraw glass after typing pauses

bool g_bootAnnDone = false;
uint32_t g_bootAnnAt = 0;

uint32_t g_lastUiRevision = 0;
uint32_t g_lastMsgRedrawMs = 0;
constexpr uint32_t MSG_REDRAW_MIN_MS = 2500;

void peerToHex32(const uint8_t peer[16], char out[33]);
void show_screen(ScreenId id);
void next_screen();
const char* tabLabel(ScreenId id);
void draw_nav();
void draw_nav_box(int x, int y, int w, int h, const char* label, bool active, char hotkey = 0);
void draw_compose_or_hint(const char* idleHint);
void build_chats();
void build_contacts();
void build_peers();
void doAnnounce();
void doSendTest(const char* body);
void loadMessageRows(bool force);
void appendAnnouncedPeers();  // E23: fill empty slots from AnnounceManager
void loadHistoryRows();
void composeClear();
void composeAppend(char c);
void composeBackspace();
void composeLog();
void clampSelPeer();
bool cycleSelPeer();
bool cst328Read(int16_t& x, int16_t& y);  // E22
bool handleTouchAt(int16_t x, int16_t y);  // true if already drew

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
    clampSelPeer();
    if (g_msgRowCount == 0 || !g_msgRows[g_selPeer].used) {
        strcpy(g_txStr, "no peer");
        Serial.println("[TX] no peer — open Peers/Msg Box, press 1-9 or tap a row first");
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

    const uint8_t* dest = g_msgRows[g_selPeer].peer;
    char hex[33];
    peerToHex32(dest, hex);
    size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    Serial.printf("[TX] to %s body=\"%s\" free_int=%u\n", hex, local,
                  (unsigned)freeInt);

    // Rejection 8 seen under low internal heap (~12K) — wait and retry once
    if (freeInt < 16000) {
        Serial.printf("[TX] low heap %u — pause before submit\n", (unsigned)freeInt);
        delay(200);
        freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        Serial.printf("[TX] heap after pause %u\n", (unsigned)freeInt);
    }

    auto sub = g_proto.lxmfSubmit(dest, nullptr, 0,
                                  (const uint8_t*)local, n, false);
    if (!sub.accepted()) {
        unsigned r = (unsigned)sub.rejection;
        snprintf(g_txStr, sizeof(g_txStr), "rej %u", r);
        // Best-effort labels (protocol enum may vary)
        const char* why = "?";
        if (r == 8) why = "resource/OOM";
        else if (r == 1) why = "not-ready";
        else if (r == 7) why = "no-path?";
        Serial.printf("[TX] rejected %u (%s) free_int=%u\n",
                      r, why, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        if (r == 8) {
            delay(300);
            sub = g_proto.lxmfSubmit(dest, nullptr, 0,
                                     (const uint8_t*)local, n, false);
            if (sub.accepted()) {
                Serial.println("[TX] retry accepted after OOM pause");
            } else {
                Serial.printf("[TX] retry still rejected %u — reboot may free heap\n",
                              (unsigned)sub.rejection);
                return;
            }
        } else {
            return;
        }
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
            if (ir.outcome == handheld::storage::Outcome::Committed) {
                strcpy(g_txStr, ir.txSuppressed ? "saved" : "sent");
                g_msgRowsLoaded = false;
                if (g_featAudio) g_audio.playMessage();
            } else {
                snprintf(g_txStr, sizeof(g_txStr), "fail %u", (unsigned)ir.error);
            }
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
    // Prefer announce / cache display name (Plus-style); short hash only if unnamed
    std::string name = resolvePeerName(peer);
    if (!name.empty()) {
        if (unread > 0)
            snprintf(out, outLen, "%.16s *%u", name.c_str(), (unsigned)unread);
        else
            snprintf(out, outLen, "%.18s", name.c_str());
        return;
    }
    if (unread > 0)
        snprintf(out, outLen, "%02x%02x%02x%02x… *%u",
                 peer[0], peer[1], peer[2], peer[3], (unsigned)unread);
    else
        snprintf(out, outLen, "%02x%02x%02x%02x…",
                 peer[0], peer[1], peer[2], peer[3]);
}

void formatPeerSub(char* out, size_t outLen, const uint8_t peer[16], const char* preview) {
    if (preview && preview[0]) {
        snprintf(out, outLen, "%.24s", preview);
        return;
    }
    char hex[33];
    peerToHex32(peer, hex);
    snprintf(out, outLen, "ID: %.16s", hex);
}

void notifyInbound() {
    if (!g_featAudio) return;
    g_audio.requestMessage();
#if defined(BOARD_MOTOR_PIN) && (BOARD_MOTOR_PIN >= 0)
    pinMode(BOARD_MOTOR_PIN, OUTPUT);
    digitalWrite(BOARD_MOTOR_PIN, HIGH);
    delay(40);
    digitalWrite(BOARD_MOTOR_PIN, LOW);
#endif
}

void composeClear() {
    g_compose[0] = '\0';
    g_composeLen = 0;
    g_composeDirty = false;
    g_lastComposeMs = 0;
}

void composeLog() {
    Serial.printf("[COMPOSE] len=%u \"%s\"\n",
                  (unsigned)g_composeLen, g_compose);
}

void composeAppend(char c) {
    if (c < 32 || c > 126) return;
    g_composeMode = true;
    if (g_composeLen >= COMPOSE_CAP) {
        Serial.println("[COMPOSE] full");
        return;
    }
    g_compose[g_composeLen++] = c;
    g_compose[g_composeLen] = '\0';
    g_composeDirty = true;
    g_lastComposeMs = millis();
    composeLog();
}

void composeBackspace() {
    if (g_composeLen == 0) return;
    g_compose[--g_composeLen] = '\0';
    g_composeDirty = true;
    g_lastComposeMs = millis();
    composeLog();
}

void clampSelPeer() {
    if (g_msgRowCount <= 0) {
        g_selPeer = 0;
        return;
    }
    if (g_selPeer < 0) g_selPeer = 0;
    if (g_selPeer >= g_msgRowCount) g_selPeer = g_msgRowCount - 1;
}

bool cycleSelPeer() {
    if (g_msgRowCount <= 0) {
        Serial.println("[PEER] no peers");
        return false;
    }
    g_selPeer = (g_selPeer + 1) % g_msgRowCount;
    clampSelPeer();
    char hex[33];
    peerToHex32(g_msgRows[g_selPeer].peer, hex);
    Serial.printf("[PEER] sel=%d/%d %s\n", g_selPeer, g_msgRowCount, hex);
    return true;
}

// Minimal CST328 (addr 0x1A) single-finger read. No SensorLib dependency.
// Register window starts at 0xD000; finger-1 state at first 5 bytes.
bool cst328Read(int16_t& x, int16_t& y) {
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    Wire.write((uint8_t)0xD0);
    Wire.write((uint8_t)0x00);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)TOUCH_I2C_ADDR, 7) < 5) return false;
    uint8_t d0 = Wire.read();
    uint8_t d1 = Wire.read();
    uint8_t d2 = Wire.read();
    uint8_t d3 = Wire.read();
    (void)Wire.read();  // pressure
    while (Wire.available()) (void)Wire.read();
    if ((d0 & 0x0F) != 0x06) return false;  // pressed state
    x = (int16_t)((d1 << 4) | ((d3 >> 4) & 0x0F));
    y = (int16_t)((d2 << 4) | (d3 & 0x0F));
    return true;
}

// ---------------------------------------------------------------------------
// Layout (240x320 portrait) — two-row nav, boxed tabs, no page title
//
//   TOP_NAV  y=0 .. TOP_H-1     [ Home ] [ Settings ]   (2 boxes)
//   Content  y=CONTENT_TOP .. CONTENT_BOTTOM
//   Compose  just above BOT_NAV when non-empty
//   BOT_NAV  y=BOT_TOP .. EPD_HEIGHT-1
//            [ Chats ] [ Cont ] [ Peers ]               (3 boxes)
//
// Active tab: black filled box + white label (e-ink invert highlight).
// Touch hit-test must stay in sync with draw_nav().
// ---------------------------------------------------------------------------
constexpr int TOP_H          = 36;
constexpr int BOT_H          = 40;
constexpr int BOT_TOP        = EPD_HEIGHT - BOT_H;          // 280
constexpr int CONTENT_TOP    = TOP_H;                      // 36 — no gap under nav
constexpr int CONTENT_BOTTOM = BOT_TOP;                    // flush to bot nav
constexpr int TOP_TAB_W      = EPD_WIDTH / 2;               // 120
// Bottom: Chats | Contact | Peers
// Bottom order: Contact | Msg Box | Peers
constexpr int BOT_CONTACT_W  = 80;
constexpr int BOT_MSGBOX_W   = 96;
constexpr int BOT_PEERS_W    = 64;   // 80+96+64=240
constexpr int BOT_CHATS_W    = BOT_MSGBOX_W;  // SCR_CHATS = Msg Box
constexpr int PEERS_TOP      = CONTENT_TOP + 28;  // after section header
constexpr int ROW_H          = 42;
// Home grid — zero outer margin, 2px tile gap
constexpr int TILE_GAP       = 2;
constexpr int TILE_W         = (EPD_WIDTH - TILE_GAP * 2) / 3;  // 78
constexpr int TILE_H         = 42;
constexpr int HOME_ID_H      = 56;   // identity card height
constexpr int HOME_GRID_Y    = CONTENT_TOP + HOME_ID_H;    // 92
constexpr int HOME_ANN_H     = 34;

const char* tabLabel(ScreenId id) {
    switch (id) {
        case SCR_HOME:     return "Home";
        case SCR_CHATS:    return "Msg Box";
        case SCR_CONTACTS: return "Contact";
        case SCR_PEERS:    return "Peers";
        case SCR_SETTINGS: return "Settings";
        default:           return "?";
    }
}

bool isTopTab(ScreenId id) {
    return id == SCR_HOME || id == SCR_SETTINGS;
}

// Map top-row index 0..1 -> screen
ScreenId topTabScreen(int i) {
    // Left = Settings, Right = Home (right-thumb Home)
    return (i == 0) ? SCR_SETTINGS : SCR_HOME;
}

// Map bottom-row index 0..2 -> screen
ScreenId botTabScreen(int i) {
    if (i <= 0) return SCR_CHATS;
    if (i == 1) return SCR_CONTACTS;
    return SCR_PEERS;
}

bool handleTouchAt(int16_t x, int16_t y) {
    Serial.printf("[TOUCH] xy=%d,%d screen=%u\n", (int)x, (int)y, (unsigned)g_screen);

    // Top nav: Home | Settings
    if (y < TOP_H) {
        int i = (x < TOP_TAB_W) ? 0 : 1;
        ScreenId want = topTabScreen(i);
        Serial.printf("[TAB] top -> %s\n", tabLabel(want));
        if (want == g_screen) return true;
        g_showHistory = false;
        show_screen(want);
        return true;
    }

    // Bottom nav: Chats | Contact | Peers (unequal widths)
    if (y >= BOT_TOP) {
        ScreenId want = SCR_CONTACTS;
        if (x < BOT_CONTACT_W)
            want = SCR_CONTACTS;
        else if (x < BOT_CONTACT_W + BOT_MSGBOX_W)
            want = SCR_CHATS;   // Msg Box
        else
            want = SCR_PEERS;
        Serial.printf("[TAB] bot -> %s\n", tabLabel(want));
        if (want == g_screen) return true;
        if (want != SCR_CHATS) g_showHistory = false;
        show_screen(want);
        return true;
    }

    // History: any content tap returns to Chats list
    if (g_screen == SCR_CHATS && g_showHistory) {
        g_showHistory = false;
        show_screen(SCR_CHATS);
        return true;
    }

    // List row select
    if ((g_screen == SCR_CHATS || g_screen == SCR_CONTACTS || g_screen == SCR_PEERS) &&
        g_msgRowCount > 0 && y >= PEERS_TOP && y < CONTENT_BOTTOM) {
        int row = (y - PEERS_TOP) / ROW_H;
        if (row < 0) row = 0;
        if (row >= g_msgRowCount) row = g_msgRowCount - 1;
        const int was = g_selPeer;
        g_selPeer = row;
        clampSelPeer();
        char hex[33];
        peerToHex32(g_msgRows[g_selPeer].peer, hex);
        Serial.printf("[PEER] touch sel=%d/%d (was %d) %s\n",
                      g_selPeer, g_msgRowCount, was, hex);
        // Always refresh so 1> selection is visible even if same row
        show_screen(g_screen);
        return true;
    }

    // Home: Announce button
    // Compose Back (top-right above bottom nav)
    if ((g_composeLen > 0 || g_composeMode) &&
        y >= BOT_TOP - 36 && y < BOT_TOP) {
        composeClear();
        g_composeMode = false;
        Serial.println("[COMPOSE] back — leave message mode");
        show_screen(g_screen);
        return true;
    }

    // Home: status tiles toggle (3x2 grid) then Announce
    if (g_screen == SCR_HOME) {
        const int gy = HOME_GRID_Y;
        const int tw = TILE_W;
        const int th = TILE_H;
        const int gap = TILE_GAP;
        if (y >= gy && y < gy + 2 * (th + gap)) {
            int col = x / (tw + gap);
            if (col > 2) col = 2;
            int row = (y - gy) / (th + gap);
            if (row > 1) row = 1;
            int idx = row * 3 + col;
            // 0 LORA 1 TCP 2 WIFI 3 PEERS 4 AUDIO 5 GPS
            if (idx == 0) { g_featLora = !g_featLora; Serial.printf("[FEAT] LORA %s\n", g_featLora?"ON":"OFF"); }
            else if (idx == 1) { g_featTcp = !g_featTcp; Serial.printf("[FEAT] TCP %s\n", g_featTcp?"ON":"OFF"); }
            else if (idx == 2) { g_featWifi = !g_featWifi; Serial.printf("[FEAT] WIFI %s\n", g_featWifi?"ON":"OFF"); }
            else if (idx == 4) {
                g_featAudio = !g_featAudio;
                g_audio.setEnabled(g_featAudio);
                Serial.printf("[FEAT] AUDIO %s\n", g_featAudio?"ON":"OFF");
                if (g_featAudio) notifyInbound();  // sample beep
            }
            else if (idx == 5) { g_featGps = !g_featGps; Serial.printf("[FEAT] GPS %s\n", g_featGps?"ON":"OFF"); }
            // PEERS tile (3) is status only
            show_screen(SCR_HOME);
            return true;
        }
        if (y >= gy + 2 * (th + gap) && y < BOT_TOP - 2) {
            Serial.println("[HOME] touch Announce");
            doAnnounce();
            if (g_featAudio) g_audio.playAnnounce();
            show_screen(SCR_HOME);
            return true;
        }
    }

    Serial.println("[TOUCH] content ignore");
    return true;
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

    clampSelPeer();
    if (!g_storeReady || g_msgRowCount == 0 || !g_msgRows[g_selPeer].used) {
        snprintf(g_histNote, sizeof(g_histNote), "(no peer)");
        return;
    }

    char hex[33];
    peerToHex32(g_msgRows[g_selPeer].peer, hex);
    std::string peerHex(hex);
    Serial.printf("[HIST] load peer=%s free_int=%u\n", hex,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // E20: mark conversation read when opening History for selected peer.
    {
        auto mr = g_msgStore.requestMarkRead(std::string(hex));
        if (mr.accepted()) {
            MessageStore::Result mres;
            if (waitResult(mr.ticket, mres, 200)) {
                Serial.printf("[HIST] markRead outcome=%u err=%u\n",
                              (unsigned)mres.outcome, (unsigned)mres.error);
            }
            g_msgStore.releaseResult(mr.ticket);
            g_msgStore.poll();
        } else {
            Serial.printf("[HIST] markRead rejected %u\n", (unsigned)mr.rejection);
        }
    }

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
        memcpy(key.peer, g_msgRows[g_selPeer].peer, 16);
        key.counter = entries[i].counter;
        key.incoming = entries[i].incoming;

        // Header alone is ~104B; need room for a short title+body snippet.
        constexpr uint16_t kRecCap = 256;
        auto rsub = g_msgStore.requestRecord(key, 0, kRecCap);
        if (!rsub.accepted()) {
            Serial.printf("[HIST] record rej ctr=%u in=%u rej=%u\n",
                          (unsigned)entries[i].counter, (unsigned)entries[i].incoming,
                          (unsigned)rsub.rejection);
            HistRow& h = g_histRows[g_histRowCount++];
            h.used = true;
            snprintf(h.line, sizeof(h.line), "%s#%u",
                     entries[i].incoming ? "<" : ">", (unsigned)entries[i].counter);
            continue;
        }
        Result rres;
        if (!waitResult(rsub.ticket, rres) ||
            rres.outcome != Outcome::Committed || rres.error != Error::None) {
            Serial.printf("[HIST] record fail ctr=%u out=%u err=%u\n",
                          (unsigned)entries[i].counter,
                          (unsigned)rres.outcome, (unsigned)rres.error);
            g_msgStore.releaseResult(rsub.ticket);
            HistRow& h = g_histRows[g_histRowCount++];
            h.used = true;
            snprintf(h.line, sizeof(h.line), "%s#%u",
                     entries[i].incoming ? "<" : ">", (unsigned)entries[i].counter);
            continue;
        }

        uint8_t buf[256];
        size_t want = rres.length < sizeof(buf) ? rres.length : sizeof(buf);
        bool ok = g_msgStore.readPayload(rsub.ticket, buf, want);
        g_msgStore.releaseResult(rsub.ticket);

        HistRow& h = g_histRows[g_histRowCount++];
        h.used = true;
        const char* arrow = entries[i].incoming ? "<" : ">";
        if (!ok || want < sizeof(StoredRecordHeader)) {
            Serial.printf("[HIST] short payload len=%u need=%u\n",
                          (unsigned)want, (unsigned)sizeof(StoredRecordHeader));
            snprintf(h.line, sizeof(h.line), "%s#%u", arrow, (unsigned)entries[i].counter);
            continue;
        }
        StoredRecordHeader hdr;
        memcpy(&hdr, buf, sizeof(hdr));
        size_t off = sizeof(StoredRecordHeader);
        if (off + hdr.titleLength <= want) off += hdr.titleLength;
        else off = want;
        size_t clen = hdr.contentLength;
        if (off + clen > want) clen = want > off ? want - off : 0;
        char snip[22];
        size_t copy = clen < sizeof(snip) - 1 ? clen : sizeof(snip) - 1;
        if (copy) memcpy(snip, buf + off, copy);
        snip[copy] = '\0';
        for (size_t k = 0; k < copy; k++) {
            if ((unsigned char)snip[k] < 32 || (unsigned char)snip[k] > 126)
                snip[k] = '?';
        }
        if (copy)
            snprintf(h.line, sizeof(h.line), "%s %s", arrow, snip);
        else
            snprintf(h.line, sizeof(h.line), "%s#%u", arrow, (unsigned)entries[i].counter);
        Serial.printf("[HIST] %s#%u \"%s\" tlen=%u clen=%u\n",
                      arrow, (unsigned)entries[i].counter, snip,
                      (unsigned)hdr.titleLength, (unsigned)hdr.contentLength);
    }

    if (g_histRowCount == 0)
        snprintf(g_histNote, sizeof(g_histNote), "(no history)");
    else
        Serial.printf("[HIST] loaded %d rows\n", g_histRowCount);
}


// E23: Top up Messages rows from recently announced peers so the user can
// select one and send a keyboard reply even with no prior conversation.
void appendAnnouncedPeers() {
    if (g_msgRowCount >= MSG_ROWS) return;

    const uint8_t* local = nullptr;
    if (g_protoReady) local = g_proto.localDestHash();

    const auto& nodes = g_announceMgr.nodes();
    int added = 0;
    for (const auto& node : nodes) {
        if (g_msgRowCount >= MSG_ROWS) break;
        if (node.hash.size() != 16) continue;
        if (local && memcmp(node.hash.data(), local, 16) == 0) continue;

        bool already = false;
        for (int i = 0; i < g_msgRowCount; i++) {
            if (g_msgRows[i].used &&
                memcmp(g_msgRows[i].peer, node.hash.data(), 16) == 0) {
                already = true;
                break;
            }
        }
        if (already) continue;

        MsgRow& r = g_msgRows[g_msgRowCount];
        r.used = true;
        memcpy(r.peer, node.hash.data(), 16);
        r.unread = 0;
        formatPeerLine(r.line1, sizeof(r.line1), r.peer, 0);
        // Distinguish announce-only peers (no conversation yet).
        if (node.rssi != 0)
            snprintf(r.line2, sizeof(r.line2), "(ann %d dBm)", node.rssi);
        else
            snprintf(r.line2, sizeof(r.line2), "(announced)");
        g_msgRowCount++;
        added++;
        char hex[33];
        peerToHex32(r.peer, hex);
        Serial.printf("[MSG] ann peer=%s name=\"%s\" rssi=%d\n",
                      hex, node.name.empty() ? "-" : node.name.c_str(), node.rssi);
    }
    if (added)
        Serial.printf("[MSG] appended %d announced peer(s) total=%d\n",
                      added, g_msgRowCount);
}

void loadMessageRows(bool force) {
    if (g_storeReady) g_msgStore.poll();
    const uint32_t rev = g_storeReady ? g_msgStore.revision() : 0;

    if (g_msgRowsLoaded && !force && g_msgRowCount > 0 && rev == g_msgCacheRevision) {
        Serial.printf("[MSG] using cached %d rows rev=%u\n", g_msgRowCount, (unsigned)rev);
        // E23: still top up empty slots from newly announced peers
        const int before = g_msgRowCount;
        appendAnnouncedPeers();
        if (g_msgRowCount > before) clampSelPeer();
        return;
    }
    if (g_msgRowsLoaded && rev != g_msgCacheRevision)
        Serial.printf("[MSG] cache stale rev %u -> %u\n",
                      (unsigned)g_msgCacheRevision, (unsigned)rev);

    g_msgRowCount = 0;
    g_msgNote[0] = '\0';
    for (int i = 0; i < MSG_ROWS; i++) g_msgRows[i].used = false;

    Serial.printf("[MSG] load free_int=%u rev=%u store=%d\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)rev, (int)g_storeReady);

    if (g_storeReady) {
        if (!tryConversationPage(MSG_ROWS)) tryConversationPage(1);
    }

    // E23: fill remaining rows from AnnounceManager nodes (keyboard TX targets)
    appendAnnouncedPeers();

    if (g_msgRowCount == 0 && g_storeReady) {
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
        int nAnn = g_announceMgr.nodeCount();
        if (g_storeReady) {
            uint32_t c = g_msgStore.totalConversations();
            int u = g_msgStore.totalUnreadCount();
            if (c > 0)
                snprintf(g_msgNote, sizeof(g_msgNote), "(%u stored, list empty)", (unsigned)c);
            else if (u > 0)
                snprintf(g_msgNote, sizeof(g_msgNote), "(%d unread, list empty)", u);
            else if (nAnn > 0)
                snprintf(g_msgNote, sizeof(g_msgNote), "(%d ann, wait list)", nAnn);
            else
                snprintf(g_msgNote, sizeof(g_msgNote), "(no peers yet)");
        } else if (nAnn > 0) {
            snprintf(g_msgNote, sizeof(g_msgNote), "(%d ann, no store)", nAnn);
        } else {
            snprintf(g_msgNote, sizeof(g_msgNote), "(no peers yet)");
        }
        g_msgRowsLoaded = false;
    } else {
        g_msgRowsLoaded = true;
        g_msgCacheRevision = g_storeReady ? g_msgStore.revision() : 0;
        clampSelPeer();
        Serial.printf("[MSG] loaded %d rows rev=%u sel=%d\n",
                      g_msgRowCount, (unsigned)g_msgCacheRevision, g_selPeer);
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

// Centered label — use LVGL text width when possible for true center
void draw_label_centered(int x, int y, int w, int h, const char* label, bool bold) {
    const lv_font_t* font = &lv_font_montserrat_16;
    lv_coord_t tw = lv_txt_get_width(label, strlen(label), font, 0, LV_TEXT_FLAG_NONE);
    if (tw <= 0) tw = (lv_coord_t)(strlen(label) * 7);
    int lx = x + (w - (int)tw) / 2;
    if (lx < x) lx = x;
    int ly = y + (h - 16) / 2;
    if (ly < y) ly = y;
    for (int k = 0; k < (bold ? 2 : 1); k++) {
        lv_obj_t* lab = lv_label_create(g_root);
        lv_label_set_text(lab, label);
        lv_obj_set_style_text_font(lab, font, 0);
        lv_obj_set_style_text_color(lab, lv_color_black(), 0);
        lv_obj_set_pos(lab, lx + k, ly);
    }
}

void draw_tile(int x, int y, int w, int h, const char* title, const char* value) {
    lv_obj_t* box = lv_obj_create(g_root);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    draw_label_centered(x, y + 1, w, 16, title, false);
    draw_label_centered(x, y + h / 2, w, 18, value, true);
}

void draw_nav_box(int x, int y, int w, int h, const char* label, bool active, char hotkey) {
    lv_obj_t* box = lv_obj_create(g_root);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, active ? 3 : 1, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    if (active) {
        lv_obj_t* under = lv_obj_create(g_root);
        lv_obj_set_size(under, w - 8, 3);
        lv_obj_set_pos(under, x + 4, y + h - 5);
        lv_obj_set_style_radius(under, 0, 0);
        lv_obj_set_style_border_width(under, 0, 0);
        lv_obj_set_style_bg_color(under, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(under, LV_OPA_COVER, 0);
        lv_obj_clear_flag(under, LV_OBJ_FLAG_SCROLLABLE);
    }

    const lv_font_t* font = &lv_font_montserrat_16;
    lv_coord_t full = lv_txt_get_width(label, strlen(label), font, 0, LV_TEXT_FLAG_NONE);
    if (full <= 0) full = (lv_coord_t)(strlen(label) * 7);
    int lx = x + (w - (int)full) / 2;
    if (lx < x + 2) lx = x + 2;
    int ly = y + (h - (active ? 4 : 0) - 16) / 2;
    if (ly < y) ly = y;

    int idx = -1;
    if (hotkey) {
        char hk = (char)toupper((unsigned char)hotkey);
        for (int i = 0; label[i]; i++) {
            if (toupper((unsigned char)label[i]) == hk) { idx = i; break; }
        }
    }
    if (idx < 0) {
        draw_label_centered(x, y, w, h - (active ? 4 : 0), label, active);
        return;
    }

    char before[24] = {0};
    if (idx > 0) {
        memcpy(before, label, (size_t)idx);
        before[idx] = '\0';
    }
    lv_coord_t bw = before[0]
        ? lv_txt_get_width(before, strlen(before), font, 0, LV_TEXT_FLAG_NONE) : 0;
    char one[2] = { label[idx], 0 };
    lv_coord_t cw = lv_txt_get_width(one, 1, font, 0, LV_TEXT_FLAG_NONE);
    if (cw <= 0) cw = 8;
    const char* after = label + idx + 1;

    if (before[0]) {
        lv_obj_t* lab = lv_label_create(g_root);
        lv_label_set_text(lab, before);
        lv_obj_set_style_text_font(lab, font, 0);
        lv_obj_set_style_text_color(lab, lv_color_black(), 0);
        lv_obj_set_pos(lab, lx, ly);
    }
    for (int k = 0; k < 2; k++) {
        lv_obj_t* lab = lv_label_create(g_root);
        lv_label_set_text(lab, one);
        lv_obj_set_style_text_font(lab, font, 0);
        lv_obj_set_style_text_color(lab, lv_color_black(), 0);
        lv_obj_set_pos(lab, lx + (int)bw + k, ly);
    }
    if (after && after[0]) {
        for (int k = 0; k < (active ? 2 : 1); k++) {
            lv_obj_t* lab = lv_label_create(g_root);
            lv_label_set_text(lab, after);
            lv_obj_set_style_text_font(lab, font, 0);
            lv_obj_set_style_text_color(lab, lv_color_black(), 0);
            lv_obj_set_pos(lab, lx + (int)bw + (int)cw + k, ly);
        }
    }
    // Underline tight under hotkey glyph
    lv_obj_t* bar = lv_obj_create(g_root);
    lv_obj_set_size(bar, (int)cw + 2, 2);
    lv_obj_set_pos(bar, lx + (int)bw, ly + 16);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
}

void draw_nav() {
    draw_nav_box(0,         0, TOP_TAB_W, TOP_H, "Settings", g_screen == SCR_SETTINGS, 'S');
    draw_nav_box(TOP_TAB_W, 0, TOP_TAB_W, TOP_H, "Home",     g_screen == SCR_HOME, 'H');
    int x0 = 0;
    // Hotkey letter underlined in draw_nav_box via hotkey char
    draw_nav_box(x0, BOT_TOP, BOT_CONTACT_W, BOT_H, "Contact", g_screen == SCR_CONTACTS, 'C');
    x0 += BOT_CONTACT_W;
    draw_nav_box(x0, BOT_TOP, BOT_MSGBOX_W, BOT_H, "Msg Box", g_screen == SCR_CHATS, 'M');
    x0 += BOT_MSGBOX_W;
    draw_nav_box(x0, BOT_TOP, BOT_PEERS_W, BOT_H, "Peers", g_screen == SCR_PEERS, 'P');
}

// Full-width action button (Announce) — short label fits montserrat_16
void draw_action_btn(int x, int y, int w, int h, const char* label) {
    lv_obj_t* box = lv_obj_create(g_root);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    draw_label_centered(x, y, w, h, label, true);
}

// Plus-style settings/list row: title + subtitle + trailing ">"
void draw_menu_row(int y, int h, const char* title, const char* sub, bool selected) {
    lv_obj_t* row = lv_obj_create(g_root);
    lv_obj_set_size(row, EPD_WIDTH - 8, h);
    lv_obj_set_pos(row, 4, y);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    fat_label(g_root, title, 12, y + 4);
    if (sub && sub[0])
        fat_label(g_root, sub, 12, y + 22);
    fat_label(g_root, ">", EPD_WIDTH - 22, y + (h - 16) / 2);
}

void draw_compose_or_hint(const char* idleHint) {
    if (g_composeLen > 0 || g_composeMode) {
        char foot[40];
        snprintf(foot, sizeof(foot), ">%.26s", g_compose);
        fat_label(g_root, foot, 8, BOT_TOP - 52);
        // Large Back control — leave message mode
        lv_obj_t* bk = lv_obj_create(g_root);
        lv_obj_set_size(bk, EPD_WIDTH - 8, 28);
        lv_obj_set_pos(bk, 4, BOT_TOP - 32);
        lv_obj_set_style_radius(bk, 0, 0);
        lv_obj_set_style_border_width(bk, 2, 0);
        lv_obj_set_style_border_color(bk, lv_color_black(), 0);
        lv_obj_set_style_bg_color(bk, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(bk, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(bk, 0, 0);
        lv_obj_clear_flag(bk, LV_OBJ_FLAG_SCROLLABLE);
        fat_label(g_root, "press ^ to go back", 16, BOT_TOP - 28);
    } else if (idleHint && idleHint[0]) {
        fat_label(g_root, idleHint, 8, BOT_TOP - 18);
    }
    draw_nav();
}

void add_row(int& y, const char* left, const char* right) {
    fat_label(g_root, left, 8, y);
    fat_label(g_root, right, 118, y);
    y += 22;
}

void draw_battery_gauge(int x, int y, int w, int h, int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    // Outer body
    lv_obj_t* body = lv_obj_create(g_root);
    lv_obj_set_size(body, w, h);
    lv_obj_set_pos(body, x, y);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_border_color(body, lv_color_black(), 0);
    lv_obj_set_style_bg_color(body, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    // Tip
    lv_obj_t* tip = lv_obj_create(g_root);
    lv_obj_set_size(tip, 4, h / 2);
    lv_obj_set_pos(tip, x + w, y + h / 4);
    lv_obj_set_style_radius(tip, 0, 0);
    lv_obj_set_style_border_width(tip, 1, 0);
    lv_obj_set_style_border_color(tip, lv_color_black(), 0);
    lv_obj_set_style_bg_color(tip, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    // Fill
    int inner = w - 6;
    int fw = (inner * pct) / 100;
    if (fw > 0) {
        lv_obj_t* fill = lv_obj_create(g_root);
        lv_obj_set_size(fill, fw, h - 6);
        lv_obj_set_pos(fill, x + 3, y + 3);
        lv_obj_set_style_radius(fill, 0, 0);
        lv_obj_set_style_border_width(fill, 0, 0);
        lv_obj_set_style_bg_color(fill, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
    }
    char ps[8];
    snprintf(ps, sizeof(ps), "%d%%", pct);
    fat_label(g_root, ps, x + w + 10, y);
}

void build_home() {
    clear_screen();
    const int y0 = CONTENT_TOP;  // flush under top nav

    // Identity — full width, no side gap, flush under Settings|Home
    lv_obj_t* idBox = lv_obj_create(g_root);
    lv_obj_set_size(idBox, EPD_WIDTH, HOME_ID_H);
    lv_obj_set_pos(idBox, 0, y0);
    lv_obj_set_style_radius(idBox, 0, 0);
    lv_obj_set_style_border_width(idBox, 2, 0);
    lv_obj_set_style_border_color(idBox, lv_color_black(), 0);
    lv_obj_set_style_bg_color(idBox, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(idBox, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(idBox, 0, 0);
    lv_obj_clear_flag(idBox, LV_OBJ_FLAG_SCROLLABLE);

    fat_label(g_root, "IDENTITY", 6, y0 + 2);
    const char* on = (g_loraIf && g_loraIf->isOnline()) ? "ONLINE" : "OFFLINE";
    // ~30px left of prior 118; gauge ~15px left of prior far-right
    fat_label(g_root, on, 93, y0 + 2);
    int pct = 0;
    if (g_pctStr[0] && g_pctStr[0] != '-')
        pct = atoi(g_pctStr);
    draw_battery_gauge(EPD_WIDTH - 72, y0 + 3, 32, 14, pct);
    char shortId[14];
    snprintf(shortId, sizeof(shortId), "%.10s", g_destHex);
    fat_label(g_root, shortId, 6, y0 + 20);
    char lxmfLine[28];
    snprintf(lxmfLine, sizeof(lxmfLine), "LXMF %.10s", g_destHex);
    fat_label(g_root, lxmfLine, 6, y0 + 38);

    // 3x2 tiles — flush, 2px gaps only
    const int gx = 0;
    const int gy = HOME_GRID_Y;
    const int tw = TILE_W;
    const int th = TILE_H;
    const int gap = TILE_GAP;
    char peerN[8];
    if (g_storeReady)
        snprintf(peerN, sizeof(peerN), "%u", (unsigned)g_msgStore.totalConversations());
    else
        strcpy(peerN, "0");
    const char* loraV = g_featLora && g_loraIf && g_loraIf->isOnline() ? "ON" : "OFF";
    draw_tile(gx,                gy,            tw, th, "LORA",  loraV);
    draw_tile(gx + tw + gap,     gy,            tw, th, "TCP",   g_featTcp ? "ON" : "OFF");
    draw_tile(gx + 2*(tw + gap), gy,            tw, th, "WIFI",  g_featWifi ? "ON" : "OFF");
    draw_tile(gx,                gy + th + gap, tw, th, "PEERS", peerN);
    draw_tile(gx + tw + gap,     gy + th + gap, tw, th, "AUDIO", g_featAudio ? "ON" : "OFF");
    draw_tile(gx + 2*(tw + gap), gy + th + gap, tw, th, "GPS",   g_featGps ? "ON" : "OFF");

    // Status + full-width Announce — packed under tiles
    int sy = gy + 2 * (th + gap) + 2;
    fat_label(g_root, "Listening for peers", 4, sy);
    char annAgo[28];
    snprintf(annAgo, sizeof(annAgo), "Announced: %s", g_announceStr);
    fat_label(g_root, annAgo, 4, sy + 16);
    draw_action_btn(0, sy + 34, EPD_WIDTH, HOME_ANN_H, "Announce");

    draw_compose_or_hint("");
}

void build_list_body(const char* section, const char* emptyNote) {
    for (int i = 0; i < g_msgRowCount; i++) {
        if (g_msgRows[i].used)
            formatPeerLine(g_msgRows[i].line1, sizeof(g_msgRows[i].line1),
                           g_msgRows[i].peer, g_msgRows[i].unread);
    }
    clear_screen();
    int y = CONTENT_TOP;
    if (section && section[0]) {
        fat_label(g_root, section, 8, y);
        y += 24;
    }
    if (g_msgRowCount == 0) {
        fat_label(g_root, emptyNote && emptyNote[0] ? emptyNote : "(empty)", 8, y);
    } else {
        for (int i = 0; i < g_msgRowCount; i++) {
            char title[28], sub[28];
            snprintf(title, sizeof(title), "%d%s %s",
                     i + 1,
                     (i == g_selPeer) ? ">" : " ",
                     g_msgRows[i].line1);
            if (g_msgRows[i].line2[0])
                snprintf(sub, sizeof(sub), "%.26s", g_msgRows[i].line2);
            else
                sub[0] = '\0';
            draw_menu_row(y, sub[0] ? 44 : 30, title, sub, i == g_selPeer);
            y += (sub[0] ? 48 : 34);
            if (y > CONTENT_BOTTOM - 8) break;
        }
    }
}

void build_chats() {
    // Plus Chats: thread list with delivery-ish subtitle
    if (g_showHistory) {
        clear_screen();
        int y = CONTENT_TOP;
        fat_label(g_root, "Thread", 8, y); y += 22;
        if (g_msgRowCount > 0 && g_msgRows[g_selPeer].used) {
            char hdr[32];
            snprintf(hdr, sizeof(hdr), ">%s", g_msgRows[g_selPeer].line1);
            fat_label(g_root, hdr, 8, y);
            y += 26;
        }
        if (g_histRowCount == 0) {
            fat_label(g_root, g_histNote[0] ? g_histNote : "(no messages)", 8, y);
        } else {
            for (int i = 0; i < g_histRowCount; i++) {
                fat_label(g_root, g_histRows[i].line, 8, y);
                y += 24;
                if (y > CONTENT_BOTTOM - 4) break;
            }
        }
        draw_compose_or_hint("H=back  type+Enter");
        return;
    }
    loadMessageRows(false);
    char sec[32];
    snprintf(sec, sizeof(sec), "MSG BOX  %s  %s", g_convStr, g_unreadStr);
    build_list_body(sec, g_msgNote[0] ? g_msgNote : "(no chats)");
    draw_compose_or_hint("type+Enter send  V=thread  Back=esc");
}

void build_contacts() {
    // Plus Contacts: Share My QR header + contact rows
    g_showHistory = false;
    g_msgRowsLoaded = false;
    if (g_storeReady) g_msgStore.poll();
    g_msgRowCount = 0;
    g_msgNote[0] = '\0';
    for (int i = 0; i < MSG_ROWS; i++) g_msgRows[i].used = false;
    if (g_storeReady) {
        if (!tryConversationPage(MSG_ROWS)) tryConversationPage(1);
    }
    if (g_msgRowCount == 0)
        snprintf(g_msgNote, sizeof(g_msgNote), "(no contacts yet)");
    clampSelPeer();

    clear_screen();
    int y = CONTENT_TOP + 4;
    // Share My QR row (Plus) — display only on e-ink
    draw_menu_row(y, 40, "Share My QR", g_destHex, false);
    y += 48;
    char sec[28];
    snprintf(sec, sizeof(sec), "CONTACTS (%d)", g_msgRowCount);
    fat_label(g_root, sec, 8, y);
    y += 26;
    if (g_msgRowCount == 0) {
        fat_label(g_root, g_msgNote, 8, y);
    } else {
        for (int i = 0; i < g_msgRowCount; i++) {
            char title[28], sub[36];
            snprintf(title, sizeof(title), "%d%s %s",
                     i + 1, (i == g_selPeer) ? ">" : " ",
                     g_msgRows[i].line1);
            char hex[33];
            peerToHex32(g_msgRows[i].peer, hex);
            snprintf(sub, sizeof(sub), "ID: %.16s", hex);
            draw_menu_row(y, 44, title, sub, i == g_selPeer);
            y += 48;
            if (y > CONTENT_BOTTOM - 8) break;
        }
    }
    draw_compose_or_hint("1-9 select  type+Enter");
}

void build_peers() {
    g_lastPeerCount = g_announceMgr.nodes().size();
    g_peerDirty = false;
    // Plus Peers: RECENT PEERS section
    g_showHistory = false;
    g_msgRowsLoaded = false;
    g_msgRowCount = 0;
    g_msgNote[0] = '\0';
    for (int i = 0; i < MSG_ROWS; i++) g_msgRows[i].used = false;
    appendAnnouncedPeers();
    if (g_msgRowCount == 0)
        snprintf(g_msgNote, sizeof(g_msgNote), "(none — Announce on Home)");
    clampSelPeer();

    clear_screen();
    int y = CONTENT_TOP;
    char sec[28];
    snprintf(sec, sizeof(sec), "RECENT PEERS (%d)", g_msgRowCount);
    fat_label(g_root, sec, 8, y);
    y += 24;
    if (g_msgRowCount == 0) {
        fat_label(g_root, g_msgNote, 8, y);
    } else {
        for (int i = 0; i < g_msgRowCount; i++) {
            char title[28], sub[36];
            snprintf(title, sizeof(title), "%d%s %s",
                     i + 1, (i == g_selPeer) ? ">" : " ",
                     g_msgRows[i].line1);
            char hex[33];
            peerToHex32(g_msgRows[i].peer, hex);
            snprintf(sub, sizeof(sub), "ID: %.16s", hex);
            draw_menu_row(y, 44, title, sub, i == g_selPeer);
            y += 48;
            if (y > CONTENT_BOTTOM - 8) break;
        }
    }
    draw_compose_or_hint("1-9 select  type+Enter");
}

void build_settings() {
    // Plus Settings: menu rows with subtitle + >
    clear_screen();
    int y = CONTENT_TOP + 2;

    char batSub[28];
    snprintf(batSub, sizeof(batSub), "%s %s", g_batStr, g_pctStr);
    char loraSub[32];
    snprintf(loraSub, sizeof(loraSub), "Long Fast %.0f MHz",
             LORA_DEFAULT_FREQ / 1e6f);
    char idSub[28];
    snprintf(idSub, sizeof(idSub), "%.16s", g_destHex);

    draw_menu_row(y, 44, "Identity & Device", idSub, false); y += 48;
    draw_menu_row(y, 44, "Battery", batSub, false); y += 48;
    draw_menu_row(y, 44, "LoRa", loraSub, false); y += 48;
    char annSub[28];
    snprintf(annSub, sizeof(annSub), "Ann %s", g_announceStr);
    draw_menu_row(y, 44, "Announce / TX", annSub, false);

    draw_compose_or_hint("");
}

void show_screen(ScreenId id) {
    g_screen = id;
    refreshLiveData();
    switch (id) {
        case SCR_HOME:     build_home(); break;
        case SCR_CHATS:    build_chats(); break;
        case SCR_CONTACTS: build_contacts(); break;
        case SCR_PEERS:    build_peers(); break;
        case SCR_SETTINGS: build_settings(); break;
        default:           build_home(); break;
    }
    lv_refr_now(nullptr);
    g_display.refreshIfDirty();
    if (g_storeReady) g_lastUiRevision = g_msgStore.revision();
    g_composeDirty = false;
    Serial.printf("[UI] screen %u (%s) drawn\n", (unsigned)id, tabLabel(id));
}

void next_screen() {
    show_screen((ScreenId)((g_screen + 1) % SCR_COUNT));
}

void maybeAutoRedrawMessages() {
    if (g_screen != SCR_CHATS || !g_storeReady) return;
    g_msgStore.poll();
    const uint32_t rev = g_msgStore.revision();
    if (rev == g_lastUiRevision) return;
    if (millis() - g_lastMsgRedrawMs < MSG_REDRAW_MIN_MS) return;
    g_lastMsgRedrawMs = millis();
    Serial.printf("[UI] auto Messages redraw rev %u -> %u\n",
                  (unsigned)g_lastUiRevision, (unsigned)rev);
    show_screen(SCR_CHATS);
}

// E21: after typing pauses, one fullRefresh so the footer shows the compose line.
// Never refresh per keypress — only when idle and dirty.
// Peers screen: refresh when announce set grows; throttle full refresh to 30s
void maybeAutoRedrawPeers() {
    if (g_screen != SCR_PEERS || g_composeLen > 0) return;
    size_t n = g_announceMgr.nodes().size();
    if (n == g_lastPeerCount && !g_peerDirty) return;
    uint32_t now = millis();
    bool first = (g_lastPeerAutoMs == 0);
    if (!first && (now - g_lastPeerAutoMs) < 30000u) {
        g_peerDirty = true;  // pending until throttle allows
        return;
    }
    g_lastPeerCount = n;
    g_lastPeerAutoMs = now;
    g_peerDirty = false;
    Serial.printf("[PEER] auto-refresh count=%u\n", (unsigned)n);
    show_screen(SCR_PEERS);
}


void maybeComposeRedraw() {
    if (!g_composeDirty) return;
    if (g_lastComposeMs == 0) return;
    if ((millis() - g_lastComposeMs) < COMPOSE_IDLE_MS) return;
    if (millis() - g_lastMsgRedrawMs < MSG_REDRAW_MIN_MS) return;
    // Avoid full e-ink rebuild when heap is tight — Enter needs room for lxmfSubmit
    size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (freeInt < 14000) {
        // Rate-limit: one log per 5s, and push next attempt out so we do not spin
        static uint32_t s_lastSkipLog = 0;
        uint32_t now = millis();
        if (now - s_lastSkipLog > 5000u) {
            s_lastSkipLog = now;
            Serial.printf("[UI] skip compose redraw low heap %u (will retry)\n",
                          (unsigned)freeInt);
        }
        // Defer without clearing dirty — but don't re-check every loop
        g_lastMsgRedrawMs = now;
        return;
    }
    g_lastMsgRedrawMs = millis();
    Serial.printf("[UI] compose idle redraw len=%u\n", (unsigned)g_composeLen);
    show_screen(g_screen);
}

// Handle one keyboard event. Returns true if a full screen redraw is needed.
bool handleKeyEvent(const KeyEvent& ev) {
    // Enter: send compose if non-empty, else next screen.
    if (ev.enter) {
        if (g_composeLen > 0) {
            char body[COMPOSE_CAP + 1];
            memcpy(body, g_compose, g_composeLen + 1);
            Serial.printf("[TX] Enter compose len=%u body=\"%s\"\n",
                          (unsigned)g_composeLen, body);
            composeClear();
            g_composeMode = false;
            doSendTest(body);
            return true;
        }
        Serial.println("[KEY] Enter empty — next screen");
        next_screen();
        return false;
    }

    if (ev.del) {
        composeBackspace();
        return false;
    }

    if (ev.space) {
        composeAppend(' ');
        return false;
    }

    // Printable character
    if (ev.character >= 32 && ev.character <= 126) {
        // Shortcuts only when compose is empty
        // A announce  S settings  H home  I inbox  M msg box  P peers
        // V view thread  N next peer  T test ping
        if (g_composeLen == 0) {
            const bool onMsgScreens =
                (g_screen == SCR_CHATS || g_screen == SCR_CONTACTS ||
                 g_screen == SCR_PEERS);

            // 1-9: select peer row on messaging screens only
            if (onMsgScreens && ev.character >= '1' && ev.character <= '9') {
                int idx = ev.character - '1';
                if (idx < g_msgRowCount && g_msgRows[idx].used) {
                    g_selPeer = idx;
                    char hex[33];
                    peerToHex32(g_msgRows[idx].peer, hex);
                    Serial.printf("[PEER] key %c -> sel=%d %s\n",
                                  ev.character, g_selPeer, hex);
                    show_screen(g_screen);
                    return false;
                }
            }

            // On Msg Box / Contact / Peers: letters always type (fix HELLO→Home)
            // Only V=thread N=next T=test remain as shortcuts there.
            if (onMsgScreens) {
                if (ev.character == 'v' || ev.character == 'V') {
                    g_showHistory = !g_showHistory;
                    if (g_showHistory) loadHistoryRows();
                    show_screen(SCR_CHATS);
                    return false;
                }
                if (ev.character == 'n' || ev.character == 'N') {
                    if (cycleSelPeer()) {
                        if (g_showHistory) loadHistoryRows();
                        return true;
                    }
                    return false;
                }
                if (ev.character == 't' || ev.character == 'T') {
                    doSendTest(nullptr);
                    return true;
                }
                // fall through to composeAppend
            } else {
                // Home / Settings: full nav shortcuts
                if (ev.character == 'a' || ev.character == 'A') {
                    doAnnounce();
                    if (g_featAudio) g_audio.playAnnounce();
                    return true;
                }
                if (ev.character == 's' || ev.character == 'S') {
                    g_showHistory = false;
                    show_screen(SCR_SETTINGS);
                    return false;
                }
                if (ev.character == 'h' || ev.character == 'H') {
                    g_showHistory = false;
                    show_screen(SCR_HOME);
                    return false;
                }
                if (ev.character == 'm' || ev.character == 'M') {
                    g_showHistory = false;
                    show_screen(SCR_CHATS);
                    return false;
                }
                if (ev.character == 'c' || ev.character == 'C') {
                    g_showHistory = false;
                    show_screen(SCR_CONTACTS);
                    return false;
                }
                if (ev.character == 'p' || ev.character == 'P') {
                    g_showHistory = false;
                    show_screen(SCR_PEERS);
                    return false;
                }
                if (ev.character == 't' || ev.character == 'T') {
                    doSendTest(nullptr);
                    return true;
                }
            }
        }
        composeAppend(ev.character);
        return false;
    }

    return false;
}

}  // namespace (anonymous)

bool begin() {
    Serial.println();
    Serial.println("========================================");
    Serial.println(" RATSPEAK  T-Deck Pro  Phase G1.12");
    Serial.println(" no skip-log flood + ONLINE +5");
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

    g_audio.begin();
    g_audio.setEnabled(g_featAudio);
#if defined(BOARD_MOTOR_PIN) && (BOARD_MOTOR_PIN >= 0)
    pinMode(BOARD_MOTOR_PIN, OUTPUT);
    digitalWrite(BOARD_MOTOR_PIN, LOW);
#endif
    Serial.printf("[AUDIO] notify %s motor=%d\n",
                  g_audio.isReady() ? "ready" : "no-i2s",
#if defined(BOARD_MOTOR_PIN)
                  BOARD_MOTOR_PIN
#else
                  -1
#endif
    );

    // E22: probe CST328 (already reset earlier). Soft-fail keeps INT-only next.
    Wire.beginTransmission(TOUCH_I2C_ADDR);
    g_touchOk = (Wire.endTransmission() == 0);
    Serial.printf("[TOUCH] CST328 %s (0x%02X)\n",
                  g_touchOk ? "ok" : "absent", (unsigned)TOUCH_I2C_ADDR);

    show_screen(SCR_HOME);
    Serial.println("[BOOT] ready");
    g_bootAnnAt = millis() + 3000;
    Serial.println("[HINT] type+Enter=send | p=peer | touch=row | H hist | a ann | r reload");
    return true;
}

void loop() {
    if (g_protoReady) g_proto.loop();
    if (g_storeReady) g_msgStore.poll();
    g_announceMgr.loop();
    g_audio.loop();

    // Beep+vibe on new inbound (revision bump while not composing)
    static uint32_t s_prevRev = 0;
    if (g_storeReady) {
        uint32_t rev = g_msgStore.revision();
        if (s_prevRev != 0 && rev > s_prevRev && g_composeLen == 0) {
            if (g_msgStore.totalUnreadCount() > 0)
                notifyInbound();
        }
        s_prevRev = rev;
    }

    if (!g_bootAnnDone && g_bootAnnAt != 0 && millis() >= g_bootAnnAt) {
        g_bootAnnDone = true;
        Serial.println("[ANN] boot announce");
        doAnnounce();
    }

    maybeAutoRedrawMessages();
    maybeAutoRedrawPeers();
    maybeComposeRedraw();

    if (g_kbOk) {
        g_kb.update();
        if (g_kb.hasEvent()) {
            const KeyEvent& ev = g_kb.getEvent();
            bool needRedraw = handleKeyEvent(ev);
            g_kb.discardPending();
            if (needRedraw)
                show_screen(g_screen);
        }
    }

    static int lastInt = HIGH;
    static uint32_t lastTouchMs = 0;
    int tInt = digitalRead(TOUCH_INT);
    if (lastInt == HIGH && tInt == LOW && (millis() - lastTouchMs > 400)) {
        lastTouchMs = millis();
        int16_t tx = 0, ty = 0;
        if (g_touchOk && cst328Read(tx, ty)) {
            handleTouchAt(tx, ty);
        } else {
            Serial.println("[TOUCH] INT next");
            next_screen();
        }
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
                if (g_screen == SCR_SETTINGS || g_screen == SCR_CHATS || g_screen == SCR_CONTACTS || g_screen == SCR_PEERS)
                    show_screen(g_screen);
            } else if (p[0] == 'a' || p[0] == 'A') {
                doAnnounce();
                if (g_screen == SCR_SETTINGS) show_screen(SCR_SETTINGS);
            } else if (p[0] == 'r' || p[0] == 'R') {
                loadMessageRows(true);
                show_screen(SCR_CHATS);
            } else if (p[0] == 'H') {
                g_showHistory = !g_showHistory;
                if (g_showHistory) loadHistoryRows();
                show_screen(SCR_CHATS);
            } else if (p[0] == 'n' || p[0] == 'N' || p[0] == '>') {
                g_showHistory = false;
                next_screen();
            } else if (p[0] == 'h' || p[0] == '1') {
                g_showHistory = false;
                show_screen(SCR_HOME);
            } else if (p[0] == 'm' || p[0] == 'M' || p[0] == '2') {
                g_showHistory = false;
                show_screen(SCR_CHATS);
            } else if (p[0] == '3') {
                g_showHistory = false;
                show_screen(SCR_CONTACTS);
            } else if (p[0] == 'e' || p[0] == 'E' || p[0] == '4') {
                g_showHistory = false;
                show_screen(SCR_PEERS);
            } else if (p[0] == 's' || p[0] == 'S' || p[0] == '5') {
                g_showHistory = false;
                show_screen(SCR_SETTINGS);
            } else if (p[0] == 'c' || p[0] == 'C') {
                // serial clear compose
                composeClear();
                Serial.println("[COMPOSE] cleared");
                if (g_screen == SCR_CHATS || g_screen == SCR_SETTINGS || g_screen == SCR_CONTACTS || g_screen == SCR_PEERS)
                    show_screen(g_screen);
            } else if (p[0] == 'p' || p[0] == 'P') {
                if (cycleSelPeer()) {
                    if (g_showHistory) loadHistoryRows();
                    if (g_screen == SCR_CHATS || g_screen == SCR_CONTACTS ||
                        g_screen == SCR_PEERS)
                        show_screen(g_screen);
                }
            } else {
                Serial.printf("[CMD] unknown \"%s\" (t/a/r/n/m/h/s/H/c/p)\n", p);
            }
        } else if (c >= 32 && c < 127) {
            if (lineLen + 1 < sizeof(line)) line[lineLen++] = c;
        }
    }
    delay(5);
}

}  // namespace standalone
