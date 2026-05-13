#include "consensus/coins_db.h"
#include "primitives/transaction.h"
#include <sstream>

namespace dinero {
namespace consensus {

// ============================================================================
// CoinsViewCache Implementation
// ============================================================================

CoinsViewCache::CoinsViewCache(CoinsDB* base_db)
    : base_db_(base_db)
    , current_height_(0)
    , has_changes_(false)
{}

CoinsViewCache::~CoinsViewCache() {
    // Ensure cache is flushed or explicitly cleared
    // (In production, caller should handle this explicitly)
}

StatusOr<UTXOEntry> CoinsViewCache::getCoin(const OutPoint& outpoint) const {
    // Check if coin was spent in this cache
    if (spent_coins_.count(outpoint)) {
        return Status::NotFound;  // Spent in cache
    }

    // Check if coin was added in this cache
    auto added_it = added_coins_.find(outpoint);
    if (added_it != added_coins_.end()) {
        return added_it->second;  // Found in added coins
    }

    // Check cached coins (read cache)
    auto cached_it = cached_coins_.find(outpoint);
    if (cached_it != cached_coins_.end()) {
        return cached_it->second;  // Found in read cache
    }

    // Fall back to database
    if (!base_db_) {
        return Status::Internal;
    }

    auto db_result = base_db_->getCoin(outpoint);
    if (db_result.ok()) {
        // Cache the result for future lookups (mutable cache)
        cached_coins_[outpoint] = db_result.value();
    }

    return db_result;
}

bool CoinsViewCache::hasCoin(const OutPoint& outpoint) const {
    auto result = getCoin(outpoint);
    return result.ok();
}

uint32_t CoinsViewCache::getHeight() const {
    return current_height_;
}

void CoinsViewCache::setHeight(uint32_t height) {
    current_height_ = height;
}

void CoinsViewCache::addCoin(const OutPoint& outpoint, const UTXOEntry& coin) {
    // Remove from spent set if it was marked spent
    spent_coins_.erase(outpoint);

    // Add to added coins (or update if already added)
    added_coins_[outpoint] = coin;

    // Also add to read cache for immediate lookups
    cached_coins_[outpoint] = coin;

    has_changes_ = true;
}

StatusOr<UTXOEntry> CoinsViewCache::spendCoin(const OutPoint& outpoint) {
    // Get the coin first
    auto coin_result = getCoin(outpoint);
    if (!coin_result.ok()) {
        return coin_result.status();  // Coin doesn't exist
    }

    UTXOEntry coin = coin_result.value();

    // Check if coin was added in this cache
    auto added_it = added_coins_.find(outpoint);
    if (added_it != added_coins_.end()) {
        // Coin was added and spent in same cache - just remove it
        added_coins_.erase(added_it);
        cached_coins_.erase(outpoint);
    } else {
        // Coin exists in DB - mark as spent
        spent_coins_.insert(outpoint);
        cached_coins_.erase(outpoint);
    }

    has_changes_ = true;
    return coin;
}

Status CoinsViewCache::flush() {
    if (!has_changes_) {
        return Status::Ok;  // Nothing to flush
    }

    if (!base_db_) {
        return Status::Internal;
    }

    // Prepare batch write
    std::vector<std::pair<OutPoint, UTXOEntry>> coins_to_add;
    std::vector<OutPoint> coins_to_spend;

    // Collect added coins
    for (const auto& [outpoint, coin] : added_coins_) {
        coins_to_add.emplace_back(outpoint, coin);
    }

    // Collect spent coins
    for (const auto& outpoint : spent_coins_) {
        coins_to_spend.push_back(outpoint);
    }

    // Write batch to database
    Status status = base_db_->writeBatch(coins_to_add, coins_to_spend);
    if (status != Status::Ok) {
        return status;
    }

    // Clear cache after successful flush
    added_coins_.clear();
    spent_coins_.clear();
    cached_coins_.clear();  // Clear read cache to avoid stale data
    has_changes_ = false;

    return Status::Ok;
}

void CoinsViewCache::clear() {
    added_coins_.clear();
    spent_coins_.clear();
    cached_coins_.clear();
    has_changes_ = false;
}

size_t CoinsViewCache::getCacheSize() const {
    return added_coins_.size() + spent_coins_.size() + cached_coins_.size();
}

size_t CoinsViewCache::getAddedCount() const {
    return added_coins_.size();
}

size_t CoinsViewCache::getSpentCount() const {
    return spent_coins_.size();
}

// ============================================================================
// Phase 23.1.F: Apply Transaction to UTXO Set
// ============================================================================

bool CoinsViewCache::applyTransaction(
    const Transaction& tx,
    const uint256& txid,
    uint32_t height,
    bool is_coinbase,
    UndoCoins& undo
) {
    // Coinbase transactions have no inputs to spend (they create coins)
    if (!is_coinbase) {
        // Spend all inputs
        for (const auto& input : tx.vin) {
            OutPoint outpoint(input.prevout.txid, input.prevout.vout);

            // Get the coin before spending (for undo data)
            auto coin_result = spendCoin(outpoint);
            if (!coin_result.ok()) {
                // Input not found - this should never happen if validation passed
                return false;
            }

            // Save spent coin to undo data (for reorg)
            undo.addSpentCoin(outpoint, coin_result.value());
        }
    }

    // Add all outputs to UTXO set
    for (size_t vout = 0; vout < tx.vout.size(); vout++) {
        const auto& output = tx.vout[vout];

        // Create UTXO entry
        // Phase M.6.2: Pass AmountUna directly (no extraction needed)
        UTXOEntry coin(
            output.value,
            output.scriptPubKey,
            height,
            is_coinbase,
            output.is_confidential,
            output.commitment
        );

        // Add to UTXO set
        // Phase M.4.3-B: Wrap uint256 in TxId for OutPoint constructor
        OutPoint outpoint(TxId(txid), static_cast<uint32_t>(vout));
        addCoin(outpoint, coin);
    }

    return true;
}

bool CoinsViewCache::undoTransaction(
    const Transaction& tx,
    const uint256& txid,
    const UndoCoins& undo
) {
    // Remove all outputs created by this transaction
    for (size_t vout = 0; vout < tx.vout.size(); vout++) {
        // Phase M.4.3-B: Wrap uint256 in TxId for OutPoint constructor
        OutPoint outpoint(TxId(txid), static_cast<uint32_t>(vout));

        // Remove from UTXO set
        auto result = spendCoin(outpoint);
        if (!result.ok()) {
            // Output doesn't exist - this is okay if it was already spent
            // During reorg, some outputs might have been spent in later blocks
            continue;
        }
    }

    // Restore all spent inputs (from undo data)
    for (const auto& [outpoint, coin] : undo.spent_coins) {
        addCoin(outpoint, coin);
    }

    return true;
}

} // namespace consensus
} // namespace dinero
