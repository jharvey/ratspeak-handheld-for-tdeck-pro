#include "StandaloneApp.h"

#include <SPI.h>
#include <Wire.h>
#include <lvgl.h>
#include <Preferences.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <LittleFS.h>

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
bool g_darkTheme = false;  // false=white paper, true=black-ink select

struct LoraPreset { const char* name; uint8_t sf; float bw; uint8_t cr; };
static const LoraPreset kLoraPresets[] = {
    {"Long Fast",  11, 250000.f, 5},
    {"Long Slow",  12, 125000.f, 8},
    {"Medium",     10, 250000.f, 5},
    {"Fast Short",  7, 500000.f, 5},
};
static constexpr int kLoraPresetCount = 4;
int g_loraPreset = 0;

static inline lv_color_t theme_fg() {
    return g_darkTheme ? lv_color_white() : lv_color_black();
}
static inline lv_color_t theme_bg() {
    return g_darkTheme ? lv_color_black() : lv_color_white();
}

// Settings hierarchy (Plus-like). SET_ROOT = top list; others = sub-pages.
enum SettingsPage : uint8_t {
    SET_ROOT = 0,
    SET_IDENTITY,
    SET_SCREEN,
    SET_BATTERY,
    SET_LORA,
    SET_NETWORK,
    SET_TIME,
    SET_AUDIO,
    SET_DIAG,
    SET_STORAGE,
};
SettingsPage g_settingsPage = SET_ROOT;
int g_settingsSel = 0;  // highlighted row on current page
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

constexpr int MSG_ROWS = 6;
struct MsgRow {
    char line1[28];
    char line2[28];
    uint8_t peer[16];
    uint32_t unread;
    bool used;
    bool saved;  // true = trusted contact
};
bool g_nameEditMode = false;  // Contacts: type name then Enter to save
MsgRow g_msgRows[MSG_ROWS];
int g_msgRowCount = 0;
int g_selPeer = 0;  // E20: selected MsgRow for TX / History / mark-read
char g_msgNote[40] = "";
bool g_msgRowsLoaded = false;
uint32_t g_msgCacheRevision = 0;

// Local display-name overrides: show "1> RADIO1" even if announce name is "1".
struct LocalName {
    uint8_t peer[16];
    char name[24];
    bool used;
};
LocalName g_localNames[8] = {};
void setLocalName(const uint8_t peer[16], const char* name) {
    if (!peer || !name || !name[0]) return;
    for (int i = 0; i < 8; i++) {
        if (g_localNames[i].used && memcmp(g_localNames[i].peer, peer, 16) == 0) {
            snprintf(g_localNames[i].name, sizeof(g_localNames[i].name), "%.23s", name);
            return;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (!g_localNames[i].used) {
            g_localNames[i].used = true;
            memcpy(g_localNames[i].peer, peer, 16);
            snprintf(g_localNames[i].name, sizeof(g_localNames[i].name), "%.23s", name);
            return;
        }
    }
}
const char* getLocalName(const uint8_t peer[16]) {
    if (!peer) return nullptr;
    for (int i = 0; i < 8; i++) {
        if (g_localNames[i].used && memcmp(g_localNames[i].peer, peer, 16) == 0)
            return g_localNames[i].name;
    }
    return nullptr;
}

constexpr int HIST_ROWS = 6;
struct HistRow {
    char line[36];   // "> HELLO" or "< hi"
    char st[8];      // out: sent/queued/deliv; in: ""
    bool incoming;
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
void settingsGoBack();
void settingsActivate();
bool inMessageUi();
void goBackFromMessage();

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
    // found=1 size=0 means the chip ID was seen but OPI PSRAM was not mapped.
    // Board must use memory_type=qio_opi + BOARD_HAS_PSRAM (see lilygo_t_deck_pro.json).
    bool found = psramFound();
    size_t sz = ESP.getPsramSize();
    size_t fr = ESP.getFreePsram();
    size_t capSp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t capIn = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    Serial.printf("[PSRAM] found=%d size=%u free=%u  caps_spiram=%u caps_int=%u\n",
                  (int)found, (unsigned)sz, (unsigned)fr,
                  (unsigned)capSp, (unsigned)capIn);
    if (found && sz == 0) {
        Serial.println("[PSRAM] WARN chip seen but size=0 — OPI not mapped");
        Serial.println("[PSRAM] Fix: board=lilygo_t_deck_pro memory_type=qio_qspi");
        Serial.println("[PSRAM]      (Pro uses QSPI PSRAM, not OPI qio_opi)");
    } else if (found && sz > 0) {
        Serial.printf("[PSRAM] OK mapped %u KB — protocol can use SPIRAM\n",
                      (unsigned)(sz / 1024));
    } else {
        Serial.println("[PSRAM] none — stuck on INTERNAL SMALL node");
    }
}

bool tryProto(uint32_t caps, const char* label) {
    Serial.printf("[PROTO] try %s free_int=%u\n", label,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return g_proto.begin(&g_flash, nullptr, &g_idMgr, &g_msgStore,
                         &g_announceMgr, RS_HANDHELD_PROFILE_SMALL, caps);
}

// When FlashStore::begin fails on corrupt media, format() is a no-op because
// it requires _ready==true. Bypass: erase SPIFFS/littlefs partition, format, remount.
bool recoverCorruptLittleFS() {
    Serial.println("[FLASH] recoverCorruptLittleFS…");
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "littlefs");
    if (!part)
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!part)
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
    if (!part) {
        Serial.println("[FLASH] no data/spiffs partition found");
        return false;
    }
    Serial.printf("[FLASH] erase label=%s size=%u offset=0x%x\n",
                  part->label, (unsigned)part->size, (unsigned)part->address);
    LittleFS.end();
    esp_err_t e = esp_partition_erase_range(part, 0, part->size);
    if (e != ESP_OK) {
        Serial.printf("[FLASH] erase failed err=0x%x\n", (unsigned)e);
        return false;
    }
    // format=true mounts a fresh FS on the blank partition
    if (!LittleFS.begin(true, "/littlefs", 10, part->label)) {
        Serial.println("[FLASH] LittleFS.begin(format) failed after erase");
        return false;
    }
    LittleFS.end();
    Serial.println("[FLASH] partition erased + formatted — calling FlashStore::begin");
    return g_flash.begin();
}

bool initStorageAndProto() {
    // Corrupted LittleFS → mount fails, FlashStore::format refuses (!_ready).
    if (!g_flash.begin()) {
        Serial.println("[FLASH] begin failed — partition erase recovery");
        if (!recoverCorruptLittleFS()) {
            Serial.println("[FLASH] recovery failed — no identity/announce");
            return false;
        }
        Serial.println("[FLASH] recovery OK — fresh FS");
    }
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

void applyLoraPreset(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kLoraPresetCount) idx = kLoraPresetCount - 1;
    g_loraPreset = idx;
    const LoraPreset& pr = kLoraPresets[g_loraPreset];
    if (g_radio) {
        g_radio->setSpreadingFactor(pr.sf);
        g_radio->setSignalBandwidth(pr.bw);
        g_radio->setCodingRate4(pr.cr);
        Serial.printf("[LORA] preset=%s SF=%u BW=%.0f CR=%u\n",
                      pr.name, (unsigned)pr.sf, pr.bw, (unsigned)pr.cr);
    } else {
        Serial.printf("[LORA] preset=%s (radio null)\n", pr.name);
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

    // OutgoingContract::Rejection:
    // 0 None 1 Busy 2 Invalid 3 TooLarge 4 NoMemory 5 Unavailable
    // 6 Fenced 7 Exhausted 8 Recovering 9 Stopped
    auto rejectionName = [](unsigned r) -> const char* {
        switch (r) {
            case 0: return "None";
            case 1: return "Busy";
            case 2: return "Invalid";
            case 3: return "TooLarge";
            case 4: return "NoMemory";
            case 5: return "Unavailable";
            case 6: return "Fenced";
            case 7: return "Exhausted";
            case 8: return "Recovering";
            case 9: return "Stopped";
            default: return "?";
        }
    };
    // storage::Outcome: 0 Committed 1 Failed 2 Cancelled
    // storage::Error: 0 None 1 Unavailable 2 Allocation 3 InvalidRecord ...
    auto errorName = [](unsigned e) -> const char* {
        switch (e) {
            case 0: return "None";
            case 1: return "Unavailable";
            case 2: return "Allocation";
            case 3: return "InvalidRecord";
            case 4: return "CounterExhausted";
            case 5: return "RevisionExhausted";
            case 6: return "Read";
            case 7: return "Write";
            case 8: return "Verify";
            case 9: return "Rename";
            case 10: return "Stale";
            case 11: return "Cancelled";
            case 12: return "Internal";
            default: return "?";
        }
    };

    // Reclaim LVGL widgets so MessageStore CreateOutgoing can allocate
    if (g_root) {
        lv_obj_clean(g_root);
        Serial.printf("[TX] lv_obj_clean free_int=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    delay(30);

    // Short recovery tick (boot drain should have finished Recovering)
    for (int i = 0; i < 30; i++) {
        g_proto.loop();
        if (g_storeReady) g_msgStore.poll();
        delay(10);
    }
    Serial.printf("[TX] pre-submit free_int=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    auto sub = g_proto.lxmfSubmit(dest, nullptr, 0,
                                  (const uint8_t*)local, n, false);
    if (!sub.accepted()) {
        unsigned r = (unsigned)sub.rejection;
        snprintf(g_txStr, sizeof(g_txStr), "rej %u", r);
        Serial.printf("[TX] rejected %u (%s) free_int=%u\n",
                      r, rejectionName(r),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        if (r == 8 || r == 1) {
            for (int attempt = 0; attempt < 5; attempt++) {
                for (int i = 0; i < 40; i++) {
                    g_proto.loop();
                    if (g_storeReady) g_msgStore.poll();
                    delay(15);
                }
                sub = g_proto.lxmfSubmit(dest, nullptr, 0,
                                         (const uint8_t*)local, n, false);
                if (sub.accepted()) {
                    Serial.printf("[TX] accepted after wait attempt %d\n", attempt + 1);
                    break;
                }
                Serial.printf("[TX] still %u (%s) attempt %d\n",
                              (unsigned)sub.rejection,
                              rejectionName((unsigned)sub.rejection),
                              attempt + 1);
            }
            if (!sub.accepted()) {
                Serial.println("[TX] give up — still Recovering/Busy");
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
            unsigned oc = (unsigned)ir.outcome;
            unsigned er = (unsigned)ir.error;
            // Outcome: 0 Committed 1 Failed 2 Cancelled
            const char* ocName = oc == 0 ? "Committed" : oc == 1 ? "Failed" : oc == 2 ? "Cancelled" : "?";
            Serial.printf("[TX] outcome=%u (%s) err=%u (%s) suppressed=%d free_int=%u\n",
                          oc, ocName, er,
                          er == 0 ? "None" : er == 1 ? "Unavailable" : er == 2 ? "Allocation"
                          : er == 3 ? "InvalidRecord" : er == 7 ? "Write" : "?",
                          (int)ir.txSuppressed,
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            g_proto.lxmfAcknowledge(sub.ticket);
            if (ir.outcome == handheld::storage::Outcome::Committed) {
                strcpy(g_txStr, ir.txSuppressed ? "saved" : "sent");
                g_msgRowsLoaded = false;
                if (g_featAudio) g_audio.playMessage();
                // N1/N2: refresh thread so new outbound appears with status
                if (g_showHistory) loadHistoryRows();
            } else if (ir.error == handheld::storage::Error::Allocation) {
                // Retry once after another clean — Allocation at ~12K is common
                Serial.println("[TX] Allocation fail — clean+retry once");
                if (g_root) lv_obj_clean(g_root);
                delay(50);
                for (int i = 0; i < 20; i++) {
                    g_proto.loop();
                    if (g_storeReady) g_msgStore.poll();
                    delay(10);
                }
                auto sub2 = g_proto.lxmfSubmit(dest, nullptr, 0,
                                               (const uint8_t*)local, n, false);
                if (!sub2.accepted()) {
                    snprintf(g_txStr, sizeof(g_txStr), "alloc rej");
                    Serial.printf("[TX] retry rejected %u free_int=%u\n",
                                  (unsigned)sub2.rejection,
                                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                    return;
                }
                for (int j = 0; j < 800; j++) {
                    g_proto.loop();
                    if (g_storeReady) g_msgStore.poll();
                    handheld::outgoing::InitialResult ir2{};
                    auto p2 = g_proto.lxmfPoll(sub2.ticket, ir2);
                    if (p2 == handheld::outgoing::Poll::Pending) { delay(10); continue; }
                    if (p2 == handheld::outgoing::Poll::Ready) {
                        g_proto.lxmfAcknowledge(sub2.ticket);
                        Serial.printf("[TX] retry outcome=%u err=%u free_int=%u\n",
                                      (unsigned)ir2.outcome, (unsigned)ir2.error,
                                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                        if (ir2.outcome == handheld::storage::Outcome::Committed) {
                            strcpy(g_txStr, ir2.txSuppressed ? "saved" : "sent");
                            g_msgRowsLoaded = false;
                            if (g_featAudio) g_audio.playMessage();
                            if (g_showHistory) loadHistoryRows();
                        } else {
                            snprintf(g_txStr, sizeof(g_txStr), "fail %u", (unsigned)ir2.error);
                        }
                        return;
                    }
                    break;
                }
                strcpy(g_txStr, "alloc timeout");
            } else {
                snprintf(g_txStr, sizeof(g_txStr), "fail %u", er);
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
    // Priority: user override > name cache > announce node.name
    // Skip tiny placeholder announce names ("1") so list shows short id until renamed.
    auto usable = [](const std::string& n) -> bool {
        if (n.empty()) return false;
        if (n.size() <= 2) {
            // single digit / two-char placeholders from default display names
            bool allDigit = true;
            for (char c : n) if (c < '0' || c > '9') { allDigit = false; break; }
            if (allDigit) return false;
        }
        return true;
    };
    if (const char* loc = getLocalName(peer)) {
        if (loc[0]) return std::string(loc);
    }
    char hex[33];
    peerToHex32(peer, hex);
    std::string name = g_announceMgr.lookupName(hex);
    if (usable(name)) return name;
    char hexUp[33];
    for (int i = 0; i < 32; i++) {
        char c = hex[i];
        hexUp[i] = (c >= 'a' && c <= 'f') ? (char)(c - 32) : c;
    }
    hexUp[32] = 0;
    name = g_announceMgr.lookupName(hexUp);
    if (usable(name)) return name;
    const DiscoveredNode* n = g_announceMgr.findNode(rs::Bytes(peer, 16));
    if (n && usable(n->name)) return n->name;
    n = g_announceMgr.findNodeByHex(hex);
    if (n && usable(n->name)) return n->name;
    for (const auto& node : g_announceMgr.nodes()) {
        if (node.hash.size() == 16 && memcmp(node.hash.data(), peer, 16) == 0 &&
            usable(node.name))
            return node.name;
    }
    return {};
}

void formatPeerLine(char* out, size_t outLen, const uint8_t peer[16], uint32_t unread) {
    // Title: name only ("RADIO1"); list prefix draws ">1 "
    (void)unread;
    std::string name = resolvePeerName(peer);
    if (!name.empty()) {
        snprintf(out, outLen, "%.20s", name.c_str());
        return;
    }
    snprintf(out, outLen, "%02x%02x%02x%02x%02x%02x",
             peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
}

void formatPeerSub(char* out, size_t outLen, const uint8_t peer[16], const char* preview) {
    // Full address only on line 2
    (void)preview;
    char hex[33];
    peerToHex32(peer, hex);
    snprintf(out, outLen, "%.28s", hex);
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

bool inMessageUi() {
    return g_showHistory || g_composeLen > 0 || g_composeMode || g_nameEditMode;
}

void goBackFromMessage() {
    bool changed = false;
    if (g_nameEditMode) {
        g_nameEditMode = false;
        composeClear();
        g_composeMode = false;
        changed = true;
        Serial.println("[UI] Back — cancel name edit");
    }
    if (g_composeLen > 0 || g_composeMode) {
        composeClear();
        g_composeMode = false;
        changed = true;
        Serial.println("[UI] Back — cleared compose");
    }
    if (g_showHistory) {
        g_showHistory = false;
        changed = true;
        Serial.println("[UI] Back — left thread");
    }
    if (changed) show_screen(g_screen);
}

// Unified Back: message UI, Settings sub-page, or stay
bool doBackAction() {
    if (inMessageUi()) {
        goBackFromMessage();
        return true;
    }
    if (g_screen == SCR_SETTINGS && g_settingsPage != SET_ROOT) {
        settingsGoBack();
        return true;
    }
    return false;
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
constexpr int STATS_H        = 32;  // CONV / NEW / UNRD strip
constexpr int PEERS_TOP      = CONTENT_TOP + STATS_H + 4;
constexpr int ROW_H          = 44;
constexpr int SET_ROW_H      = 44;
int g_settingsScroll = 0;
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

    // Message mode: only Back Up bars (top or bottom full width)
    if (inMessageUi()) {
        if (y < TOP_H || y >= BOT_TOP) {
            Serial.println("[TAB] Back Up (message mode)");
            goBackFromMessage();
            return true;
        }
        // Content area: keep thread open (do not dismiss on body tap)
        return false;
    }

    // Top nav: Settings | Home  (on Settings sub-page, left half = Back)
    if (y < TOP_H) {
        if (g_screen == SCR_SETTINGS && g_settingsPage != SET_ROOT) {
            Serial.println("[TAB] Settings Back");
            settingsGoBack();
            return true;
        }
        int i = (x < TOP_TAB_W) ? 0 : 1;
        ScreenId want = topTabScreen(i);
        Serial.printf("[TAB] top -> %s\n", tabLabel(want));
        if (want == g_screen) return true;
        g_showHistory = false;
        show_screen(want);
        return true;
    }

    // Bottom nav: Contact | Msg Box | Peers
    if (y >= BOT_TOP) {
        ScreenId want = SCR_CONTACTS;
        if (x < BOT_CONTACT_W)
            want = SCR_CONTACTS;
        else if (x < BOT_CONTACT_W + BOT_MSGBOX_W)
            want = SCR_CHATS;
        else
            want = SCR_PEERS;
        Serial.printf("[TAB] bot -> %s\n", tabLabel(want));
        if (want == g_screen) return true;
        g_showHistory = false;
        show_screen(want);
        return true;
    }

    // Settings list touch
    if (g_screen == SCR_SETTINGS) {
        const int listTop = CONTENT_TOP + 2;
        const int maxVis = (CONTENT_BOTTOM - listTop - 4) / SET_ROW_H;
        if (y >= listTop && y < CONTENT_BOTTOM) {
            int row = (y - listTop) / SET_ROW_H;
            if (row < 0) row = 0;
            int idx = g_settingsScroll + row;
            if (idx > 8) idx = 8;
            g_settingsSel = idx;
            Serial.printf("[SET] touch sel=%d\n", g_settingsSel);
            settingsActivate();
            return true;
        }
        return false;
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
        // N1: on Msg Box, open thread for this peer
        if (g_screen == SCR_CHATS) {
            g_showHistory = true;
            loadHistoryRows();
            show_screen(SCR_CHATS);
            Serial.println("[PEER] Msg Box → thread");
            return true;
        }
        if (g_selPeer != was) {
            show_screen(g_screen);  // only full refresh when selection changes
            return true;
        }
        Serial.println("[PEER] selected — type message then Enter");
        return true;  // no fullRefresh (preserves heap for TX)
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
            else if (idx == 1 || idx == 2) {
                Serial.printf("[FEAT] PATH/LINK status-only paths=%s links=%s\n",
                              g_pathsStr, g_linksStr);
            }
            else if (idx == 4) {
                g_featAudio = !g_featAudio;
                g_audio.setEnabled(g_featAudio);
                Serial.printf("[FEAT] AUDIO %s\n", g_featAudio?"ON":"OFF");
                if (g_featAudio) notifyInbound();
            }
            else if (idx == 5) {
                Serial.printf("[FEAT] LXMFQ=%s (status)\n", g_lxmfqStr);
            }
            // PEERS tile (3) status only
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
    r.saved = false;
    memcpy(r.peer, v.peer, 16);
    r.unread = v.unreadCount;
    formatPeerLine(r.line1, sizeof(r.line1), r.peer, r.unread);
    {
        char prev[28] = "";
        size_t pl = v.previewLength;
        if (pl >= sizeof(prev)) pl = sizeof(prev) - 1;
        if (pl > 0) {
            memcpy(prev, v.preview, pl);
            prev[pl] = '\0';
            for (size_t k = 0; k < pl; k++) {
                if ((unsigned char)prev[k] < 32 || (unsigned char)prev[k] > 126)
                    prev[k] = '?';
            }
        }
        formatPeerSub(r.line2, sizeof(r.line2), r.peer, prev[0] ? prev : nullptr);
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
    r.saved = false;
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
    for (int i = 0; i < HIST_ROWS; i++) {
        g_histRows[i].used = false;
        g_histRows[i].incoming = false;
        g_histRows[i].line[0] = '\0';
        g_histRows[i].st[0] = '\0';
    }

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
        h.incoming = entries[i].incoming;
        if (copy)
            snprintf(h.line, sizeof(h.line), "%s %s", arrow, snip);
        else
            snprintf(h.line, sizeof(h.line), "%s#%u", arrow, (unsigned)entries[i].counter);
        // N2: coarse status from stored LXMF status byte (outbound only)
        h.st[0] = '\0';
        if (!entries[i].incoming) {
            // LXMFStatus-ish: 0 queued 1 sending 2 sent 3 delivered 4 failed …
            switch (hdr.status) {
                case 0: strcpy(h.st, "queued"); break;
                case 1: strcpy(h.st, "send"); break;
                case 2: strcpy(h.st, "sent"); break;
                case 3: strcpy(h.st, "deliv"); break;
                case 4: strcpy(h.st, "fail"); break;
                default: snprintf(h.st, sizeof(h.st), "s%u", (unsigned)hdr.status); break;
            }
        }
        Serial.printf("[HIST] %s#%u \"%s\" st=%s tlen=%u clen=%u\n",
                      arrow, (unsigned)entries[i].counter, snip, h.st,
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
        r.saved = false;
        memcpy(r.peer, node.hash.data(), 16);
        r.unread = 0;
        formatPeerLine(r.line1, sizeof(r.line1), r.peer, 0);
        {
            char hex[33];
            peerToHex32(r.peer, hex);
            if (const char* loc = getLocalName(r.peer)) {
                snprintf(r.line1, sizeof(r.line1), "%.20s", loc);
                r.saved = true;
            } else {
                std::string cached = g_announceMgr.lookupName(hex);
                if (!cached.empty())
                    snprintf(r.line1, sizeof(r.line1), "%.20s", cached.c_str());
            }
            if (node.rssi != 0)
                snprintf(r.line2, sizeof(r.line2), "ID:%.12s %ddBm", hex, node.rssi);
            else
                snprintf(r.line2, sizeof(r.line2), "ID: %.16s", hex);
        }
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
    for (int i = 0; i < MSG_ROWS; i++) { g_msgRows[i].used = false; g_msgRows[i].saved = false; }

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
            r.saved = false;
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
    // Dark theme: double-draw +1px for a slightly bolder white glyph
    const int passes = g_darkTheme ? 2 : 1;
    for (int k = 0; k < passes; k++) {
        lv_obj_t* o = lv_label_create(parent);
        lv_label_set_text(o, txt ? txt : "");
        lv_obj_set_style_text_font(o, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(o, theme_fg(), 0);
        lv_obj_set_pos(o, x + k, y);
    }
}

void clear_screen() {
    g_display.fillScreen(g_darkTheme);
    if (g_root) {
        lv_obj_clean(g_root);
        lv_obj_set_style_bg_color(g_root, theme_bg(), 0);
        lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    } else {
        g_root = lv_scr_act();
        lv_obj_set_style_bg_color(g_root, theme_bg(), 0);
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
        lv_obj_set_style_text_color(lab, theme_fg(), 0);
        lv_obj_set_pos(lab, lx + k, ly);
    }
}

void draw_splash() {
    // Early boot screen — black paper, white text (readable on e-ink)
    const bool saved = g_darkTheme;
    g_darkTheme = true;
    clear_screen();
    // Simple "logo" block
    lv_obj_t* logo = lv_obj_create(g_root);
    lv_obj_set_size(logo, 120, 40);
    lv_obj_set_pos(logo, (EPD_WIDTH - 120) / 2, 48);
    lv_obj_set_style_radius(logo, 0, 0);
    lv_obj_set_style_border_width(logo, 3, 0);
    lv_obj_set_style_border_color(logo, theme_fg(), 0);
    lv_obj_set_style_bg_color(logo, theme_bg(), 0);
    lv_obj_set_style_bg_opa(logo, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(logo, 0, 0);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_SCROLLABLE);
    draw_label_centered((EPD_WIDTH - 120) / 2, 48, 120, 40, "RatSpeak", true);

    fat_label(g_root, "T-Deck Pro  V1.1", 40, 110);
    fat_label(g_root, "RatSpeak     V0.1", 40, 140);
    fat_label(g_root, "BOOTING...", 70, 200);
    LvglPort::updateAndRefresh();
    g_display.fullRefresh();
    Serial.println("[BOOT] splash drawn");
    g_darkTheme = saved;
}


void draw_tile(int x, int y, int w, int h, const char* title, const char* value) {
    lv_obj_t* box = lv_obj_create(g_root);
    lv_obj_set_size(box, w, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, theme_fg(), 0);
    lv_obj_set_style_bg_color(box, theme_bg(), 0);
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
    lv_obj_set_style_border_color(box, theme_fg(), 0);
    lv_obj_set_style_bg_color(box, theme_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    if (active) {
        lv_obj_t* under = lv_obj_create(g_root);
        lv_obj_set_size(under, w - 8, 3);
        lv_obj_set_pos(under, x + 4, y + h - 5);
        lv_obj_set_style_radius(under, 0, 0);
        lv_obj_set_style_border_width(under, 0, 0);
        lv_obj_set_style_bg_color(under, theme_fg(), 0);
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
        lv_obj_set_style_text_color(lab, theme_fg(), 0);
        lv_obj_set_pos(lab, lx, ly);
    }
    for (int k = 0; k < 2; k++) {
        lv_obj_t* lab = lv_label_create(g_root);
        lv_label_set_text(lab, one);
        lv_obj_set_style_text_font(lab, font, 0);
        lv_obj_set_style_text_color(lab, theme_fg(), 0);
        lv_obj_set_pos(lab, lx + (int)bw + k, ly);
    }
    if (after && after[0]) {
        for (int k = 0; k < (active ? 2 : 1); k++) {
            lv_obj_t* lab = lv_label_create(g_root);
            lv_label_set_text(lab, after);
            lv_obj_set_style_text_font(lab, font, 0);
            lv_obj_set_style_text_color(lab, theme_fg(), 0);
            lv_obj_set_pos(lab, lx + (int)bw + (int)cw + k, ly);
        }
    }
    // Underline tight under hotkey glyph
    lv_obj_t* bar = lv_obj_create(g_root);
    lv_obj_set_size(bar, (int)cw + 2, 2);
    lv_obj_set_pos(bar, lx + (int)bw, ly + 16);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, theme_fg(), 0);
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
    if (g_screen == SCR_CHATS && g_showHistory)
        draw_nav_box(x0, BOT_TOP, BOT_MSGBOX_W, BOT_H, "Back", true, 'B');
    else
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
    lv_obj_set_style_border_color(box, theme_fg(), 0);
    lv_obj_set_style_bg_color(box, theme_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    draw_label_centered(x, y, w, h, label, true);
}

// Plus-style settings/list row: title + subtitle + trailing ">"
void draw_menu_row(int y, int h, const char* title, const char* sub, bool selected) {
    // Selection = thick border only (same in both themes — no solid fill bar)
    lv_obj_t* row = lv_obj_create(g_root);
    lv_obj_set_size(row, EPD_WIDTH - 4, h);
    lv_obj_set_pos(row, 2, y);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(row, theme_fg(), 0);
    lv_obj_set_style_bg_color(row, theme_bg(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    const lv_font_t* font = &lv_font_montserrat_16;
    const bool hasSub = sub && sub[0];
    const int passes = g_darkTheme ? 2 : 1;
    for (int k = 0; k < passes; k++) {
        lv_obj_t* lab = lv_label_create(row);
        lv_label_set_text(lab, title ? title : "");
        lv_obj_set_style_text_font(lab, font, 0);
        lv_obj_set_style_text_color(lab, theme_fg(), 0);
        lv_obj_set_pos(lab, 6 + k, hasSub ? 4 : (h - 16) / 2);
    }
    if (hasSub) {
        for (int k = 0; k < passes; k++) {
            lv_obj_t* s = lv_label_create(row);
            lv_label_set_text(s, sub);
            lv_obj_set_style_text_font(s, font, 0);
            lv_obj_set_style_text_color(s, theme_fg(), 0);
            lv_obj_set_pos(s, 6 + k, 22);
        }
    }
}

void draw_stat_boxes(int y, unsigned conv, unsigned neu, unsigned unread) {
    const int gap = 2;
    const int bw = (EPD_WIDTH - 4 - 2 * gap) / 3;
    const int bh = STATS_H - 2;
    char a[14], b[14], c[14];
    snprintf(a, sizeof(a), "%02u CONV", conv);
    snprintf(b, sizeof(b), "%02u NEW", neu);
    snprintf(c, sizeof(c), "%02u UNRD", unread);
    const char* labs[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        int x = 2 + i * (bw + gap);
        lv_obj_t* box = lv_obj_create(g_root);
        lv_obj_set_size(box, bw, bh);
        lv_obj_set_pos(box, x, y);
        lv_obj_set_style_radius(box, 0, 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, theme_fg(), 0);
        lv_obj_set_style_bg_color(box, theme_bg(), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* lab = lv_label_create(box);
        lv_label_set_text(lab, labs[i]);
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lab, theme_fg(), 0);
        lv_coord_t tw = lv_txt_get_width(labs[i], strlen(labs[i]),
                                         &lv_font_montserrat_12, 0, LV_TEXT_FLAG_NONE);
        if (tw <= 0) tw = (lv_coord_t)(strlen(labs[i]) * 6);
        int lx = (bw - (int)tw) / 2;
        if (lx < 2) lx = 2;
        lv_obj_set_pos(lab, lx, (bh - 12) / 2);
    }
}

void draw_compose_or_hint(const char* idleHint) {
    // Single short line above bottom tabs — never draw into the nav band
    if (g_composeLen > 0 || g_composeMode) {
        char foot[36];
        if (g_nameEditMode)
            snprintf(foot, sizeof(foot), "Name>%.20s", g_compose);
        else
            snprintf(foot, sizeof(foot), ">%.24s", g_compose);
        fat_label(g_root, foot, 4, BOT_TOP - 22);
    } else if (idleHint && idleHint[0]) {
        fat_label(g_root, idleHint, 4, BOT_TOP - 22);
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
    lv_obj_set_style_border_color(body, theme_fg(), 0);
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
    lv_obj_set_style_border_color(tip, theme_fg(), 0);
    lv_obj_set_style_bg_color(tip, theme_fg(), 0);
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
        lv_obj_set_style_bg_color(fill, theme_fg(), 0);
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
    lv_obj_set_style_border_color(idBox, theme_fg(), 0);
    lv_obj_set_style_bg_color(idBox, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(idBox, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(idBox, 0, 0);
    lv_obj_clear_flag(idBox, LV_OBJ_FLAG_SCROLLABLE);

    fat_label(g_root, "IDENTITY", 6, y0 + 2);
    const char* on = (g_loraIf && g_loraIf->isOnline()) ? "ONLINE" : "OFFLINE";
    // ~30px left of prior 118; gauge ~15px left of prior far-right
    fat_label(g_root, on, 88, y0 + 2);
    int pct = 0;
    if (g_pctStr[0] && g_pctStr[0] != '-')
        pct = atoi(g_pctStr);
    draw_battery_gauge(EPD_WIDTH - 77, y0 + 3, 32, 14, pct);
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
    // N3: live path/link/RSSI (refreshLiveData already called from show_screen)
    char loraV[14], peerV[14], pathV[14];
    if (g_featLora && g_loraIf && g_loraIf->isOnline()) {
        // RSSI is negative dBm — still a valid reading (was wrongly treated as missing)
        if (g_rssiStr[0] && !(g_rssiStr[0] == '-' && g_rssiStr[1] == '\0'))
            snprintf(loraV, sizeof(loraV), "%s", g_rssiStr);
        else
            strcpy(loraV, "ON");
    } else {
        strcpy(loraV, "OFF");
    }
    // PEERS: announced nodes + stored conversations
    {
        int ann = g_announceMgr.nodeCount();
        unsigned conv = g_storeReady ? g_msgStore.totalConversations() : 0;
        snprintf(peerV, sizeof(peerV), "%d/%u", ann, conv);
    }
    // Reuse TCP tile for paths, WIFI for links when those features off (live mesh stats)
    snprintf(pathV, sizeof(pathV), "p%s", g_pathsStr);
    char linkV[14];
    snprintf(linkV, sizeof(linkV), "l%s", g_linksStr);
    draw_tile(gx,                gy,            tw, th, "LORA",  loraV);
    draw_tile(gx + tw + gap,     gy,            tw, th, "PATH",  pathV);
    draw_tile(gx + 2*(tw + gap), gy,            tw, th, "LINK",  linkV);
    draw_tile(gx,                gy + th + gap, tw, th, "PEERS", peerV);
    draw_tile(gx + tw + gap,     gy + th + gap, tw, th, "AUDIO", g_featAudio ? "ON" : "OFF");
    draw_tile(gx + 2*(tw + gap), gy + th + gap, tw, th, "LXMFQ", g_lxmfqStr);

    // Status + full-width Announce — packed under tiles
    int sy = gy + 2 * (th + gap) + 2;
    fat_label(g_root, "Listening for peers", 4, sy);
    char annAgo[28];
    snprintf(annAgo, sizeof(annAgo), "Announced: %s", g_announceStr);
    fat_label(g_root, annAgo, 4, sy + 16);
    draw_action_btn(0, sy + 34, EPD_WIDTH, HOME_ANN_H, "Announce");

    draw_compose_or_hint("");
}

void build_list_body(const char* /*section*/, const char* emptyNote,
                     bool withStats, unsigned conv, unsigned neu, unsigned unread) {
    for (int i = 0; i < g_msgRowCount; i++) {
        if (!g_msgRows[i].used) continue;
        formatPeerLine(g_msgRows[i].line1, sizeof(g_msgRows[i].line1),
                       g_msgRows[i].peer, g_msgRows[i].unread);
        if (!g_msgRows[i].line2[0])
            formatPeerSub(g_msgRows[i].line2, sizeof(g_msgRows[i].line2),
                          g_msgRows[i].peer, nullptr);
    }
    clear_screen();
    int y = CONTENT_TOP;
    if (withStats) {
        draw_stat_boxes(y, conv, neu, unread);
        y += STATS_H + 2;
    }
    if (g_msgRowCount == 0) {
        fat_label(g_root, emptyNote && emptyNote[0] ? emptyNote : "(empty)", 8, y + 4);
    } else {
        for (int i = 0; i < g_msgRowCount; i++) {
            char title[28], sub[28];
            if (i == g_selPeer)
                snprintf(title, sizeof(title), ">%d %s", i + 1, g_msgRows[i].line1);
            else
                snprintf(title, sizeof(title), " %d %s", i + 1, g_msgRows[i].line1);
            if (g_msgRows[i].line2[0])
                snprintf(sub, sizeof(sub), "%.26s", g_msgRows[i].line2);
            else
                sub[0] = 0;
            draw_menu_row(y, ROW_H, title, sub, i == g_selPeer);
            y += ROW_H;
            if (y > CONTENT_BOTTOM - 4) break;
        }
    }
}

// Plus-style outline bubbles: left=in, right=out; BOTH white bg / black text
static void draw_chat_bubble(int y, const char* text, bool incoming, const char* status) {
    const int margin = 4;
    const int maxW = EPD_WIDTH - 28;
    int tlen = (int)strlen(text ? text : "");
    int tw = tlen * 7 + 12;
    if (tw > maxW) tw = maxW;
    if (tw < 36) tw = 36;
    const int h = 24;
    int x = incoming ? margin : (EPD_WIDTH - margin - tw);

    lv_obj_t* box = lv_obj_create(g_root);
    lv_obj_set_size(box, tw, h);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, theme_fg(), 0);
    // Always match page bg so text (theme_fg) stays readable
    lv_obj_set_style_bg_color(box, theme_bg(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    const int passes = g_darkTheme ? 2 : 1;
    for (int k = 0; k < passes; k++) {
        lv_obj_t* lab = lv_label_create(box);
        lv_label_set_text(lab, text ? text : "");
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lab, theme_fg(), 0);
        lv_obj_set_pos(lab, 6 + k, 4);
    }

    if (status && status[0]) {
        int sx = x;
        if (sx + 40 > EPD_WIDTH - 4) sx = EPD_WIDTH - 44;
        fat_label(g_root, status, sx, y + h + 1);
    }
}

void build_chats() {
    // Plus-like thread: left = from peer, right = from us
    if (g_showHistory) {
        clear_screen();
        int y = CONTENT_TOP + 2;
        char hdr[40];
        if (g_msgRowCount > 0 && g_msgRows[g_selPeer].used)
            snprintf(hdr, sizeof(hdr), "%.20s", g_msgRows[g_selPeer].line1);
        else
            strcpy(hdr, "Chat");
        fat_label(g_root, hdr, 4, y);
        y += 20;
        if (g_txStr[0] && g_txStr[0] != '-') {
            char stline[36];
            snprintf(stline, sizeof(stline), "last: %s", g_txStr);
            fat_label(g_root, stline, 4, y);
            y += 18;
        }
        if (g_histRowCount == 0) {
            fat_label(g_root, g_histNote[0] ? g_histNote : "(no messages)", 4, y);
        } else {
            for (int i = 0; i < g_histRowCount; i++) {
                if (!g_histRows[i].used) continue;
                // Strip leading "< "/" > " from stored line for bubble text
                const char* raw = g_histRows[i].line;
                const char* body = raw;
                if ((raw[0] == '<' || raw[0] == '>') && raw[1] == ' ')
                    body = raw + 2;
                else if (raw[0] == '<' || raw[0] == '>')
                    body = raw + 1;
                bool incoming = g_histRows[i].incoming || raw[0] == '<';
                if (y + 28 > BOT_TOP - 28) break;
                draw_chat_bubble(y, body, incoming, incoming ? nullptr : g_histRows[i].st);
                y += (incoming || !g_histRows[i].st[0]) ? 28 : 40;
                if (y > CONTENT_BOTTOM - 30) break;
            }
        }
        draw_compose_or_hint(g_composeLen > 0 ? "" : "Enter=send");
        return;
    }
    loadMessageRows(false);
    unsigned conv = 0, unread = 0;
    if (g_storeReady) {
        conv = (unsigned)g_msgStore.totalConversations();
        unread = (unsigned)g_msgStore.totalUnreadCount();
    }
    build_list_body(nullptr, g_msgNote[0] ? g_msgNote : "(no chats)",
                    true, conv, unread, unread);
    draw_compose_or_hint("tap=open  type+Enter=send");
}

void loadContactsRows() {
    // Plus-like: TRUSTED (saved) then RECENT (announced, not saved)
    g_msgRowCount = 0;
    g_msgNote[0] = '\0';
    for (int i = 0; i < MSG_ROWS; i++) {
        g_msgRows[i].used = false;
        g_msgRows[i].saved = false;
    }
    const uint8_t* local = g_protoReady ? g_proto.localDestHash() : nullptr;
    const auto& nodes = g_announceMgr.nodes();

    auto addNode = [&](const DiscoveredNode& node, bool asSaved) {
        if (g_msgRowCount >= MSG_ROWS) return;
        if (node.hash.size() != 16) return;
        if (local && memcmp(node.hash.data(), local, 16) == 0) return;
        // dedupe
        for (int i = 0; i < g_msgRowCount; i++) {
            if (g_msgRows[i].used && memcmp(g_msgRows[i].peer, node.hash.data(), 16) == 0)
                return;
        }
        MsgRow& r = g_msgRows[g_msgRowCount];
        r.used = true;
        r.saved = asSaved;
        r.unread = 0;
        memcpy(r.peer, node.hash.data(), 16);
        char hex[33];
        peerToHex32(r.peer, hex);
        std::string nm;
        if (const char* loc = getLocalName(r.peer)) nm = loc;
        if (nm.empty()) nm = g_announceMgr.lookupName(hex);
        if (nm.empty() && !node.name.empty()) nm = node.name;
        // Only cache real names (not announce placeholders like "1")
        if (!nm.empty() && !(nm.size() <= 2 && nm.find_first_not_of("0123456789") == std::string::npos))
            setLocalName(r.peer, nm.c_str());
        formatPeerLine(r.line1, sizeof(r.line1), r.peer, 0);
        formatPeerSub(r.line2, sizeof(r.line2), r.peer, nullptr);
        // second line: full-ish id + age
        unsigned long ageMs = (node.lastSeen > 0 && millis() >= node.lastSeen)
                                  ? (millis() - node.lastSeen) : 0;
        unsigned ageM = (unsigned)(ageMs / 60000UL);
        if (asSaved)
            snprintf(r.line2, sizeof(r.line2), "ID: %.14s", hex);
        else if (ageM > 0)
            snprintf(r.line2, sizeof(r.line2), "ID: %.10s  %um", hex, ageM);
        else
            snprintf(r.line2, sizeof(r.line2), "ID: %.14s", hex);
        g_msgRowCount++;
    };

    // Pass 1: trusted / saved
    for (const auto& node : nodes) {
        if (node.saved) addNode(node, true);
    }
    // Also: name-cache entries that might not be in nodes yet
    // (skip — nodes carry saved flag after saveNode)

    // Pass 2: recent peers not saved
    for (const auto& node : nodes) {
        if (!node.saved) addNode(node, false);
    }
    clampSelPeer();
}

void build_contacts() {
    // Plus Contacts: TRUSTED CONTACTS + RECENT PEERS with names
    g_showHistory = false;
    g_msgRowsLoaded = false;
    loadContactsRows();
    for (int i = 0; i < g_msgRowCount; i++) {
        if (!g_msgRows[i].used) continue;
        formatPeerLine(g_msgRows[i].line1, sizeof(g_msgRows[i].line1),
                       g_msgRows[i].peer, 0);
        formatPeerSub(g_msgRows[i].line2, sizeof(g_msgRows[i].line2),
                      g_msgRows[i].peer, nullptr);
    }

    clear_screen();
    int y = CONTENT_TOP + 2;

    if (g_nameEditMode) {
        fat_label(g_root, "Set contact name", 4, y);
        y += 20;
        if (g_msgRowCount > 0 && g_msgRows[g_selPeer].used) {
            char hex[33];
            peerToHex32(g_msgRows[g_selPeer].peer, hex);
            char sub[40];
            snprintf(sub, sizeof(sub), "%.20s", g_msgRows[g_selPeer].line1);
            fat_label(g_root, sub, 4, y);
            y += 18;
            snprintf(sub, sizeof(sub), "ID: %.16s", hex);
            fat_label(g_root, sub, 4, y);
            y += 22;
        }
        fat_label(g_root, "Type name, Enter=save", 4, y);
        y += 20;
        if (g_composeLen > 0) {
            char shown[40];
            snprintf(shown, sizeof(shown), "Name: %s", g_compose);
            fat_label(g_root, shown, 4, y);
        } else {
            fat_label(g_root, "Name: _", 4, y);
        }
        draw_compose_or_hint("Enter=save  ^=cancel");
        return;
    }

    int trusted = 0, recent = 0;
    for (int i = 0; i < g_msgRowCount; i++) {
        if (!g_msgRows[i].used) continue;
        if (g_msgRows[i].saved) trusted++;
        else recent++;
    }

    char hdr[28];
    snprintf(hdr, sizeof(hdr), "TRUSTED (%d)", trusted);
    fat_label(g_root, hdr, 4, y);
    y += 18;

    bool drewTrusted = false;
    for (int i = 0; i < g_msgRowCount; i++) {
        if (!g_msgRows[i].used || !g_msgRows[i].saved) continue;
        drewTrusted = true;
        char title[28];
        if (i == g_selPeer)
            snprintf(title, sizeof(title), ">%d %s", i + 1, g_msgRows[i].line1);
        else
            snprintf(title, sizeof(title), " %d %s", i + 1, g_msgRows[i].line1);
        draw_menu_row(y, ROW_H, title, g_msgRows[i].line2, i == g_selPeer);
        y += ROW_H;
        if (y > CONTENT_BOTTOM - 60) break;
    }
    if (!drewTrusted) {
        fat_label(g_root, "(none — save a peer)", 8, y);
        y += 20;
    }

    y += 4;
    snprintf(hdr, sizeof(hdr), "RECENT PEERS (%d)", recent);
    fat_label(g_root, hdr, 4, y);
    y += 18;

    bool drewRecent = false;
    for (int i = 0; i < g_msgRowCount; i++) {
        if (!g_msgRows[i].used || g_msgRows[i].saved) continue;
        drewRecent = true;
        char title[28];
        if (i == g_selPeer)
            snprintf(title, sizeof(title), ">%d %s", i + 1, g_msgRows[i].line1);
        else
            snprintf(title, sizeof(title), " %d %s", i + 1, g_msgRows[i].line1);
        draw_menu_row(y, ROW_H, title, g_msgRows[i].line2, i == g_selPeer);
        y += ROW_H;
        if (y > CONTENT_BOTTOM - 8) break;
    }
    if (!drewRecent) {
        fat_label(g_root, "(none — Announce)", 8, y);
    }

    draw_compose_or_hint("1-9  Enter=name/msg");
}

void build_peers() {
    g_lastPeerCount = g_announceMgr.nodes().size();
    g_peerDirty = false;
    // Plus Peers: RECENT PEERS section
    g_showHistory = false;
    g_msgRowsLoaded = false;
    g_msgRowCount = 0;
    g_msgNote[0] = '\0';
    for (int i = 0; i < MSG_ROWS; i++) { g_msgRows[i].used = false; g_msgRows[i].saved = false; }
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
            formatPeerLine(g_msgRows[i].line1, sizeof(g_msgRows[i].line1),
                           g_msgRows[i].peer, 0);
            formatPeerSub(g_msgRows[i].line2, sizeof(g_msgRows[i].line2),
                          g_msgRows[i].peer, nullptr);
            char title[28], sub[36];
            if (i == g_selPeer)
                snprintf(title, sizeof(title), ">%d %s", i + 1, g_msgRows[i].line1);
            else
                snprintf(title, sizeof(title), " %d %s", i + 1, g_msgRows[i].line1);
            if (g_msgRows[i].line2[0])
                snprintf(sub, sizeof(sub), "%.28s", g_msgRows[i].line2);
            else {
                char hex[33];
                peerToHex32(g_msgRows[i].peer, hex);
                snprintf(sub, sizeof(sub), "%.12s…", hex);
            }
            draw_menu_row(y, ROW_H, title, sub, i == g_selPeer);
            y += ROW_H;
            if (y > CONTENT_BOTTOM - 8) break;
        }
    }
    draw_compose_or_hint("1-9 select  type+Enter=send");
}

void settingsGoBack() {
    if (g_settingsPage != SET_ROOT) {
        g_settingsPage = SET_ROOT;
        g_settingsSel = 0;
        show_screen(SCR_SETTINGS);
    }
}

void settingsActivate() {
    // Activate highlighted row on current settings page
    if (g_settingsPage == SET_ROOT) {
        static const SettingsPage kMap[] = {
            SET_IDENTITY, SET_SCREEN, SET_BATTERY, SET_LORA,
            SET_NETWORK, SET_TIME, SET_AUDIO, SET_DIAG, SET_STORAGE
        };
        if (g_settingsSel >= 0 && g_settingsSel < 9) {
            g_settingsPage = kMap[g_settingsSel];
            g_settingsSel = 0;
            show_screen(SCR_SETTINGS);
        }
        return;
    }
    // Sub-page actions
    if (g_settingsPage == SET_AUDIO) {
        if (g_settingsSel == 0) {
            g_featAudio = !g_featAudio;
            g_audio.setEnabled(g_featAudio);
            Serial.printf("[FEAT] AUDIO %s\n", g_featAudio ? "ON" : "OFF");
            if (g_featAudio) notifyInbound();
            show_screen(SCR_SETTINGS);
        }
        return;
    }
    if (g_settingsPage == SET_SCREEN) {
        if (g_settingsSel == 1) {
            g_darkTheme = !g_darkTheme;
            Serial.printf("[THEME] %s\n", g_darkTheme ? "black-ink" : "white-paper");
            show_screen(SCR_SETTINGS);
        }
        return;
    }
    if (g_settingsPage == SET_LORA) {
        if (g_settingsSel == 0) {
            g_featLora = !g_featLora;
            Serial.printf("[FEAT] LORA %s\n", g_featLora ? "ON" : "OFF");
            show_screen(SCR_SETTINGS);
        } else if (g_settingsSel == 2) {
            applyLoraPreset((g_loraPreset + 1) % kLoraPresetCount);
            show_screen(SCR_SETTINGS);
        }
        return;
    }
    if (g_settingsPage == SET_STORAGE) {
        if (g_settingsSel == 5) {  // Send Announce
            doAnnounce();
            if (g_featAudio) g_audio.playAnnounce();
            show_screen(SCR_SETTINGS);
        }
        return;
    }
}

void build_settings() {
    // Plus Settings hierarchy — live data where available; stubs otherwise
    clear_screen();
    int y = CONTENT_TOP + 2;
    char a[36], b[36];

    // Visible window of settings rows (scroll with W/S or touch)
    const int listTop = y;
    const int maxVis = (CONTENT_BOTTOM - listTop - 24) / SET_ROW_H;
    auto row = [&](const char* title, const char* sub, int idx) {
        // Only draw rows in the visible scroll window
        if (idx < g_settingsScroll) return;
        if (idx >= g_settingsScroll + maxVis) return;
        int yy = listTop + (idx - g_settingsScroll) * SET_ROW_H;
        if (yy + SET_ROW_H > CONTENT_BOTTOM - 2) return;
        draw_menu_row(yy, SET_ROW_H, title, sub, idx == g_settingsSel);
    };

    if (g_settingsPage == SET_ROOT) {
        // no "SETTINGS" title — top tab is enough; more room for rows
        snprintf(a, sizeof(a), "%.14s", g_destHex);
        row("1 Identity & Device", a, 0);
        row("2 Screen & Input", "e-ink / keys", 1);
        snprintf(a, sizeof(a), "%s %s", g_batStr, g_pctStr);
        row("3 Battery", a, 2);
        snprintf(a, sizeof(a), "LF %.0fMHz %s", LORA_DEFAULT_FREQ / 1e6f,
                 g_featLora ? "ON" : "OFF");
        row("4 LoRa", a, 3);
        row("5 Network", "WiFi/TCP stub", 4);
        row("6 Time & Location", "stub", 5);
        snprintf(a, sizeof(a), "Vol %s", g_featAudio ? "ON" : "OFF");
        row("7 Alerts & Audio", a, 6);
        snprintf(a, sizeof(a), "P%s L%s", g_pathsStr, g_linksStr);
        row("8 Diagnostics", a, 7);
        row("9 Storage & Maint.", "heap/flash", 8);
        if (g_settingsScroll > 0)
            fat_label(g_root, "^ more", EPD_WIDTH - 70, CONTENT_TOP + 2);
        if (g_settingsScroll + maxVis < 9)
            fat_label(g_root, "v more", EPD_WIDTH - 70, CONTENT_BOTTOM - 22);

        draw_compose_or_hint("^/W up  V down  1-9 Enter");
        return;
    }

    // Header with back
    const char* titles[] = {
        "SETTINGS", "Identity & Device", "Screen & Input", "Battery",
        "LoRa", "Network", "Time & Location", "Alerts & Audio",
        "Diagnostics", "Storage & Maint."
    };
    int ti = (int)g_settingsPage;
    if (ti < 0 || ti > 9) ti = 0;
    snprintf(a, sizeof(a), "< %s", titles[ti]);
    fat_label(g_root, a, 4, y); y += 18;

    if (g_settingsPage == SET_IDENTITY) {
        row("Firmware", "Pro G1.37", 0);
        snprintf(a, sizeof(a), "%.16s", g_destHex);
        row("LXMF Address", a, 1);
        snprintf(a, sizeof(a), "%.16s", g_destHex);
        row("Identity Hash", a, 2);
        row("Display Name", "(see announce)", 3);
        row("Identity Slot", "0 active", 4);
        row("Create Identity", "[reboot+erase]", 5);
        draw_compose_or_hint("Enter=Theme  ^=back");
        return;
    }
    if (g_settingsPage == SET_SCREEN) {
        row("Screen Bright", "e-ink N/A", 0);
        row("Theme", g_darkTheme ? "Black ink" : "White paper", 1);
        row("Dim After", "N/A", 2);
        row("Display Off", "N/A", 3);
        row("Key Backlight", "serial: L", 4);
        row("Keys Wake", "stub", 5);
        draw_compose_or_hint("L serial toggle KB light");
        return;
    }
    if (g_settingsPage == SET_BATTERY) {
        snprintf(a, sizeof(a), "%s", g_pctStr);
        row("Estimated Charge", a, 0);
        snprintf(a, sizeof(a), "%s", g_batStr);
        row("Battery Volts", a, 1);
        row("Battery Display", "Bar+%", 2);
        draw_compose_or_hint("^=back");
        return;
    }
    if (g_settingsPage == SET_LORA) {
        row("LoRa Radio", g_featLora ? "ON" : "OFF", 0);
        snprintf(a, sizeof(a), "Americas (%.0f)", LORA_DEFAULT_FREQ / 1e6f);
        row("Region Preset", a, 1);
        row("Link Preset", kLoraPresets[g_loraPreset].name, 2);
        snprintf(a, sizeof(a), "%s", g_rssiStr);
        row("Last RSSI", a, 3);
        row("Developer RF", "locked", 4);
        draw_compose_or_hint("0=radio 2=preset cycle  ^back");
        return;
    }
    if (g_settingsPage == SET_NETWORK) {
        row("WiFi Mode", "stub", 0);
        row("Scan Networks", "[future]", 1);
        row("WiFi SSID", "(not set)", 2);
        row("TCP Server", "None", 3);
        row("Port", "4242", 4);
        row("LAN Discovery", "OFF", 5);
        draw_compose_or_hint("WiFi/TCP future");
        return;
    }
    if (g_settingsPage == SET_TIME) {
        row("GPS Time Sync", "OFF", 0);
        row("GPS Location", "OFF", 1);
        row("Timezone", "local", 2);
        row("24-Hour Clock", "OFF", 3);
        draw_compose_or_hint("GPS future");
        return;
    }
    if (g_settingsPage == SET_AUDIO) {
        row("Audio Alerts", g_featAudio ? "ON" : "OFF", 0);
        row("Volume", "default", 1);
        row("Vibrate on msg", g_featAudio ? "ON" : "OFF", 2);
        draw_compose_or_hint("Enter toggles audio");
        return;
    }
    if (g_settingsPage == SET_DIAG) {
        row("RNS Transport", g_protoReady ? "ACTIVE" : "OFF", 0);
        snprintf(a, sizeof(a), "%s", g_pathsStr);
        row("Known Paths", a, 1);
        snprintf(a, sizeof(a), "%s", g_linksStr);
        row("Live Links", a, 2);
        snprintf(a, sizeof(a), "%s", g_rssiStr);
        row("LoRa / RSSI", a, 3);
        size_t hi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        snprintf(a, sizeof(a), "%u KB", (unsigned)(hi / 1024));
        row("Heap free", a, 4);
        size_t ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        snprintf(a, sizeof(a), "%u KB", (unsigned)(ps / 1024));
        row("PSRAM free", a, 5);
        snprintf(a, sizeof(a), "%lu s", (unsigned long)(millis() / 1000UL));
        row("Uptime", a, 6);
        draw_compose_or_hint("^=back");
        return;
    }
    if (g_settingsPage == SET_STORAGE) {
        size_t hi = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        snprintf(a, sizeof(a), "%u KB", (unsigned)(hi / 1024));
        row("Free Heap", a, 0);
        snprintf(a, sizeof(a), "%u KB", (unsigned)(ps / 1024));
        row("Free PSRAM", a, 1);
        row("Flash", "LittleFS", 2);
        row("SD Card", "optional", 3);
        row("SD Msg Store", "flash", 4);
        snprintf(a, sizeof(a), "Ann %s", g_announceStr);
        row("Send Announce", a, 5);
        row("Show QR Code", "[future]", 6);
        row("Reboot Now", "hold power", 7);
        draw_compose_or_hint("Enter on Announce to send");
        return;
    }
    draw_compose_or_hint("^=back");
}

void show_screen(ScreenId id) {
    if (id == SCR_SETTINGS && g_screen != SCR_SETTINGS) {
        // Entering Settings from another tab → root list
        g_settingsPage = SET_ROOT;
        g_settingsSel = 0;
    }
    if (id != SCR_SETTINGS) {
        // Leaving settings clears sub-page so next visit starts clean
        // (keep if only redrawing same settings)
    }
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
    if (freeInt < 16000) {
        // One log per 30s max; defer recheck 15s — avoids serial flood / apparent crash
        static uint32_t s_lastSkipLog = 0;
        uint32_t now = millis();
        if (s_lastSkipLog == 0 || (now - s_lastSkipLog) > 30000u) {
            s_lastSkipLog = now;
            Serial.printf("[UI] skip compose redraw low heap %u\n", (unsigned)freeInt);
        }
        g_lastMsgRedrawMs = now + 12000;  // next try ~15s later (check uses now-last)
        // Actually lastMsgRedrawMs should be in the past for millis()-last < MIN
        // Set so millis()-last is small: last = now works with MIN=2500 → 2.5s
        // Force longer: temporarily clear dirty? No — user needs footer eventually.
        g_lastMsgRedrawMs = now;
        g_lastComposeMs = now;  // reset idle timer so we wait COMPOSE_IDLE again
        return;
    }
    g_lastMsgRedrawMs = millis();
    Serial.printf("[UI] compose idle redraw len=%u\n", (unsigned)g_composeLen);
    show_screen(g_screen);
}

// Handle one keyboard event. Returns true if a full screen redraw is needed.
bool handleKeyEvent(const KeyEvent& ev) {
    // Keyboard Up = Back (lone Shift on Pro keyboard; also ev.up if set)
    if (ev.up) {
        Serial.println("[KEY] Up/Back");
        if (doBackAction()) return true;
        return false;
    }

    // Enter: send compose if non-empty, else next screen.
    if (ev.enter) {
        // Contacts name-edit: save typed name as trusted contact
        if (g_nameEditMode) {
            if (g_composeLen == 0) {
                Serial.println("[NAME] empty — type a name or ^=cancel");
                return false;
            }
            if (g_msgRowCount <= 0 || !g_msgRows[g_selPeer].used) {
                Serial.println("[NAME] no peer selected");
                g_nameEditMode = false;
                composeClear();
                return true;
            }
            char body[COMPOSE_CAP + 1];
            memcpy(body, g_compose, g_composeLen + 1);
            char hex[33];
            peerToHex32(g_msgRows[g_selPeer].peer, hex);
            bool ok = g_announceMgr.setContactName(std::string(hex), std::string(body));
            g_announceMgr.saveNode(std::string(hex));
            g_announceMgr.saveNameCache();
            setLocalName(g_msgRows[g_selPeer].peer, body);
            snprintf(g_msgRows[g_selPeer].line1, sizeof(g_msgRows[g_selPeer].line1),
                     "%.20s", body);
            g_msgRows[g_selPeer].saved = true;
            formatPeerSub(g_msgRows[g_selPeer].line2, sizeof(g_msgRows[g_selPeer].line2),
                          g_msgRows[g_selPeer].peer, nullptr);
            Serial.printf("[NAME] save %s -> \"%s\" ok=%d line1=\"%s\"\n",
                          hex, body, (int)ok, g_msgRows[g_selPeer].line1);
            g_nameEditMode = false;
            composeClear();
            g_composeMode = false;
            g_msgRowsLoaded = false;
            show_screen(SCR_CONTACTS);
            return true;
        }
        // Contacts: non-empty compose = contact name (never LXMF send)
        if (g_composeLen > 0 && (g_screen == SCR_CONTACTS || g_nameEditMode)) {
            if (g_msgRowCount <= 0 || !g_msgRows[g_selPeer].used) {
                Serial.println("[NAME] select a peer first");
                composeClear();
                g_nameEditMode = false;
                g_composeMode = false;
                return true;
            }
            char body[COMPOSE_CAP + 1];
            memcpy(body, g_compose, g_composeLen + 1);
            char hex[33];
            peerToHex32(g_msgRows[g_selPeer].peer, hex);
            bool ok = g_announceMgr.setContactName(std::string(hex), std::string(body));
            g_announceMgr.saveNode(std::string(hex));
            g_announceMgr.saveNameCache();
            setLocalName(g_msgRows[g_selPeer].peer, body);
            snprintf(g_msgRows[g_selPeer].line1, sizeof(g_msgRows[g_selPeer].line1),
                     "%.20s", body);
            g_msgRows[g_selPeer].saved = true;
            formatPeerSub(g_msgRows[g_selPeer].line2, sizeof(g_msgRows[g_selPeer].line2),
                          g_msgRows[g_selPeer].peer, nullptr);
            Serial.printf("[NAME] save %s -> \"%s\" ok=%d\n", hex, body, (int)ok);
            g_nameEditMode = false;
            composeClear();
            g_composeMode = false;
            g_msgRowsLoaded = false;
            show_screen(SCR_CONTACTS);
            return true;
        }
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
        // Empty Enter on Contacts: start name-edit for selected peer
        if (g_screen == SCR_CONTACTS) {
            if (g_msgRowCount > 0 && g_msgRows[g_selPeer].used) {
                g_nameEditMode = true;
                composeClear();
                g_composeMode = true;
                Serial.println("[NAME] edit mode — type name, Enter=save");
                show_screen(SCR_CONTACTS);
                return true;
            }
            Serial.println("[NAME] select a peer first");
            return false;
        }
        // Empty Enter on messaging screens: stay put
        if (g_screen == SCR_CHATS || g_screen == SCR_PEERS) {
            Serial.println("[KEY] Enter empty — type a message first");
            return false;
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

            // 1-9: select peer (never type digit while compose empty on msg screens)
            if (g_screen == SCR_SETTINGS && g_composeLen == 0) {
                // W/S move highlight (scroll into view). Lone Shift = Back.
                if (ev.character == 'w' || ev.character == 'W') {
                    if (g_settingsSel > 0) g_settingsSel--;
                    if (g_settingsSel < g_settingsScroll) g_settingsScroll = g_settingsSel;
                    Serial.printf("[SET] up sel=%d scroll=%d\n", g_settingsSel, g_settingsScroll);
                    show_screen(SCR_SETTINGS);
                    return true;
                }
                if (ev.character == 'v' || ev.character == 'V' ||
                    ev.character == 's' || ev.character == 'S') {
                    if (g_settingsSel < 8) g_settingsSel++;
                    const int maxVis = 5;
                    if (g_settingsSel >= g_settingsScroll + maxVis)
                        g_settingsScroll = g_settingsSel - maxVis + 1;
                    if (g_settingsScroll < 0) g_settingsScroll = 0;
                    Serial.printf("[SET] dn sel=%d scroll=%d\n", g_settingsSel, g_settingsScroll);
                    show_screen(SCR_SETTINGS);
                    return true;
                }
                if (ev.character >= '1' && ev.character <= '9') {
                    g_settingsSel = (int)(ev.character - '1');
                    if (g_settingsSel < g_settingsScroll) g_settingsScroll = g_settingsSel;
                    Serial.printf("[SET] key sel=%d\n", g_settingsSel);
                    if (g_settingsPage == SET_ROOT)
                        settingsActivate();
                    else
                        show_screen(SCR_SETTINGS);
                    return true;
                }
            }
            if (onMsgScreens && ev.character >= '1' && ev.character <= '9') {
                if (g_msgRowCount == 0) {
                    if (g_screen == SCR_PEERS) {
                        appendAnnouncedPeers();
                    } else {
                        loadMessageRows(true);
                    }
                }
                int idx = ev.character - '1';
                if (idx < g_msgRowCount && g_msgRows[idx].used) {
                    g_selPeer = idx;
                    char hex[33];
                    peerToHex32(g_msgRows[idx].peer, hex);
                    Serial.printf("[PEER] key %c -> sel=%d %s\n",
                                  ev.character, g_selPeer, hex);
                    Serial.println("[PEER] selected — type message then Enter");
                    // No fullRefresh on re-select; heap must stay free for TX
                    return false;
                }
                Serial.printf("[PEER] key %c ignored (rows=%d)\n",
                              ev.character, g_msgRowCount);
                return false;  // do not append digit into compose
            }

            // On Msg Box / Contact / Peers: letters always type (fix HELLO→Home)
            // Only V=thread N=next T=test remain as shortcuts there.
            if (onMsgScreens) {
                if (ev.character == 'b' || ev.character == 'B') {
                    if (g_showHistory) {
                        g_showHistory = false;
                        show_screen(SCR_CHATS);
                        Serial.println("[KEY] Back → Msg Box list");
                        return false;
                    }
                }
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
        // Contacts: typing starts name-edit (Enter saves name, never TX)
        if (g_screen == SCR_CONTACTS && !g_nameEditMode) {
            g_nameEditMode = true;
            g_composeMode = true;
            Serial.println("[NAME] typing → name-edit mode");
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
    Serial.println(" RATSPEAK  T-Deck Pro  Phase G1.37");
    Serial.println(" Settings hierarchy + name/ID");
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

    // CRITICAL: finish LXMF outbound recovery while heap is still ~40K+.
    // After LVGL/e-ink, free_int drops to ~12K and requestPending can stall,
    // leaving Rejection::Recovering forever (submit always rejected 8).
    if (g_protoReady) {
        Serial.printf("[BOOT] LXMF recovery drain free_int=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        for (int i = 0; i < 300; i++) {
            g_proto.loop();
            if (g_storeReady) g_msgStore.poll();
            delay(10);
        }
        // Probe: tiny submit attempt to self is invalid; instead try preferred
        // no-op by watching path — just log heap after drain.
        Serial.printf("[BOOT] recovery drain done free_int=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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
    draw_splash();

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
    Serial.println("[HINT] type+Enter=send | p=peer | V=thread | a=ann | F=formatFS");
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
        if (g_touchOk) {
            // CST328 present: only act on successful coordinate reads.
            // Failed reads used to call next_screen() → jump to Settings after peer tap.
            if (cst328Read(tx, ty))
                handleTouchAt(tx, ty);
            else
                Serial.println("[TOUCH] INT no-xy (ignored)");
        } else {
            // Soft-fail path when no touch IC: INT alone cycles screens
            Serial.println("[TOUCH] INT next (no CST328)");
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
            } else if ((p[0] == 'n' || p[0] == 'N') && p[1] == ':') {
                // n:DisplayName — name the selected peer (Msg Box/Peers/Contact)
                const char* nm = p + 2;
                while (*nm == ' ') nm++;
                if (!*nm) {
                    Serial.println("[NAME] usage: n:Alice");
                } else if (g_msgRowCount <= 0 || !g_msgRows[g_selPeer].used) {
                    Serial.println("[NAME] select a peer first (Peers/Msg Box)");
                } else {
                    char hex[33];
                    peerToHex32(g_msgRows[g_selPeer].peer, hex);
                    if (g_announceMgr.setContactName(std::string(hex), std::string(nm))) {
                        g_announceMgr.saveNameCache();
                        g_announceMgr.saveNode(std::string(hex));
                        setLocalName(g_msgRows[g_selPeer].peer, nm);
                        snprintf(g_msgRows[g_selPeer].line1, sizeof(g_msgRows[g_selPeer].line1),
                                 "%.20s", nm);
                        g_msgRows[g_selPeer].saved = true;
                        formatPeerSub(g_msgRows[g_selPeer].line2, sizeof(g_msgRows[g_selPeer].line2),
                                      g_msgRows[g_selPeer].peer, nullptr);
                        Serial.printf("[NAME] %s -> \"%s\" line1=\"%s\"\n",
                                      hex, nm, g_msgRows[g_selPeer].line1);
                        g_msgRowsLoaded = false;
                        show_screen(g_screen);
                    } else {
                        Serial.println("[NAME] setContactName failed");
                    }
                }
            } else if (p[0] == 'L' && p[1] == 0) {
                static bool s_lit = false;
                s_lit = !s_lit;
                if (s_lit) g_kb.backlightOn();
                else g_kb.backlightOff();
                Serial.printf("[KB] backlight %s\n", s_lit ? "ON" : "OFF");
            } else if (p[0] == 'F') {
                // Serial F — erase+format LittleFS (works even when !_ready)
                Serial.println("[FLASH] serial F recovery");
                if (recoverCorruptLittleFS()) {
                    Serial.println("[FLASH] OK — reboot now for clean identity");
                    strcpy(g_announceStr, "fmt ok");
                } else {
                    Serial.println("[FLASH] recovery failed");
                    strcpy(g_announceStr, "fmt fail");
                }
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
