#pragma once

#include "config/Config.h"
#include "storage/StorageContract.h"
#include <Arduino.h>
#include <atomic>

class WriteQueue {
public:
    using Request = handheld::storage::Request;
    using Result = handheld::storage::Result;
    using Ticket = handheld::storage::Ticket;
    using Submission = handheld::storage::Submission;
    using Budget = handheld::storage::Budget;
    enum class Execution : uint8_t { Immediate, Deferred };
    struct PayloadPart { const void* data; size_t length; };
    struct Executor {
        virtual ~Executor() = default;
        // Storage-only hook; never invoke application/protocol/UI callbacks or
        // retain these borrowed arguments. Request/result bytes belong to a slot.
        virtual void execute(const Request&, uint8_t*, size_t length,
                             size_t capacity, Result&) = 0;
    };
    explicit WriteQueue(uint64_t previousSequence = 0) : _nextSequence(previousSequence) {}
    ~WriteQueue();
    WriteQueue(const WriteQueue&) = delete;
    WriteQueue& operator=(const WriteQueue&) = delete;
    bool begin(Executor&, Execution);
    // Explicit setup-to-service handoff. The previous producer must be
    // quiescent; no admitted request/result may survive the owner transfer.
    bool adoptOwner();
    Submission submit(const Request&, const PayloadPart* parts = nullptr,
                      size_t partCount = 0, size_t resultCapacity = 0);
    bool peekResult(Ticket, Result&, Request* request = nullptr) const;
    bool readPayload(Ticket, void*, size_t length, size_t offset = 0) const;
    bool releaseResult(Ticket);
    bool cancel(Ticket); // queued only; retain credit until worker publishes Cancelled
    Ticket nextReady(uint64_t afterSequence = 0) const;
    void requestStop();
    bool finishStop(); // nonblocking: worker exited and owner consumed every result
    bool stopped() const { return _workerStopped.load(std::memory_order_acquire); }
    // Worker samples its own live handle. Never query a task which may already
    // have self-deleted; stopped/immediate/not-yet-sampled are unavailable.
    bool workerStackHighWater(uint32_t& bytes) const {
        const auto sampled = _workerStackFree.load(std::memory_order_acquire);
        if (stopped() || sampled == UINT32_MAX) return false;
        bytes = sampled;
        return true;
    }
    bool deferred() const { return _execution == Execution::Deferred; }
    bool accepting() const { assertOwner(); return _accepting; }
    int drainCount() const { assertOwner(); return _outstanding; }
    bool isFull() const;
    // Diagnostics: one character per slot (F free, Q queued, C cancel queued, R running,
    // D done and waiting for its owner to take the result, X retired).
    size_t describe(char* out, size_t capacity) const;
    size_t retainedBytes() const;

    static constexpr bool CompactProfile = STORAGE_ASYNC_WRITES != 0;
    static constexpr size_t PayloadBytes = CompactProfile ? Budget::CardPayloadBytes : Budget::LargeBoardPayloadBytes;

private:
    using State = handheld::storage::State;
    using Slot = handheld::storage::Slot;
    static constexpr uint8_t StopSlot = UINT8_MAX;
    static void taskFunc(void*);
    void executeSlot(uint8_t);
    uint8_t* payload(uint8_t);
    const uint8_t* payload(uint8_t) const;
    bool matches(Ticket) const;
    void assertOwner() const;
    size_t capacity(uint8_t slot) const { return Budget::payloadCapacity(CompactProfile, slot); }
    Slot _slots[Budget::SlotCount];
    uint8_t _payload[PayloadBytes] = {};
    uint16_t _lengths[Budget::SlotCount] = {};
    uint16_t _resultCapacities[Budget::SlotCount] = {};
    void* _queue = nullptr;
    void* _task = nullptr;
    void* _owner = nullptr;
    Executor* _executor = nullptr;
    uint64_t _nextSequence = 0;
    std::atomic<bool> _workerStopped{true};
    std::atomic<uint32_t> _workerStackFree{UINT32_MAX};
    uint8_t _outstanding = 0;
    Execution _execution = Execution::Immediate;
    bool _accepting = false;
    bool _stopRequested = false;
};
static_assert(sizeof(WriteQueue) <= WriteQueue::PayloadBytes + WriteQueue::Budget::SlotCeiling + 256,
              "Storage executor bookkeeping exceeds its fixed-byte envelope");
