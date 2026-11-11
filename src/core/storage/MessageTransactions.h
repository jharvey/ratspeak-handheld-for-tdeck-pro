#pragma once

#include "FlashStore.h"
#include "SDStore.h"
#include "WriteQueue.h"
#include "MessageRecord.h"
#include "StorageLease.h"
#include "ConversationView.h"
#include <vector>
#include <string>
#include <utility>

namespace handheld::storage {

// Storage-only semantic executor. All record reads, revisions, counters and
// medium selection happen under one filesystem lease on the executor task.
class MessageTransactions final : public WriteQueue::Executor {
public:
    bool begin(FlashStore* flash, SDStore* sd, bool external, bool deferred);
    void execute(const Request&, uint8_t*, size_t length, size_t capacity, Result&) override;
    void setExternal(bool enabled) { _external = enabled; clearSummaries(); } // owner, quiescent only
    bool external() const { return _external; }

    struct Cursor {
        File directory;
        FlashStore::NameScan names;   // flash records: name-only scan (no per-file open)
        uint8_t peer[16] = {};
        uint32_t cutoff = 0;
        uint8_t medium = 0;
        bool opened = false;
        bool flashBackups = false;    // the flash peer directory holds a .bak record copy
        Error error = Error::None;
    };
    // Caller holds an explicit filesystem lease. No retained filename vector.
    void beginRecords(Cursor&, const uint8_t peer[16], MessageDocument* scratch = nullptr);
    bool nextRecord(Cursor&, RecordKey&);
    bool nextPeer(Cursor&, char peerHex[33]); // unordered unique peers, one pass per root
    bool nextPeer(const char* afterHex, char peerHex[33], Error&);
    Error load(const RecordKey&, MessageDocument&, StoredRecordHeader&);
    uint32_t deletedThrough(const uint8_t peer[16]);
    uint32_t nextCounter() const { return _nextCounter; }
    uint64_t mutationEpoch() const { return _mutationEpoch; } // caller holds lease
    using RecentIds = std::vector<std::pair<uint32_t, std::string>>;
    // Shared aggregate semantics. Optional bounded startup hints
    // run on the boot owner only; ordinary queries pass no heap-owning outputs.
    Error summarize(const uint8_t peer[16], ConversationView&, ConversationSelector&,
        RecentIds* recent = nullptr, size_t recentCapacity = 0, uint32_t* latestRevision = nullptr);
    // Boot metadata pass (owner, before the worker starts): while open, summarize() also
    // collects the pending outgoing keys and seeds the duplicate-ID index, so outgoing
    // recovery and the first incoming save do not re-read every record.
    void beginStartupScan();
    void endStartupScan(bool complete);
    int pendingCached();   // pending outgoing keys held, -1 when the cache is not valid
    // Where flash record reads spend their time (microseconds, cumulative since boot).
    struct ReadProfile { uint32_t opens = 0; uint64_t openUs = 0, parseUs = 0, closeUs = 0, listUs = 0; };
    static ReadProfile& readProfile() { static ReadProfile p; return p; }

private:
    struct Summary {
        ConversationView row;
        uint32_t counter = 0, revision = 0, used = 0;
        bool incoming = false;
    };
    static_assert(sizeof(Summary) == Budget::ConversationSummaryBytes, "Review summary retention budget");
    static constexpr size_t SummaryCapacity = Budget::conversationSummaryCapacity(WriteQueue::CompactProfile);
    void clearSummaries();
    void invalidateSummary(const uint8_t peer[16]);
    Summary* findSummary(const uint8_t peer[16]);
    void rememberSummary(const ConversationView&, const ConversationSelector&, uint32_t revision);
    void checkSummaryMedia();
    // Incoming message-ID index for the duplicate check on create. Without it every
    // incoming create loads every earlier incoming record of that peer. Entries are
    // (first 4 ID bytes << 32 | counter); a prefix hit is confirmed by loading that one
    // record, so the index only has to be complete, never exact. Built by the first
    // full scan, appended by later creates, dropped by any other kind of change.
    // Kept out of line (idIndexState()) so the inline executor stays within its byte budget.
    struct IdIndex {
        uint8_t peer[16] = {};
        std::vector<uint64_t> ids;
        uint32_t used = 0;
        bool valid = false, hasRecords = false;
    };
    static constexpr size_t IdIndexPeers = 4, IdIndexMax = 1024;   // <= 8 KB per conversation
    // Sorted (counter, peer) keys of outgoing records with status 1/2, complete only
    // while valid. Recovery's ReadPending answers from it with one load per query.
    static constexpr size_t PendingMax = 256;
    struct IdIndexState {
        const MessageTransactions* owner = nullptr;
        IdIndex slots[IdIndexPeers];
        uint32_t clock = 0;
        std::vector<RecordKey> pending;
        bool pendingValid = false, startup = false, pendingOverflow = false;
    };
    void dropPending();
    void notePending(const RecordKey&, bool isPending);
    IdIndexState& idIndexState();
    IdIndex* findIdIndex(const uint8_t peer[16], bool touch);
    void dropIdIndex(const uint8_t* peer);   // nullptr drops every peer
    void installIdIndex(const uint8_t peer[16], std::vector<uint64_t>&& ids, bool hasRecords);
    static uint64_t idEntry(const uint8_t id[32], uint32_t counter) {
        uint32_t prefix; memcpy(&prefix, id, 4); return (uint64_t(prefix) << 32) | counter;
    }
    struct Medium {
        FlashStore* flash = nullptr;
        SDStore* sd = nullptr;
        bool isReady() const { return flash ? flash->isReady() : sd && sd->isReady(); }
        bool exists(const char* path) { return flash ? flash->exists(path) : sd && sd->exists(path); }
        bool remove(const char* path) { return flash ? flash->remove(path) : sd && sd->remove(path); }
        bool rename(const char* from, const char* to) { return flash ? flash->rename(from, to) : sd && sd->rename(from, to); }
        bool ensureDir(const char* path) { return flash ? flash->ensureDir(path) : sd && sd->ensureDir(path); }
        File open(const char* path, const char* mode = "r") { return flash ? flash->openFile(path, mode) : sd ? sd->openFile(path, mode) : File(); }
        Error write(const char* path, const AtomicSource& source) { return flash ? flash->writeAtomic(path, source) : sd ? sd->writeAtomic(path, source) : Error::Unavailable; }
    };
    Medium medium(unsigned index) const { return index == 0 ? Medium{_flash, nullptr} : Medium{nullptr, _external ? _sd : nullptr}; }
    void path(const RecordKey&, unsigned medium, char output[128], const char* suffix = "") const;
    void directory(const uint8_t peer[16], unsigned medium, char output[96]) const;
    Error openDirectory(unsigned medium, const char* path, bool required, File&);
    bool nextFlashRecord(Cursor&, RecordKey&);
    bool nextFlashRecordTimed(Cursor&, RecordKey&);
    // load() for a key the cursor just produced, in the same lease: reuses the cursor's
    // delete cutoff and skips the flash .bak probe when the directory has none.
    Error load(const RecordKey&, MessageDocument&, StoredRecordHeader&, const Cursor* scan);
    static bool filename(const char*, uint32_t&, bool&);
    Error inspect(const RecordKey&, unsigned medium, const char* suffix, MessageDocument&, StoredRecordHeader&,
                  bool* parsed = nullptr,
                  bool* missing = nullptr);
    Error commit(const RecordKey&, MessageDocument&, bool creating, Result&);
    void create(const Request&, uint8_t*, Result&);
    void update(const Request&, Result&);
    void markRead(const Request&, Result&);
    void erase(const Request&, Result&);
    void read(const Request&, uint8_t*, size_t, Result&);
    void history(const Request&, uint8_t*, size_t, Result&);
    void pending(const Request&, uint8_t*, size_t, Result&);
    void conversationPage(const Request&, uint8_t*, size_t, Result&);
    void conversation(const Request&, uint8_t*, size_t, Result&);
    void trim(const Request&, Result&);
    bool reserveCounter(uint32_t&);
    bool retainedCopy(const RecordKey&, unsigned medium, MessageDocument&);
    bool initializeCounter();
    uint32_t deletedThrough(const uint8_t peer[16], MessageDocument&, uint8_t* markerState = nullptr,
                            Error* markerError = nullptr);
    FlashStore* _flash = nullptr;
    SDStore* _sd = nullptr;
    bool _external = false, _deferred = false;
    uint8_t _blockedMedia = 0, _preferBackup = 0;
    uint32_t _nextCounter = 1, _reservedThrough = 0;
    uint64_t _mutationEpoch = 1;
    uint32_t _summaryClock = 0;
    uint8_t _summaryMedia = 0;
    Summary _summaries[SummaryCapacity];
};

} // namespace handheld::storage
