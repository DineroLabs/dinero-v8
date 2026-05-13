#include "mempool/coins_view_mempool.h"

namespace dinero {

CoinsViewMemPool::CoinsViewMemPool(const consensus::ChainStateView* base)
    : base_(base) {
    if (!base_) {
        throw std::invalid_argument("CoinsViewMemPool: base ChainStateView cannot be null");
    }
}

StatusOr<consensus::UTXOEntry> CoinsViewMemPool::getCoin(const OutPoint& out) const {
    // Resolution order (Bitcoin Core CoinsViewMemPool logic):
    //
    // Phase M.1: Updated to use ChainStateView and UTXOEntry
    //
    // 1. If mempool created this output → return mempool UTXO
    // 2. If mempool spent this output → return Status::NotFound (spent)
    // 3. Else → query ChainStateView (confirmed UTXOs)

    // Check if mempool created this output
    auto created_it = created_.find(out);
    if (created_it != created_.end()) {
        return created_it->second;
    }

    // Check if mempool spent this output
    if (spent_.count(out) > 0) {
        return Status::NotFound;  // Spent by mempool tx
    }

    // Fall through to ChainStateView (consensus UTXOs)
    return base_->getCoin(out);
}

void CoinsViewMemPool::spendCoin(const OutPoint& out) {
    // Mark output as spent by mempool transaction
    // This does NOT write to ChainDB - purely in-memory tracking

    // If mempool created this output, remove it from created set
    // (spend cancels out the create - output never existed)
    auto created_it = created_.find(out);
    if (created_it != created_.end()) {
        created_.erase(created_it);
        return;
    }

    // Otherwise, mark as spent (will block getCoin queries)
    spent_.insert(out);
}

void CoinsViewMemPool::addCoin(const OutPoint& out, const consensus::UTXOEntry& utxo) {
    // Add output created by mempool transaction
    // Phase M.1: Changed parameter from Coin to UTXOEntry
    // This does NOT write to ChainDB - purely in-memory tracking

    // Remove from spent set if it was previously marked spent
    // (shouldn't happen in normal flow, but defensive)
    spent_.erase(out);

    // Add to created map
    created_[out] = utxo;
}

void CoinsViewMemPool::clear() {
    // Clear all mempool state
    // Called when block is accepted or daemon restarts
    spent_.clear();
    created_.clear();
}

bool CoinsViewMemPool::hasCommitment(const std::vector<uint8_t>& commitment) const {
    if (commitment.empty()) {
        return false;
    }

    for (const auto& [outpoint, utxo] : created_) {
        (void)outpoint;
        if (utxo.is_confidential && utxo.commitment == commitment) {
            return true;
        }
    }
    return false;
}

} // namespace dinero
