#pragma once

// ============================================================================
// CONSENSUS LAYER - PURE IN-MEMORY UTXO SET
// ============================================================================
//
// Phase 2: Pure Consensus Architecture
//
// ConsensusUTXOSet is the canonical UTXO state for consensus validation.
// It is PURE - no database, no mutex, no filesystem operations.
//
// CRITICAL INVARIANTS:
//   - NO persistence (no DB, no filesystem)
//   - NO threading primitives (no mutex, no atomic)
//   - NO IO operations
//   - Pure deterministic state transitions
//
// This enables:
//   - Trivial reorgs via Snapshot()/Restore()
//   - Unit tests without database setup
//   - Fuzzing and formal verification
//   - Consensus correctness independent of persistence correctness
//
// BANNED INCLUDES (enforced by CI):
//   - <rocksdb/*>
//   - <filesystem>
//   - <mutex>
//   - <thread>
//
// ============================================================================

#include "consensus/interfaces/iconsensus_utxo_set.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include "consensus/utxo_snapshot_state.h"
#include "consensus/utreexo_accumulator.h"
#include "consensus/block_undo.h"
#include "primitives/uint256.h"
#include <unordered_map>
#include <memory>
#include <string>

// Forward declarations
namespace dinero {
struct Block;
}

namespace dinero {
namespace consensus {

/**
 * ConsensusUTXOSet - Pure in-memory UTXO set with Utreexo accumulator
 *
 * Phase 2: Pure Consensus Architecture
 *
 * This class owns:
 * - In-memory UTXO map (OutPoint → UTXOEntry)
 * - Utreexo forest accumulator
 * - Current height and best block
 *
 * It does NOT own or know about:
 * - Database persistence
 * - Thread synchronization
 * - Filesystem operations
 *
 * Usage:
 * - Block validation operates on this pure state
 * - Persistence adapter loads/saves snapshots externally
 * - Reorgs use Snapshot()/Restore() for trivial rollback
 */
class ConsensusUTXOSet : public IConsensusUTXOSet {
public:
    ConsensusUTXOSet();
    ~ConsensusUTXOSet() override = default;

    // Disable copy (UTXO set should be unique per consensus instance)
    ConsensusUTXOSet(const ConsensusUTXOSet&) = delete;
    ConsensusUTXOSet& operator=(const ConsensusUTXOSet&) = delete;

    // Allow move
    ConsensusUTXOSet(ConsensusUTXOSet&&) = default;
    ConsensusUTXOSet& operator=(ConsensusUTXOSet&&) = default;

    // =========================================================================
    // Core UTXO Operations
    // =========================================================================

    /**
     * Add a new UTXO to the set
     *
     * @param outpoint The (txid, vout) identifying this UTXO
     * @param coin The UTXO entry (value, script, height, coinbase flag)
     * @return true if added successfully, false if already exists
     */
    bool AddCoin(const OutPoint& outpoint, const UTXOEntry& coin) override;

    /**
     * Spend (remove) a UTXO from the set
     *
     * @param outpoint The (txid, vout) to spend
     * @return The spent coin (for undo data), or nullptr if not found
     */
    std::unique_ptr<UTXOEntry> SpendCoin(const OutPoint& outpoint) override;

    /**
     * Get a UTXO without removing it
     *
     * @param outpoint The (txid, vout) to look up
     * @return Pointer to UTXO entry, or nullptr if not found
     */
    const UTXOEntry* GetCoin(const OutPoint& outpoint) const override;

    /**
     * Check if a UTXO exists in the set
     *
     * @param outpoint The (txid, vout) to check
     * @return true if UTXO exists (unspent), false otherwise
     */
    bool HaveCoin(const OutPoint& outpoint) const override;

    /**
     * Delete a UTXO from the set (idempotent)
     *
     * Used during reorg to remove created outputs.
     * Returns true even if UTXO was already absent (idempotent).
     *
     * @param outpoint The (txid, vout) to delete
     * @return true always (idempotent success)
     */
    bool DeleteCoin(const OutPoint& outpoint) override;

    // =========================================================================
    // Block Operations
    // =========================================================================

    /**
     * Apply a block to the UTXO set
     *
     * Processes all transactions:
     * - Spends inputs (removes from UTXO set)
     * - Creates outputs (adds to UTXO set)
     * - Updates Utreexo accumulator
     * - Populates undo data
     *
     * @param block The block to apply
     * @param height Block height
     * @param block_hash Block hash
     * @param undo [out] Populated with undo data for reorg
     * @param computed_utreexo_root [out] Computed Utreexo root after block
     * @param error [out] Error message if failed
     * @return true if block applied successfully
     */
    bool ApplyBlock(const Block& block, uint32_t height,
                   const uint256& block_hash, BlockUndo& undo,
                   UtreexoHash& computed_utreexo_root, std::string& error) override;

    /**
     * Undo a block from the UTXO set
     *
     * Reverses block application:
     * - Restores spent inputs
     * - Removes created outputs
     * - Restores Utreexo accumulator
     *
     * @param block The block to undo
     * @param height Block height
     * @param undo Undo data from original application
     * @param error [out] Error message if failed
     * @return true if block undone successfully
     */
    bool UndoBlock(const Block& block, uint32_t height,
                  const BlockUndo& undo, std::string& error) override;

    // =========================================================================
    // Snapshot Operations (Trivial Reorg)
    // =========================================================================

    bool SupportsSnapshotRestore() const override { return true; }

    /**
     * Create a snapshot of the current state
     *
     * Returns deep copy that can be used to restore state later.
     * Used for trivial reorg rollback.
     *
     * @return Immutable snapshot of current state
     */
    UTXOSnapshot Snapshot() const override;

    /**
     * Restore state from a snapshot
     *
     * Replaces current state with snapshot contents.
     * Used for trivial reorg rollback.
     *
     * @param snapshot The snapshot to restore from
     */
    void Restore(const UTXOSnapshot& snapshot) override;

    // =========================================================================
    // State Accessors
    // =========================================================================

    /**
     * Get current block height
     */
    uint32_t GetHeight() const override { return height_; }

    /**
     * Get best block hash
     */
    const uint256& GetBestBlock() const override { return best_block_; }

    /**
     * Set best block (after successful block connection)
     */
    void SetBestBlock(const uint256& hash, uint32_t height) override {
        best_block_ = hash;
        height_ = height;
    }

    /**
     * Get Utreexo commitment (root hash)
     */
    UtreexoHash GetUtreexoRoot() const override;

    /**
     * Get Utreexo forest for direct access
     *
     * Used by validation code that needs to generate proofs
     * or perform batch verification.
     */
    UtreexoForest& GetForest() override { return forest_; }
    const UtreexoForest& GetForest() const override { return forest_; }

    // =========================================================================
    // Metrics
    // =========================================================================

    /**
     * Get number of UTXOs in set
     */
    size_t GetSetSize() const override { return utxos_.size(); }

    /**
     * Get approximate memory usage in bytes
     */
    size_t GetMemoryUsage() const override;

    /**
     * Clear all state (for testing)
     *
     * WARNING: Destroys all consensus state. Only for tests.
     */
    void Clear() override;

    // =========================================================================
    // Direct UTXO Map Access (for persistence adapter)
    // =========================================================================

    /**
     * Get read-only access to UTXO map
     *
     * Used by persistence adapter to serialize state.
     */
    const std::unordered_map<OutPoint, UTXOEntry>& GetUTXOs() const {
        return utxos_;
    }

    /**
     * Bulk load UTXOs (for persistence adapter)
     *
     * Used during startup to load from persistent storage.
     * Clears existing state before loading.
     */
    bool BulkLoad(const std::unordered_map<OutPoint, UTXOEntry>& utxos,
                  uint32_t height, const uint256& best_block);

private:
    // In-memory UTXO set: OutPoint → UTXOEntry
    std::unordered_map<OutPoint, UTXOEntry> utxos_;

    // Utreexo accumulator (owned)
    UtreexoForest forest_;

    // Current state
    uint32_t height_ = 0;
    uint256 best_block_;

    // Height-aware canonical-roots semantics (issue #490).
    //
    // The canonical-roots fork (utreexo_canonical_roots_activation.h, Stage 3)
    // changes what an empty subtree contributes to the commitment. The live
    // BlockValidator flips the forest's mode and rebuilds roots when it first
    // sees the activation height (block_validation.cpp:882), and Restore()
    // derives the mode from the height being restored to.
    //
    // ApplyBlock and UndoBlock did neither, so this API could build a
    // post-activation forest still running LEGACY semantics. That mode
    // mismatch — not any unrepaired root logic — is what made a freshly
    // generated proof fail to verify here.
    //
    // Rebuilds ONLY when crossing the boundary; a call at a height whose mode
    // already matches is a no-op. Returns true iff a transition occurred.
    bool ApplyCanonicalSemanticsForHeight(uint32_t height);

    // Helper: Process a single transaction for ApplyBlock
    bool ProcessTransaction(const struct Transaction& tx, uint32_t height,
                           bool is_coinbase, BlockUndo& undo,
                           UtreexoDelta& utreexo_delta, std::string& error);

    // Helper: Undo a single transaction for UndoBlock
    bool UndoTransaction(const struct Transaction& tx,
                        const std::vector<UndoEntry>& spent_coins,
                        size_t& spent_index,
                        UtreexoDelta& utreexo_delta, std::string& error);
};

} // namespace consensus
} // namespace dinero
