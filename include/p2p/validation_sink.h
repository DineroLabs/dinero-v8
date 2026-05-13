/**
 * Phase G.3.1: Validation Sink Wiring
 *
 * Pure wiring layer: Downloaded payloads → Validation queue.
 *
 * Design Principles:
 * - NO validation logic (just queueing)
 * - NO disk writes
 * - NO mempool
 * - NO chainstate
 * - Simple FIFO queue
 * - Thread-safe (for future use)
 *
 * What This IS:
 * - Data routing (download → validation)
 * - Queue management
 * - FIFO ordering
 *
 * What This Is NOT:
 * - Validation
 * - Storage
 * - Consensus logic
 * - State mutation
 */

#pragma once

#include "download_coordinator.h"
#include <vector>
#include <mutex>
#include <deque>

namespace dinero {
namespace p2p {

//=============================================================================
// QueuedPayload: Payload waiting for validation
//=============================================================================

struct QueuedPayload {
    InventoryVector inv;
    std::vector<uint8_t> data;

    QueuedPayload() = default;
    QueuedPayload(const InventoryVector& i, const std::vector<uint8_t>& d)
        : inv(i), data(d) {}
};

//=============================================================================
// ValidationQueue: Simple FIFO queue for validation
//=============================================================================

class ValidationQueue {
public:
    ValidationQueue() = default;

    // Queue operations
    void enqueueBlock(const InventoryVector& inv, const std::vector<uint8_t>& data);
    void enqueueTx(const InventoryVector& inv, const std::vector<uint8_t>& data);

    // Drain operations (removes from queue)
    std::vector<QueuedPayload> drainBlocks();
    std::vector<QueuedPayload> drainTxs();

    // Query operations
    size_t blockCount() const;
    size_t txCount() const;

private:
    std::deque<QueuedPayload> block_queue_;
    std::deque<QueuedPayload> tx_queue_;

    mutable std::mutex mutex_;
};

//=============================================================================
// ValidationSink: Concrete IDownloadSink implementation
//=============================================================================

class ValidationSink : public IDownloadSink {
public:
    explicit ValidationSink(ValidationQueue& queue);

    // IDownloadSink interface
    void onBlock(const InventoryVector& inv, const std::vector<uint8_t>& data) override;
    void onTx(const InventoryVector& inv, const std::vector<uint8_t>& data) override;

private:
    ValidationQueue& queue_;
};

} // namespace p2p
} // namespace dinero
