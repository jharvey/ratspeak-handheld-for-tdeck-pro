#include <Arduino.h>
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
#include "runtime/TaskOwner.h"
#include "ratspeak_protocol.h"
#include "util/Bytes.h"

// ---------------------------------------------------------------------------
// Phase E chunk 10 — name labels + boot announce
// ---------------------------------------------------------------------------

static DisplayEink g_display;
static Keyboard g_kb;
static bool g_kbOk = false;

static FlashStore g_flash;
static IdentityManager g_idMgr;
static MessageStore g_msgStore;
static AnnounceManager g_announceMgr;
static ProtocolRuntime g_proto;
static bool g_storeReady = false;
static bool g_protoReady = false;

static SX1262* g_radio = nullptr;
static LoRaInterface* g_loraIf = nullptr;

enum ScreenId : uint8_t { SCR_HOME = 0, SCR_MESSAGES, SCR_SETTINGS, SCR_COUNT };
static ScreenId g_screen = SCR_HOME;
static lv_obj_t* g_root = nullptr;

static char g_batStr[20]    = "n/a";
static char g_pctStr[8]     = "--%";
static char g_destHex[17]   = "................";
static char g_loraStr[12]   = "off";
static char g_rssiStr[16]   = "-";
static char g_snrStr[12]    = "-";
static char g_pathsStr[8]   = "0";
static char g_linksStr[8]   = "0";
static char g_lxmfqStr[8]   = "0";
static char g_convStr[24]   = "0 conv";
static char g_unreadStr[16] = "0 unread";
static char g_announceStr[20] = "never";

static constexpr int MSG_ROWS = 4;
struct MsgRow {
    char line1[28];
    char line2[28];
    bool used;
};
static MsgRow g_msgRows[MSG_ROWS];
static int g_msgRowCount = 0;
static char g_msgNote[40] = "";
static bool g_msgRowsLoaded = false;
static uint32_t g_msgCacheRevision = 0;

static bool g_bootAnnDone = false;
static uint32_t g_bootAnnAt = 0;

static void spiBusIdle() {
    pinMode(EPD_CS, OUTPUT); digitalWrite(EPD_CS, HIGH);
    pinMode(LORA_CS, OUTPUT); digitalWrite(LORA_CS, HIGH);
#ifdef SD_CS
    pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH);
#endif
}

static bool readBattery(float& volts, int& pct) {
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

static bool fillDestFromNvs() {
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

static bool initRadio() {
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

static void logPsram() {
    size_t size = ESP.getPsramSize();
    size_t free = ESP.getFreePsram();
    bool found = psramFound();
    Serial.printf("[PSRAM] found=%d size=%u free=%u  caps_spiram=%u caps_int=%u\n",
                  (int)found, (unsigned)size, (unsigned)free,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static bool tryProto(uint32_t caps, const char* label) {
    size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    Serial.printf("[PROTO] try %s free_int=%u\n", label, (unsigned)freeInt);
    return g_proto.begin(&g_flash, nullptr, &g_idMgr, &g_msgStore,
                         &g_announceMgr, RS_HANDHELD_PROFILE_SMALL, caps);
}

static bool initStorageAndProto() {
    if (!g_flash.begin()) return false;
    g_idMgr.begin(&g_flash, nullptr);
    if (!g_msgStore.begin(&g_flash, nullptr, false)) return false;
    g_storeReady = true;
    g_announceMgr.setStorage(nullptr, &g_flash);

    logPsram();

    const size_t psSize = ESP.getPsramSize();
    const size_t psFree = ESP.getFreePsram();
    if (psramFound() && psSize > 0 && psFree > 250000) {
        if (tryProto(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, "SPIRAM|8BIT") ||
            tryProto(MALLOC_CAP_DEFAULT, "DEFAULT")) {
            g_protoReady = true;
        }
    }

    if (!g_protoReady) {
        if (tryProto(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "INTERNAL|8BIT") ||
            tryProto(MALLOC_CAP_INTERNAL, "INTERNAL") ||
            tryProto(MALLOC_CAP_8BIT, "8BIT")) {
            g_protoReady = true;
        }
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
        Serial.printf("[PROTO] up dest=%s free_int=%u\n",
                      g_destHex,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    } else {
        Serial.println("[PROTO] off — store-only");
        fillDestFromNvs();
    }
    return true;
}

static void doAnnounce() {
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
            Serial.println("[ANN] deferred (radio busy)");
            break;
        default:
            strcpy(g_announceStr, "failed");
            Serial.println("[ANN] failed");
            break;
    }
}

static bool waitResult(MessageStore::Ticket ticket, MessageStore::Result& res,
                       int maxSpin = 600) {
    for (int spin = 0; spin < maxSpin; spin++) {
        g_msgStore.poll();
        if (g_msgStore.peekResult(ticket, res)) return true;
        delay(5);
    }
    return false;
}

static void peerToHex32(const uint8_t peer[16], char out[33]) {
    for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", peer[i]);
    out[32] = '\0';
}

static void formatPeerLine(char* out, size_t outLen, const uint8_t peer[16],
                           uint32_t unread) {
    char hex[33];
    peerToHex32(peer, hex);
    std::string name = g_announceMgr.lookupName(hex);
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

static void applyView(const handheld::storage::ConversationView& v) {
    if (g_msgRowCount >= MSG_ROWS) return;
    MsgRow& r = g_msgRows[g_msgRowCount];
    r.used = true;
    formatPeerLine(r.line1, sizeof(r.line1), v.peer, v.unreadCount);
    size_t pl = v.previewLength;
    if (pl >= sizeof(r.line2)) pl = sizeof(r.line2) - 1;
    memcpy(r.line2, v.preview, pl);
    r.line2[pl] = '\0';
    for (size_t k = 0; k < pl; k++) {
        if ((unsigned char)r.line2[k] < 32 || (unsigned char)r.line2[k] > 126)
            r.line2[k] = '?';
    }
    g_msgRowCount++;
}

static void applySelectorOnly(const handheld::storage::ConversationSelector& s) {
    if (g_msgRowCount >= MSG_ROWS) return;
    MsgRow& r = g_msgRows[g_msgRowCount];
    r.used = true;
    formatPeerLine(r.line1, sizeof(r.line1), s.cursor.peer, 0);
    r.line2[0] = '\0';
    g_msgRowCount++;
}

static bool tryConversationPage(uint8_t limit) {
    using namespace handheld::storage;

    auto sub = g_msgStore.requestConversationPage(
        {}, false, ConversationOrder::Recent, ConversationDirection::After, limit);
    if (!sub.accepted()) {
        Serial.printf("[MSG] page rejected rejection=%u limit=%u free_int=%u\n",
                      (unsigned)sub.rejection, (unsigned)limit,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }

    Result res;
    if (!waitResult(sub.ticket, res)) {
        Serial.println("[MSG] page timeout");
        g_msgStore.releaseResult(sub.ticket);
        return false;
    }

    Serial.printf(
        "[MSG] page outcome=%u err=%u len=%u total=%u more=%d limit=%u free_int=%u\n",
        (unsigned)res.outcome, (unsigned)res.error,
        (unsigned)res.length, (unsigned)res.total, (int)res.more,
        (unsigned)limit,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

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
        Serial.printf("[MSG] sel[%u] ctr=%u in=%u err=%u peer=%02x%02x%02x%02x\n",
                      (unsigned)i, (unsigned)sel.counter, (unsigned)sel.incoming,
                      (unsigned)sel.error,
                      sel.cursor.peer[0], sel.cursor.peer[1],
                      sel.cursor.peer[2], sel.cursor.peer[3]);

        if (!sel.counter || sel.error != Error::None) {
            applySelectorOnly(sel);
            continue;
        }

        auto dsub = g_msgStore.requestConversation(sel);
        if (!dsub.accepted()) {
            Serial.printf("[MSG] detail rejected %u\n", (unsigned)dsub.rejection);
            applySelectorOnly(sel);
            continue;
        }

        Result dres;
        if (!waitResult(dsub.ticket, dres)) {
            Serial.println("[MSG] detail timeout");
            g_msgStore.releaseResult(dsub.ticket);
            applySelectorOnly(sel);
            continue;
        }
        Serial.printf("[MSG] detail outcome=%u err=%u len=%u\n",
                      (unsigned)dres.outcome, (unsigned)dres.error,
                      (unsigned)dres.length);

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

static void loadMessageRows(bool force = false) {
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

    if (!tryConversationPage(MSG_ROWS))
        tryConversationPage(1);

    if (g_msgRowCount == 0) {
        auto ids = g_msgStore.startupRecentMessageIds(MSG_ROWS);
        Serial.printf("[MSG] startup ids=%u convs=%u unread=%d\n",
                      (unsigned)ids.size(),
                      (unsigned)g_msgStore.totalConversations(),
                      g_msgStore.totalUnreadCount());
        for (size_t i = 0; i < ids.size() && g_msgRowCount < MSG_ROWS; i++) {
            MsgRow& r = g_msgRows[g_msgRowCount];
            r.used = true;
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

static void refreshLiveData() {
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

static void fat_label(lv_obj_t* parent, const char* txt, lv_coord_t x, lv_coord_t y) {
    lv_obj_t* o = lv_label_create(parent);
    lv_label_set_text(o, txt);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(o, lv_color_black(), 0);
    lv_obj_set_pos(o, x, y);
}

static void clear_screen() {
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

static void make_header(const char* title) {
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

static void make_footer(const char* text) {
    fat_label(g_root, text, 8, EPD_HEIGHT - 22);
}

static void add_row(int& y, const char* left, const char* right) {
    fat_label(g_root, left, 8, y);
    fat_label(g_root, right, 110, y);
    y += 28;
}

static void build_home() {
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

static void build_messages() {
    clear_screen();
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
    make_footer("2/3 Msgs  Enter/touch next");
}

static void build_settings() {
    clear_screen();
    make_header("Settings");
    int y = 40;
    char freq[20], txp[12];
    snprintf(freq, sizeof(freq), "%.1f MHz", LORA_DEFAULT_FREQ / 1e6f);
    snprintf(txp, sizeof(txp), "%d dBm", LORA_DEFAULT_TX_POWER);
    add_row(y, "Radio", "Long Fast");
    add_row(y, "Freq", freq);
    add_row(y, "TX power", txp);
    add_row(y, "WiFi", "off");
    add_row(y, "Display", "e-ink");
    add_row(y, "Announce", g_announceStr);
    y += 8;
    fat_label(g_root, g_protoReady ? "(protocol up)" : "(protocol off)", 8, y);
    y += 24;
    fat_label(g_root, "serial a = announce", 8, y);
    y += 24;
    char ps[40];
    snprintf(ps, sizeof(ps), "PSRAM %uK", (unsigned)(ESP.getPsramSize() / 1024));
    fat_label(g_root, ps, 8, y);
    make_footer("3/3 Setup Enter/touch next");
}

static void show_screen(ScreenId id) {
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
    Serial.printf("[UI] screen %u drawn\n", (unsigned)id);
}

static void next_screen() {
    show_screen((ScreenId)((g_screen + 1) % SCR_COUNT));
}

void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println("========================================");
    Serial.println(" RATSPEAK  T-Deck Pro  Phase E chunk 10");
    Serial.println(" names + boot announce");
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

    if (!g_display.begin()) { Serial.println("[BOOT] Display FAILED"); return; }
    Serial.println("[BOOT] display OK");
    if (!LvglPort::begin(g_display)) { Serial.println("[BOOT] LVGL FAILED"); return; }
    Serial.println("[BOOT] LVGL OK");

    g_kbOk = g_kb.begin();
    if (g_kbOk) Serial.println("[KEYBOARD] TCA8418 keyboard ready");

    show_screen(SCR_HOME);
    Serial.println("[BOOT] ready");
    g_bootAnnAt = millis() + 3000;
    Serial.println("[HINT] serial: a=announce r=reload msgs n=next");
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

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == 'n' || c == 'N' || c == '>') next_screen();
        else if (c == 'h' || c == 'H' || c == '1') show_screen(SCR_HOME);
        else if (c == 'm' || c == 'M' || c == '2') show_screen(SCR_MESSAGES);
        else if (c == 's' || c == 'S' || c == '3') show_screen(SCR_SETTINGS);
        else if (c == 'r' || c == 'R') {
            loadMessageRows(true);
            show_screen(SCR_MESSAGES);
        } else if (c == 'a' || c == 'A') {
            doAnnounce();
            if (g_screen == SCR_SETTINGS) show_screen(SCR_SETTINGS);
        }
    }
    delay(5);
}