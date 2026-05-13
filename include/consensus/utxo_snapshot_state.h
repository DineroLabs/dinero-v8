#pragma once

// ============================================================================
// CONSENSUS LAYER - PURE UTXO SNAPSHOT STATE
// ============================================================================
//
// Phase 2: Pure Consensus Architecture
//
// This structure represents an immutable snapshot of the consensus UTXO state.
// Used for:
//   - Trivial reorg rollback (Restore(snapshot) instead of block-by-block undo)
//   - Parallel validation with snapshot isolation
//   - Testing without database
//
// INVARIANTS:
//   - NO persistence concerns (no DB, no filesystem)
//   - NO threading concerns (no mutex)
//   - Pure value type with deep copy semantics
//
// ============================================================================

#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include "primitives/uint256.h"
#include <unordered_map>
#include <cstdint>
#include <vector>

namespace dinero {
namespace consensus {

// Forward declare UtreexoHash type (32-byte hash)
// Full definition in utreexo_accumulator.h
using UtreexoHash = std::vector<uint8_t>;

/**
 * UTXOSnapshot - Immutable snapshot of consensus UTXO state
 *
 * Contains everything needed to restore a ConsensusUTXOSet to a
 * previous state. Used for trivial reorg rollback.
 *
 * Memory considerations:
 * - Phase 2: Full deep copy (correctness first)
 * - Phase 3: Copy-on-Write optimization (if needed)
 */
struct UTXOSnapshot {
    // Block height at snapshot time
    uint32_t height = 0;

    // Block hash corresponding to this UTXO state
    uint256 block_hash;

    // Utreexo accumulator root at snapshot time
    UtreexoHash utreexo_root;

    // Full UTXO map (deep copy)
    std::unordered_map<OutPoint, UTXOEntry> utxos;

    // Number of leaves in Utreexo forest at snapshot time
    uint64_t utreexo_num_leaves = 0;

    // Full serialized forest state at snapshot time.
    // Needed for true rollback semantics; root + leaf count alone are not enough
    // to reconstruct proofs or deletion state.
    std::vector<uint8_t> utreexo_forest_state;

    /**
     * Create a deep copy of this snapshot
     *
     * Returns independent copy with no shared state.
     */
    UTXOSnapshot clone() const {
        UTXOSnapshot copy;
        copy.height = height;
        copy.block_hash = block_hash;
        copy.utreexo_root = utreexo_root;
        copy.utxos = utxos;  // Deep copy of map
        copy.utreexo_num_leaves = utreexo_num_leaves;
        copy.utreexo_forest_state = utreexo_forest_state;
        return copy;
    }

    /**
     * Get approximate memory usage in bytes
     *
     * Used for metrics and memory pressure monitoring.
     */
    size_t GetMemoryUsage() const {
        size_t usage = sizeof(UTXOSnapshot);
        usage += utreexo_root.size();
        usage += utreexo_forest_state.size();

        for (const auto& [outpoint, entry] : utxos) {
            usage += sizeof(OutPoint);
            usage += sizeof(UTXOEntry);
            usage += entry.scriptPubKey.size();
            usage += entry.commitment.size();
        }

        return usage;
    }

    /**
     * Get number of UTXOs in snapshot
     */
    size_t GetUTXOCount() const {
        return utxos.size();
    }

    /**
     * Check if snapshot is empty (genesis state)
     */
    bool IsEmpty() const {
        return utxos.empty() && height == 0;
    }
};

} // namespace consensus
} // namespace dinero
