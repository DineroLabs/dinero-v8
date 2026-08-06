#pragma once

#include "primitives/uint256.h"
#include "primitives/hash_domains.h"  // For TxId
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include <optional>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>
#include <mutex>

namespace dinero {
class ChainDB;
namespace consensus {
class UtreexoForest;
}
namespace indexing {

struct UTXOPositionRebuildReport {
    bool success = false;
    size_t matched = 0;
    size_t missing = 0;
    size_t malformed = 0;
    size_t skipped_unspendable = 0;
};

/**
 * Phase 11a: Global UTXO → Utreexo Position Index
 *
 * Maps ALL UTXOs (not just wallet-owned) to their Utreexo accumulator positions.
 * This index enables O(1) proof generation for any UTXO in the set.
 *
 * ARCHITECTURAL PROPERTIES:
 * ✅ Tracks ALL UTXOs (not wallet-specific)
 * ✅ Non-consensus (rebuildable from chain + forest)
 * ✅ Indexing-layer component (like txindex)
 * ✅ Does NOT affect consensus validity
 * ✅ Does NOT store coin data (amounts, scripts)
 * ✅ Required for proof serving RPCs
 *
 * USAGE:
 * - Updated during ConnectBlock/DisconnectBlock
 * - Queried by proof generation RPCs
 * - Can be rebuilt from chainstate + UtreexoForest
 *
 * REORG SAFETY:
 * - Maintains its own undo log (separate from consensus undo)
 * - Positions restored exactly during DisconnectBlock
 * - No recomputation needed
 */

/**
 * Undo entry for a single UTXO position change
 */
struct PositionUndoEntry {
    TxId txid;
    uint32_t vout;
    uint64_t position;

    PositionUndoEntry() = default;
    PositionUndoEntry(const TxId& tx, uint32_t v, uint64_t pos)
        : txid(tx), vout(v), position(pos) {}
};

/**
 * Undo data for all position changes in a block
 */
struct BlockPositionUndo {
    uint32_t height;
    std::vector<PositionUndoEntry> removed_positions;  // UTXOs spent in this block
    std::vector<PositionUndoEntry> added_positions;    // UTXOs created in this block

    BlockPositionUndo() : height(0) {}
    explicit BlockPositionUndo(uint32_t h) : height(h) {}

    bool IsEmpty() const {
        return removed_positions.empty() && added_positions.empty();
    }

    size_t GetTotalEntries() const {
        return removed_positions.size() + added_positions.size();
    }
};

/**
 * Global index mapping (txid, vout) → Utreexo position
 *
 * This index is:
 * - Required for proof serving
 * - Updated during block processing
 * - Rebuildable from chain state
 * - Thread-safe for concurrent queries
 */
class UTXOPositionIndex {
public:
    UTXOPositionIndex();
    ~UTXOPositionIndex();

    /**
     * Add a UTXO position mapping
     *
     * Called during ConnectBlock when a new UTXO is created.
     * The position is obtained from UtreexoForest.
     *
     * @param txid Transaction ID
     * @param vout Output index
     * @param position Utreexo accumulator position
     */
    void AddPosition(const TxId& txid, uint32_t vout, uint64_t position);

    /**
     * Remove a UTXO position mapping
     *
     * Called during ConnectBlock when a UTXO is spent.
     * Returns the position for undo logging.
     *
     * @param txid Transaction ID
     * @param vout Output index
     * @return The position that was removed, or nullopt if not found
     */
    std::optional<uint64_t> RemovePosition(const TxId& txid, uint32_t vout);

    /**
     * Look up the Utreexo position for a UTXO
     *
     * This is the PRIMARY API used by proof generation.
     *
     * @param txid Transaction ID
     * @param vout Output index
     * @return The Utreexo position, or nullopt if not tracked
     */
    std::optional<uint64_t> GetPosition(const TxId& txid, uint32_t vout) const;

    /**
     * Check if a UTXO position is tracked
     *
     * @param txid Transaction ID
     * @param vout Output index
     * @return true if position exists in index
     */
    bool HasPosition(const TxId& txid, uint32_t vout) const;

    /**
     * Get the total number of tracked positions
     *
     * For statistics and diagnostics.
     *
     * @return Number of UTXOs with tracked positions
     */
    size_t GetPositionCount() const;

    /**
     * Clear all position mappings
     *
     * Used during rebuild or reset operations.
     */
    void Clear();

    /**
     * Rebuild positions from chainstate and UtreexoForest
     *
     * This operation:
     * 1. Clears existing index
     * 2. Iterates all UTXOs in ChainDB
     * 3. Queries UtreexoForest for each position
     * 4. Repopulates the index
     *
     * Required for:
     * - Initial index creation
     * - Recovery from corruption
     * - Snapshot imports
     *
     * @param chain_db Canonical UTXO set
     * @param forest Reference to Utreexo forest
     * @return Detailed rebuild report
     */
    UTXOPositionRebuildReport Rebuild(const ChainDB& chain_db,
                                     const consensus::UtreexoForest& forest);

    /**
     * Rebuild positions from the active in-memory consensus UTXO set.
     *
     * This is the authoritative rebuild source after AssumeUTXO import:
     * snapshot coins live in ConsensusUTXOSet but are intentionally absent
     * from ChainDB until normal post-snapshot block processing persists
     * incremental changes. Rebuilding from ChainDB in that state silently
     * drops proof coverage for every snapshot coin.
     */
    UTXOPositionRebuildReport Rebuild(
        const std::unordered_map<OutPoint, consensus::UTXOEntry>& utxos,
        const consensus::UtreexoForest& forest);

private:
    // OutPoint hash function for unordered_map
    struct OutPointHash {
        size_t operator()(const std::pair<TxId, uint32_t>& outpoint) const {
            // Combine txid hash with vout
            size_t h1 = std::hash<uint256>{}(outpoint.first.AsUint256());
            size_t h2 = std::hash<uint32_t>{}(outpoint.second);
            return h1 ^ (h2 << 1);
        }
    };

    // Main index: (txid, vout) → position
    std::unordered_map<std::pair<TxId, uint32_t>, uint64_t, OutPointHash> position_map_;

    // Thread safety for concurrent read access
    mutable std::mutex index_mutex_;
};

} // namespace indexing
} // namespace dinero
