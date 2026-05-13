#pragma once

#include "consensus/outpoint.h"  // Phase M.0: Single canonical OutPoint with hash function
#include "consensus/chain_state_view.h"  // Phase M.1: Abstract UTXO query interface
#include "consensus/utxo_entry.h"  // Phase M.1: Canonical UTXO type
#include "common/status.h"
#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <string>

namespace dinero {
// Phase M.0: OutPoint now defined in consensus/outpoint.h (uint256-based with std::hash specialization)

/**
 * CoinsViewMemPool - In-memory UTXO overlay for mempool validation
 *
 * Phase M.1: Refactored to use ChainStateView abstraction (not ChainDB directly)
 *
 * This is Bitcoin Core's CoinsViewMemPool equivalent:
 * - Pure in-memory (no RocksDB, no WriteBatch)
 * - Read overlay on top of ChainStateView (abstract UTXO interface)
 * - Tracks unconfirmed tx outputs (spent/created)
 * - Cleared when blocks are accepted
 *
 * SCOPE (where to use):
 *   ✅ Mempool standardness checks
 *   ✅ Double-spend detection
 *   ✅ RBF logic
 *   ✅ CPFP ancestor/descendant checks
 *   ✅ Package validation
 *
 * NEVER USE IN:
 *   ❌ ConnectBlock (consensus uses ChainDB directly)
 *   ❌ DisconnectBlock (consensus uses ChainDB directly)
 *   ❌ Wallet code (wallets observe ChainDB only)
 *   ❌ RPC mining (block templates use ChainDB)
 *
 * Lifecycle:
 *   1. Mempool validates tx → spendCoin/addCoin
 *   2. Block accepted → clear() (mempool + view)
 *   3. ChainStateView becomes authoritative again
 *
 * Bitcoin Core reference: src/txmempool.h (CTxMemPool coin tracking)
 */
class CoinsViewMemPool {
public:
    /**
     * Construct mempool view over ChainStateView base
     *
     * Phase M.1: Changed from ChainDB* to const ChainStateView*
     *
     * @param base - ChainStateView instance (non-owning, must outlive this object)
     */
    explicit CoinsViewMemPool(const consensus::ChainStateView* base);

    /**
     * Get coin with mempool overlay applied
     *
     * Phase M.1: Changed return type from std::optional<Coin> to StatusOr<UTXOEntry>
     *
     * Resolution order:
     *   1. If created by mempool tx → return mempool UTXO
     *   2. If spent by mempool tx → return Status::NotFound
     *   3. Else → return ChainStateView::getCoin()
     *
     * @param out - Outpoint to query
     * @return StatusOr<UTXOEntry> - UTXO if exists, NotFound if spent/missing
     */
    StatusOr<consensus::UTXOEntry> getCoin(const OutPoint& out) const;

    /**
     * Mark output as spent by mempool transaction
     *
     * Called when mempool tx spends an output.
     * Does NOT write to ChainDB - purely in-memory tracking.
     *
     * @param out - Outpoint being spent
     */
    void spendCoin(const OutPoint& out);

    /**
     * Add output created by mempool transaction
     *
     * Phase M.1: Changed parameter from const Coin& to const UTXOEntry&
     *
     * Called when mempool tx creates a new output.
     * Does NOT write to ChainDB - purely in-memory tracking.
     *
     * @param out - Outpoint being created
     * @param utxo - UTXOEntry data (value, scriptPubKey, height, isCoinbase)
     */
    void addCoin(const OutPoint& out, const consensus::UTXOEntry& utxo);

    /**
     * Clear all mempool state
     *
     * Called when:
     *   - Block is accepted (mempool reorganizes)
     *   - Daemon restarts
     *   - Manual flush
     *
     * After clear(), all queries fall through to ChainDB.
     */
    void clear();

    /**
     * Get number of spent outputs tracked
     * For testing/diagnostics only
     */
    size_t spentCount() const { return spent_.size(); }

    /**
     * Get number of created outputs tracked
     * For testing/diagnostics only
     */
    size_t createdCount() const { return created_.size(); }

    /**
     * Check whether a commitment exists in the overlay or base view.
     *
     * This is used by confidential mempool validation to prevent duplicate
     * commitments from re-entering the UTXO set.
     */
    bool hasCommitment(const std::vector<uint8_t>& commitment) const;

private:
    const consensus::ChainStateView* base_;  // Phase M.1: Non-owning pointer to ChainStateView (abstract UTXO query interface)

    std::unordered_set<OutPoint> spent_;                                // Outputs spent by mempool txs
    std::unordered_map<OutPoint, consensus::UTXOEntry> created_;  // Phase M.1: Outputs created by mempool txs (UTXOEntry)
};

} // namespace dinero
