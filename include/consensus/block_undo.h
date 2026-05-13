#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <optional>
#include "consensus/utxo_entry.h"
#include "consensus/utreexo_accumulator.h"  // Phase 3: For Hash256
#include "consensus/utreexo_delta.h"        // Phase 4: For delta-based undo
#include "consensus/utxo_snapshot_state.h"  // Phase 2: For snapshot-based undo

namespace dinero {
namespace consensus {

/**
 * Undo information for a single spent UTXO
 * Used to restore UTXOs during blockchain reorgs
 */
struct UndoEntry {
    uint256 txid;          // Transaction ID of spent UTXO (Phase M.0: uint256 identity)
    uint32_t vout;         // Output index of spent UTXO
    UTXOEntry coin;        // The coin that was spent (for restoration)

    UndoEntry() = default;
    UndoEntry(const uint256& tx, uint32_t v, const UTXOEntry& c)
        : txid(tx), vout(v), coin(c) {}
};

/**
 * Undo information for an entire block
 * Stores all UTXOs that were spent in this block
 * Required for safe blockchain reorgs
 */
struct BlockUndo {
    uint32_t height;                      // Block height
    uint256 block_hash;                   // Block hash (Phase M.0: uint256 identity)
    std::vector<UndoEntry> spent_coins;   // All coins spent in this block

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 4/6: Utreexo Accumulator Rollback (Delta-Based - Performance Optimized)
    // ═════════════════════════════════════════════════════════════════════════
    //
    // **History:**
    // - Phase 3: utreexo_forest_snapshot (full snapshot, ~2-10 KB per block)
    // - Phase 4: utreexo_delta (delta only, ~100-500 bytes per block)
    // - Phase 6: Removed snapshot field (migration complete)
    //
    // **Delta undo provides:**
    // - 10-20x size reduction (50-100x on large forests)
    // - Exact restoration (proven by Phase 4 tests)
    // - Correct reorg safety (proven by Phase 5 tests)
    //
    // **Size comparison:**
    // - Coinbase-only block: 48 bytes delta vs ~2 KB snapshot
    // - Typical block: ~768 bytes delta vs ~5 KB snapshot
    //
    // ═════════════════════════════════════════════════════════════════════════

    std::optional<UtreexoDelta> utreexo_delta;  // Phase 4: Delta-based undo (legacy)

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 2: Snapshot-Based Undo (Correctness First)
    // ═════════════════════════════════════════════════════════════════════════
    //
    // UTXOSnapshot captures the COMPLETE consensus state before block application.
    // DisconnectBlock becomes trivial: just restore the snapshot.
    //
    // **Trade-off:**
    // - Higher memory usage (full UTXO set copy per block)
    // - Guaranteed correctness (no partial state possible)
    //
    // **Future optimization (Phase 3):**
    // - Copy-on-Write with std::shared_ptr<const UTXOEntry>
    // - Delta snapshots (store only changes)
    //
    // ═════════════════════════════════════════════════════════════════════════

    std::optional<UTXOSnapshot> pre_block_snapshot;  // Phase 2: State before block

    // v7 shielded pool rollback support.
    // Serialized CommitmentTree frontier before the block connected.
    // DisconnectBlock restores this and rolls nullifiers back above height-1.
    std::optional<std::vector<uint8_t>> pre_block_shielded_frontier;

    BlockUndo() : height(0) {}
    explicit BlockUndo(uint32_t h, const uint256& hash = uint256())
        : height(h), block_hash(hash) {}

    // Add a spent UTXO to the undo data
    void AddSpentCoin(const uint256& txid, uint32_t vout, const UTXOEntry& coin) {
        spent_coins.emplace_back(txid, vout, coin);
    }
    
    // Get number of coins that were spent
    size_t GetSpentCount() const {
        return spent_coins.size();
    }
    
    // Check if undo data is empty
    bool IsEmpty() const {
        return spent_coins.empty();
    }
    
    // Serialize to JSON for storage
    std::string ToJson() const;
    
    // Deserialize from JSON
    static BlockUndo FromJson(const std::string& json_str);
    
    // Binary serialization (more efficient)
    std::vector<uint8_t> Serialize() const;
    static BlockUndo Deserialize(const std::vector<uint8_t>& data);
};

} // namespace consensus
} // namespace dinero
