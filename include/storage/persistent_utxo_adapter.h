#pragma once

// ============================================================================
// STORAGE LAYER - PERSISTENT UTXO ADAPTER
// ============================================================================
//
// Phase 2: Pure Consensus Architecture
//
// PersistentUTXOAdapter bridges pure consensus state to persistent storage.
// It owns the database interaction and handles:
//   - Loading initial state from ChainDB on startup
//   - Committing consensus state changes to ChainDB
//   - Atomic batching with WriteBatch
//
// CRITICAL: This is a STORAGE component, NOT a CONSENSUS component.
// Consensus code MUST NOT include this header.
//
// ============================================================================

#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/outpoint.h"
#include <rocksdb/write_batch.h>
#include <vector>
#include <utility>

namespace dinero {
namespace storage {

/**
 * PersistentUTXOAdapter - Bridges consensus to persistence
 *
 * Phase 2: Pure Consensus Architecture
 *
 * This adapter:
 * - Loads ConsensusUTXOSet from ChainDB on startup
 * - Commits ConsensusUTXOSet state to ChainDB
 * - Provides atomic batching for reorg safety
 *
 * The adapter is REPLACEABLE. Consensus has no knowledge of it.
 * Different storage backends could implement the same interface.
 */
class PersistentUTXOAdapter {
public:
    /**
     * Construct adapter with database and write token
     *
     * @param db ChainDB reference (must outlive adapter)
     * @param token Write authorization token
     */
    explicit PersistentUTXOAdapter(ChainDB& db, ChainWriteToken& token);
    ~PersistentUTXOAdapter() = default;

    // Disable copy (adapter owns state)
    PersistentUTXOAdapter(const PersistentUTXOAdapter&) = delete;
    PersistentUTXOAdapter& operator=(const PersistentUTXOAdapter&) = delete;

    // =========================================================================
    // Initialization
    // =========================================================================

    /**
     * Load initial state from ChainDB into ConsensusUTXOSet
     *
     * Called during startup to populate pure consensus state from
     * persistent storage.
     *
     * @param consensus_set The consensus UTXO set to populate
     * @return true if load succeeded, false on error
     */
    bool LoadInitialState(consensus::ConsensusUTXOSet& consensus_set);

    // =========================================================================
    // Persistence
    // =========================================================================

    /**
     * Commit current consensus state to ChainDB
     *
     * Writes all UTXOs from consensus set to database.
     * Uses provided WriteBatch for atomic commit.
     *
     * @param consensus_set The consensus UTXO set to commit
     * @param batch Optional WriteBatch for atomic commit (nullptr = immediate)
     * @return true if commit succeeded
     */
    bool CommitState(const consensus::ConsensusUTXOSet& consensus_set,
                    rocksdb::WriteBatch* batch = nullptr);

    /**
     * Commit delta changes to ChainDB (optimization)
     *
     * Only writes changed UTXOs instead of full state.
     * More efficient for incremental updates.
     *
     * @param added UTXOs added since last commit
     * @param removed OutPoints of UTXOs removed since last commit
     * @param batch Optional WriteBatch for atomic commit
     * @return true if commit succeeded
     */
    bool CommitDelta(
        const std::vector<std::pair<OutPoint, consensus::UTXOEntry>>& added,
        const std::vector<OutPoint>& removed,
        rocksdb::WriteBatch* batch = nullptr);

    /**
     * Flush any pending writes immediately
     *
     * Called on shutdown to ensure durability.
     *
     * @return true if flush succeeded
     */
    bool Flush();

    // =========================================================================
    // State Queries
    // =========================================================================

    /**
     * Get tip information from ChainDB
     *
     * @return TipInfo or error
     */
    StatusOr<TipInfo> GetTip() const;

    /**
     * Get UTXO count from ChainDB
     *
     * @return Number of UTXOs in persistent storage
     */
    size_t GetPersistentUTXOCount() const;

private:
    ChainDB& db_;
    ChainWriteToken& token_;

    // Convert between consensus and storage types
    static Coin ToDbCoin(const consensus::UTXOEntry& entry);
    static consensus::UTXOEntry FromDbCoin(const Coin& coin, uint32_t height);
};

// =============================================================================
// Phase 2 Simplified Startup Pattern
// =============================================================================
//
// The Phase 2 architecture simplifies startup to:
//   1. Load from DB:     adapter.LoadInitialState(consensus_set)
//   2. Take snapshot:    baseline = consensus_set.Snapshot()
//   3. Run consensus:    Phase2ActivateBestChain(...) uses snapshot/restore
//
// This replaces complex undo-based recovery with trivial snapshot restore.
// On crash: rebuild from ChainDB (consistent state guaranteed by atomic commits).
//
// Usage:
//   ChainWriteToken token;
//   PersistentUTXOAdapter adapter(chain_db, token);
//   ConsensusUTXOSet utxo_set;
//
//   if (!adapter.LoadInitialState(utxo_set)) {
//       // Handle error
//   }
//
//   UTXOSnapshot baseline = utxo_set.Snapshot();  // Ready for consensus loop
//
// =============================================================================

} // namespace storage
} // namespace dinero
