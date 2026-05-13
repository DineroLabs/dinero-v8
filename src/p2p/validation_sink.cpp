/**
 * Phase G.3.1: Validation Sink Wiring Implementation
 *
 * Pure queueing logic - no validation.
 */

#include "../../include/p2p/validation_sink.h"

namespace dinero {
namespace p2p {

//=============================================================================
// ValidationQueue Implementation
//=============================================================================

void ValidationQueue::enqueueBlock(const InventoryVector& inv, const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    block_queue_.emplace_back(inv, data);
}

void ValidationQueue::enqueueTx(const InventoryVector& inv, const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    tx_queue_.emplace_back(inv, data);
}

std::vector<QueuedPayload> ValidationQueue::drainBlocks() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<QueuedPayload> result;
    result.reserve(block_queue_.size());

    while (!block_queue_.empty()) {
        result.push_back(std::move(block_queue_.front()));
        block_queue_.pop_front();
    }

    return result;
}

std::vector<QueuedPayload> ValidationQueue::drainTxs() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<QueuedPayload> result;
    result.reserve(tx_queue_.size());

    while (!tx_queue_.empty()) {
        result.push_back(std::move(tx_queue_.front()));
        tx_queue_.pop_front();
    }

    return result;
}

size_t ValidationQueue::blockCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return block_queue_.size();
}

size_t ValidationQueue::txCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tx_queue_.size();
}

//=============================================================================
// ValidationSink Implementation
//=============================================================================

ValidationSink::ValidationSink(ValidationQueue& queue)
    : queue_(queue)
{
}

void ValidationSink::onBlock(const InventoryVector& inv, const std::vector<uint8_t>& data) {
    // Pure wiring: just enqueue for validation
    queue_.enqueueBlock(inv, data);
}

void ValidationSink::onTx(const InventoryVector& inv, const std::vector<uint8_t>& data) {
    // Pure wiring: just enqueue for validation
    queue_.enqueueTx(inv, data);
}

} // namespace p2p
} // namespace dinero
