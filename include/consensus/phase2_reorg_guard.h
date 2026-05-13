#pragma once

// ============================================================================
// PHASE 2: SNAPSHOT-BASED REORG GUARD
// ============================================================================
//
// Phase2ReorgGuard - RAII guard for atomic reorg commits using snapshots
//
// Key differences from original ReorgGuard:
// - Uses ConsensusUTXOSet (pure in-memory) instead of UTXOSet (persistence-coupled)
// - Uses PersistentUTXOAdapter for persistence (adapter pattern)
// - Supports snapshot/restore for trivial reorg rollback
//
// Reorg safety:
// - On failure: UTXO state is NOT modified (snapshot restore)
// - On success: Atomic commit via PersistentUTXOAdapter + ChainDB batch
//
// ============================================================================

#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "storage/persistent_utxo_adapter.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/utxo_snapshot_state.h"
#include "common/logger.h"
#include <rocksdb/write_batch.h>

namespace dinero {
namespace consensus {

/**
 * Phase2ReorgGuard - RAII guard for atomic reorg commits (Phase 2 Architecture)
 *
 * Ensures that chain tip updates are only persisted if the reorg succeeds.
 * Uses snapshot-based rollback for failure safety.
 *
 * Usage:
 *   Phase2ReorgGuard guard(chain_db, utxo_set, adapter, token);
 *
 *   // Take snapshot at fork point
 *   guard.snapshotBeforeReorg();
 *
 *   // Disconnect old blocks, connect new blocks
 *   // (All operations on pure in-memory ConsensusUTXOSet)
 *
 *   if (failure) {
 *       // Guard destructor automatically restores snapshot
 *       return;
 *   }
 *
 *   guard.commit(new_tip, new_height, new_work);
 *
 * Crash Safety (Phase 2):
 * - If process crashes before commit(), nothing is persisted
 * - On restart, ConsensusUTXOSet is rebuilt from ChainDB via PersistentUTXOAdapter
 * - No partial state possible
 */
class Phase2ReorgGuard {
public:
    /**
     * Construct guard with Phase 2 components
     *
     * @param chain_db   ChainDB for persisting tip
     * @param utxo_set   ConsensusUTXOSet (pure in-memory)
     * @param adapter    PersistentUTXOAdapter for persistence
     * @param token      Write authorization token
     */
    Phase2ReorgGuard(ChainDB& chain_db,
                     ConsensusUTXOSet& utxo_set,
                     storage::PersistentUTXOAdapter& adapter,
                     ChainWriteToken& token)
        : chain_db_(chain_db)
        , utxo_set_(utxo_set)
        , adapter_(adapter)
        , token_(token)
        , batch_()
        , committed_(false)
        , snapshot_taken_(false) {}

    /**
     * Destructor - restores snapshot if not committed
     *
     * If commit() was not called and a snapshot was taken,
     * the UTXO set is restored to its pre-reorg state.
     */
    ~Phase2ReorgGuard() {
        if (!committed_ && snapshot_taken_) {
            // Restore UTXO set to pre-reorg state
            utxo_set_.Restore(pre_reorg_snapshot_);
            dinero::g_logger.info("Phase2ReorgGuard: Restored snapshot on reorg failure");
        }
    }

    // Disable copy/move (guard is RAII, not a value)
    Phase2ReorgGuard(const Phase2ReorgGuard&) = delete;
    Phase2ReorgGuard& operator=(const Phase2ReorgGuard&) = delete;
    Phase2ReorgGuard(Phase2ReorgGuard&&) = delete;
    Phase2ReorgGuard& operator=(Phase2ReorgGuard&&) = delete;

    /**
     * Take snapshot before reorg operations begin
     *
     * MUST be called before any disconnect/connect operations.
     * The snapshot will be used to restore on failure.
     */
    void snapshotBeforeReorg() {
        pre_reorg_snapshot_ = utxo_set_.Snapshot();
        snapshot_taken_ = true;
        dinero::g_logger.info("Phase2ReorgGuard: Snapshot taken (" +
                              std::to_string(pre_reorg_snapshot_.GetUTXOCount()) + " UTXOs)");
    }

    /**
     * Get the WriteBatch for adding additional writes
     *
     * @return Reference to the internal WriteBatch
     */
    rocksdb::WriteBatch& getBatch() {
        return batch_;
    }

    /**
     * Commit all changes atomically (Phase 2: via PersistentUTXOAdapter)
     *
     * Persists UTXO changes via adapter, updates chain tip, commits batch.
     *
     * @param new_tip_hash  Hash of new chain tip
     * @param new_height    Height of new chain tip
     * @param new_work      Chainwork of new chain tip
     *
     * FATAL on failure - calls std::terminate()
     */
    void commit(const uint256& new_tip_hash, int new_height, const arith_uint256& new_work) {
        // Phase 2: Commit UTXO state via adapter
        if (!adapter_.CommitState(utxo_set_, &batch_)) {
            dinero::g_logger.error("FATAL: Failed to commit UTXO state via adapter");
            std::terminate();
        }

        // Add tip update to batch
        auto status = chain_db_.setTip(token_, new_tip_hash, new_height, new_work, &batch_);
        if (status != Status::Ok) {
            dinero::g_logger.error("FATAL: Failed to prepare tip update in Phase2ReorgGuard");
            std::terminate();
        }

        // Commit batch atomically (sync=true for durability)
        status = chain_db_.writeBatch(token_, std::move(batch_), true);
        if (status != Status::Ok) {
            dinero::g_logger.error("FATAL: Failed to commit reorg batch to ChainDB");
            dinero::g_logger.error("Chain state is now inconsistent - manual intervention required");
            std::terminate();
        }

        committed_ = true;
        dinero::g_logger.info("Phase2ReorgGuard: Commit successful (height " +
                              std::to_string(new_height) + ")");
    }

private:
    ChainDB& chain_db_;                       // Database for persistent tip
    ConsensusUTXOSet& utxo_set_;              // Pure in-memory UTXO set
    storage::PersistentUTXOAdapter& adapter_; // Adapter for persistence
    ChainWriteToken& token_;                  // Write authorization
    rocksdb::WriteBatch batch_;               // Atomic write batch
    bool committed_;                          // Track commit status
    bool snapshot_taken_;                     // Track if snapshot was taken
    UTXOSnapshot pre_reorg_snapshot_;         // Snapshot for rollback
};

} // namespace consensus
} // namespace dinero
