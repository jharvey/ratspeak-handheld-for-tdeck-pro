#pragma once

#include "protocol/ProtocolBackend.h"
#include "protocol/RustAnnouncePolicy.h"
#include "protocol/RustClock.h"
#include "protocol/RustInterfacePump.h"
#include "protocol/RustKeyMap.h"
#include "protocol/RustLxmfEngine.h"
#include "protocol/RustPathResponseCache.h"
#include "protocol/RustRatchetStore.h"
#include "protocol/RustLinkManager.h"
#include "protocol/RustResourceEngine.h"
#include "ratspeak_protocol.h"

class FlashStore;
class SDStore;
class IdentityManager;
class MessageStore;
class AnnounceManager;

// Device-side coordinator for the Rust protocol core. C++ owns hardware,
// persistence adapters, scheduling, and UI integration; the FFI owns all
// Reticulum and LXMF wire parsing and construction.
class ProtocolRuntime : public ProtocolBackend, public RustPumpSink {
public:
    ~ProtocolRuntime() override;

    // Boot order: flash + identityMgr +
    // messageStore must be begun first. profile = RS_HANDHELD_PROFILE_* for
    // this board's artifact; nodeHeapCaps = heap_caps_malloc caps for the
    // transport node (SPIRAM on tdeck/tpager SMALL, INTERNAL on cardputer
    // MICRO). Alloc failure -> honest not-ready, never a crash. announceMgr is
    // the contact bridge target for validated inbound announces.
    bool begin(FlashStore* flash, SDStore* sd, IdentityManager* idMgr, MessageStore* store,
               AnnounceManager* announceMgr, int32_t profile, uint32_t nodeHeapCaps);
    void end();
    // Nonblocking message barrier. Lifecycle consumers keep pollReceive running
    // and consume their initial-send results while storage settles. end()
    // requires both incoming and outgoing ownership drained.
    void stopReceive();
    void pollReceive();
    bool receiveDrained() const { return _lxmf.incoming().drained() && _lxmf.drained(); }

    // Retains the context and the caller-owned radio until both message owners
    // and the already-started radio burst settle. No RX, scheduler or metadata
    // retries run here. The caller still polls MessageStore and result consumers.
    void beginMaintenance(LoRaInterface& radio);
    void pollMaintenance();
    bool maintenanceDrained() const;
    bool maintenanceFailed() const;

    RustInterfacePump& pump() { return _pump; }
    bool lifecycleReady() const { return _identityLoaded && _nodeOpen; }
    bool transportStats(rs_handheld_transport_stats_t& out) const;
    static const char* versionString() { return rs_handheld_rns_version(); }

    // The announce contact bridge target; created after begin() in the mains.
    void setAnnounceManager(AnnounceManager* m) { _announceMgr = m; }
    // Seed the cached announce app_data at boot so a path response sent BEFORE our
    // first announce still carries [name, stamp_cost, supported_functionality] —
    // an empty-app_data path response would overwrite our entry on peers (display
    // name lost) and revert Python LXMF to auto_compress=True (bz2 we must reject).
    void seedAnnounceAppData(const uint8_t* appData, size_t len);
    const uint8_t* localDestHash() const { return _destHash; }
    const RustInterfacePump::Counters& pumpCounters() const { return _pump.counters(); }

    // ProtocolBackend
    const char* backendName() const override { return "rust"; }

    void loop() override;
    bool pollRadioBeforeBlockingWork() override;
    // Shutdown / explicit persist: force the peer-ratchet table out (the ring is written
    // synchronously at rotation, so it is never pending here).
    bool persistData() override;

    String identityHash() const override { return _identityHashStr; }
    String identityHashHex() const override { return _identityLoaded ? _identityHashHex : String(); }
    String destinationHashHex() const override { return _destHashHex; }
    String destinationHashStr() const override { return _destHashStr; }

    bool isTransportActive() const override { return _nodeOpen; }
    size_t pathCount() const override;
    size_t linkCount() const override;

    AnnounceResult announce(const uint8_t* appData, size_t len) override;
    unsigned long lastAnnounceTime() const override { return _lastAnnounceMs; }
    uint32_t announceFilterCount() const override;

    handheld::outgoing::Submission lxmfSubmit(const uint8_t dest[16],
        const uint8_t* title, size_t titleLength, const uint8_t* content,
        size_t contentLength, bool preferLink = false) override;
    handheld::outgoing::Poll lxmfPoll(handheld::outgoing::Ticket,
        handheld::outgoing::InitialResult&) const override;
    bool lxmfAcknowledge(handheld::outgoing::Ticket) override;
    bool lxmfCancel(handheld::outgoing::Ticket) override;
    bool lxmfStatus(const handheld::storage::RecordKey&,
        handheld::outgoing::StatusView&) const override;
    uint32_t lxmfStatusRevision() const override;
    void lxmfStopAdmissions() override;
    bool lxmfDrained() const override;
    handheld::storage::Error lxmfDrainError() const override;
    bool lxmfBeginPeerDelete(const uint8_t peer[16]) override;
    void lxmfFinishPeerDelete(const uint8_t peer[16], const handheld::storage::Result&) override;
    int lxmfQueuedCount() const override { return _enginesUp ? _lxmf.queuedCount() : 0; }
    // Milliseconds until the next scheduled transmission (a throttled PATH_RESPONSE or a
    // deferred announce); UINT32_MAX when none. A sleeping device caps its sleep with this
    // so a peer's path request is answered on time instead of at the next wake-up.
    uint32_t msUntilScheduledTx() const;
    void setMessageCallback(LXMFManager::MessageCallback cb) override { _onMessage = cb; }
    void setStatusCallback(LXMFManager::StatusCallback cb) override { _statusCb = cb; }

    String publicKeyHex() const override { return _publicKeyHex; }

    // Honesty driver: true once identity loaded + node open + engines wired.
    bool protocolReady() const override { return _enginesUp; }

    uint8_t activeResourceTransfers() const override {
        return _enginesUp ? _resources.activeTransfers() : 0;
    }
    const char* deliveryBackendDetail() const override {
        return _enginesUp ? "opportunistic+link+resource"
                          : (lifecycleReady() ? "transport-only" : "not started");
    }

    // RustPumpSink
    void onAnnounceEvent(const rs_handheld_announce_event_t& ev, uint8_t ifaceId) override;
    void onLocalFrame(const rs_handheld_local_frame_t& f, uint8_t ifaceId) override;
    void onOwnPathRequest(uint8_t ifaceId, const uint8_t tag[16], size_t tagLen) override;

private:
    bool loadOrCreateIdentity(IdentityManager* idMgr);
    bool openTransport(int32_t profile, uint32_t nodeHeapCaps);
    void seedDedup(MessageStore* store);
    bool startEngines(FlashStore* flash, SDStore* sd, MessageStore* store,
                      AnnounceManager* announceMgr);
    // Build + TX a signed lxmf.delivery announce with the given wire context (CTX_NONE for a
    // normal announce, CTX_PATH_RESPONSE for a path-request answer). Refreshes _lastAnnounceMs.
    AnnounceResult emitAnnounce(const uint8_t* appData, size_t len, uint8_t context,
                                bool pathResponse);
    // Answer a pending path request: re-announce our dest as a PATH_RESPONSE with the cached name.
    void sendPathResponseAnnounce();

    rs_handheld_rns_t* _ctx = nullptr;
    uint8_t* _nodeBuf = nullptr;
    size_t _nodeBufLen = 0;
    int32_t _profile = RS_HANDHELD_PROFILE_SMALL;
    bool _identityLoaded = false;
    bool _nodeOpen = false;
    bool _enginesUp = false;
    enum AnnounceTiming : uint8_t { PathPending = 1, NormalPending = 2, HasPathResponseTime = 4 };
    uint8_t _announceTiming = 0; // Uses the existing alignment gap after lifecycle flags.
    LoRaInterface* _maintenanceRadio = nullptr;

    uint8_t _identityHash[16] = {};
    uint8_t _destHash[16] = {};
    uint8_t _publicKey[64] = {};
    String _identityHashStr = "unknown";  // xxxx:xxxx:xxxx (micro diag format)
    String _identityHashHex;
    String _destHashHex = "unknown";
    String _destHashStr = "unknown";
    String _publicKeyHex;
    unsigned long _lastAnnounceMs = 0;

    // Path-request self-response throttle (fix map §4). Layer 2: coalesce a burst of distinct-tag
    // retries into ONE answer after a grace window. Layer 3: cap the answer rate regardless of tag.
    // (Layer 1 — same-tag dedup — is handled inside the Rust node before the signal reaches us.)
    static constexpr uint32_t PATH_REQUEST_GRACE_MS = 400;   // Python PATH_REQUEST_GRACE (burst coalesce)
    static constexpr uint32_t PATH_RESP_DEDUP_MS = 5000;     // min interval between answers
    // Explicit flags distinguish an inactive timer from a valid wrapped time0.
    uint32_t _pathRespPendingUntil = 0;
    uint32_t _lastPathRespMs = 0;
    uint32_t _normalAnnouncePendingUntil = 0;
    size_t _normalAnnouncePendingLen = 0;
    uint8_t _pendingPathResponseTag[16] = {};
    size_t _pendingPathResponseTagLen = 0;
    uint8_t _pendingPathResponseIface = RustInterfacePump::LORA_IFACE_ID;
    // Current name/capability bytes, seeded before ingress and refreshed after
    // committed settings changes. Shared by new path responses and nonempty
    // deferred normal announces; already admitted signed packets are immutable.
    static constexpr size_t APP_DATA_MAX = 256;
    uint8_t _lastAppData[APP_DATA_MAX] = {};
    size_t _lastAppDataLen = 0;

    RustClock _clock;
    RustInterfacePump _pump;
    RustKeyMap _keymap;
    RustRatchetStore _ratchets;
    RustPathResponseCache _pathResponseCache;
    RustLinkManager _links;
    RustResourceEngine _resources;
    RustLxmfEngine _lxmf;
    AnnounceManager* _announceMgr = nullptr;
    LXMFManager::MessageCallback _onMessage;
    LXMFManager::StatusCallback _statusCb;
};
