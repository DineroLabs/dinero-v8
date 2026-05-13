#pragma once

// ============================================================================
// CONSENSUS LAYER - PURE UTXO SET INTERFACE
// ============================================================================
//
// Phase 2: Pure Consensus Architecture
//
// This interface defines the contract for consensus UTXO operations.
// Used for:
//   - Mocking in unit tests (no DB required)
//   - Adapter pattern (persistence can be swapped)
//   - Clean dependency injection
//
// INVARIANTS:
//   - NO persistence concerns
//   - NO threading concerns
//   - Pure consensus types only (OutPoint, UTXOEntry)
//
// ============================================================================

#include "consensus/interfaces/iutxo_provider.h"  // Phase 2: IConsensusUTXOSet IS-A IUTXOProvider
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include "consensus/utxo_snapshot_state.h"
#include "consensus/block_undo.h"
#include "primitives/uint256.h"
#include <memory>
#include <optional>
#include <string>

// Forward declarations
namespace dinero {
struct Block;

namespace consensus {
class UtreexoForest;
using UtreexoHash = std::vector<uint8_t>;
}
}

namespace dinero {
namespace consensus {

/**
 * IConsensusUTXOSet - Abstract interface for consensus UTXO operations
 *
 * PURPOSE: Define pure consensus UTXO contract for testing and adapters
 *
 * Implementations:
 *   - ConsensusUTXOSet: Production in-memory implementation
 *   - MockConsensusUTXOSet: For unit testing (no DB required)
 *
 * Usage:
 *   void ValidateBlock(IConsensusUTXOSet& utxo_set, const Block& block);
 */
class IConsensusUTXOSet : public IUTXOProvider {
public:
    ~IConsensusUTXOSet() override = default;

    // =========================================================================
    // Core UTXO Operations
    // =========================================================================

    /**
     * Add a new UTXO to the set
     */
    virtual bool AddCoin(const OutPoint& outpoint, const UTXOEntry& coin) = 0;

    /**
     * Spend (remove) a UTXO from the set
     */
    virtual std::unique_ptr<UTXOEntry> SpendCoin(const OutPoint& outpoint) = 0;

    /**
     * Get a UTXO without removing it
     */
    virtual const UTXOEntry* GetCoin(const OutPoint& outpoint) const = 0;

    /**
     * Check if a UTXO exists in the set
     */
    virtual bool HaveCoin(const OutPoint& outpoint) const = 0;

    /**
     * Delete a UTXO from the set (idempotent)
     */
    virtual bool DeleteCoin(const OutPoint& outpoint) = 0;

    // =========================================================================
    // Block Operations
    // =========================================================================

    /**
     * Apply a block to the UTXO set
     */
    virtual bool ApplyBlock(const Block& block, uint32_t height,
                           const uint256& block_hash, BlockUndo& undo,
                           UtreexoHash& computed_utreexo_root,
                           std::string& error) = 0;

    /**
     * Undo a block from the UTXO set
     */
    virtual bool UndoBlock(const Block& block, uint32_t height,
                          const BlockUndo& undo, std::string& error) = 0;

    // =========================================================================
    // Snapshot Operations (Trivial Reorg)
    // =========================================================================

    /**
     * Whether this backend supports snapshot/restore semantics.
     *
     * IMPORTANT:
     * - true: Snapshot()/Restore() are supported for rollback paths.
     * - false: callers MUST use explicit undo paths and must not rely on
     *          Snapshot() returning an "empty but valid" object.
     */
    virtual bool SupportsSnapshotRestore() const = 0;

    /**
     * Create a snapshot of the current state
     */
    virtual UTXOSnapshot Snapshot() const = 0;

    /**
     * Restore state from a snapshot
     */
    virtual void Restore(const UTXOSnapshot& snapshot) = 0;

    // =========================================================================
    // State Accessors
    // =========================================================================

    /**
     * Get current block height
     */
    virtual uint32_t GetHeight() const = 0;

    /**
     * Get best block hash
     */
    virtual const uint256& GetBestBlock() const = 0;

    /**
     * Set best block (after successful block connection)
     */
    virtual void SetBestBlock(const uint256& hash, uint32_t height) = 0;

    /**
     * Get Utreexo commitment (root hash)
     */
    virtual UtreexoHash GetUtreexoRoot() const = 0;

    /**
     * Get Utreexo forest for direct access
     */
    virtual UtreexoForest& GetForest() = 0;
    virtual const UtreexoForest& GetForest() const = 0;

    // =========================================================================
    // Metrics
    // =========================================================================

    /**
     * Get number of UTXOs in set
     */
    virtual size_t GetSetSize() const = 0;

    /**
     * Get approximate memory usage in bytes
     */
    virtual size_t GetMemoryUsage() const = 0;

    /**
     * Clear all state (for testing)
     */
    virtual void Clear() = 0;

    // =========================================================================
    // IUTXOProvider default implementations (delegates to IConsensusUTXOSet)
    // =========================================================================

    std::optional<UTXOEntry> GetUTXO(const OutPoint& outpoint) const override {
        const UTXOEntry* coin = GetCoin(outpoint);
        if (coin) return *coin;
        return std::nullopt;
    }

    bool AddUTXO(const OutPoint& outpoint, const UTXOEntry& entry) override {
        return AddCoin(outpoint, entry);
    }

    bool SpendUTXO(const OutPoint& outpoint, uint32_t /*spend_height*/) override {
        auto spent = SpendCoin(outpoint);
        return spent != nullptr;
    }

    bool DeleteUTXO(const OutPoint& outpoint) override {
        return DeleteCoin(outpoint);
    }

    bool HasUTXO(const OutPoint& outpoint) const override {
        return HaveCoin(outpoint);
    }
};

} // namespace consensus
} // namespace dinero
