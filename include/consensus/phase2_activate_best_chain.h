#pragma once

// ============================================================================
// PHASE 2: SNAPSHOT-BASED ACTIVATE BEST CHAIN
// ============================================================================
//
// Phase2ActivateBestChain - Fork-choice with snapshot-based reorg
//
// Key differences from original ActivateBestChain:
// - Uses ConsensusUTXOSet (pure in-memory) instead of UTXOSet
// - Takes snapshot at fork point before any mutations
// - On failure: trivial restore via snapshot (no undo records needed)
// - On success: commits via PersistentUTXOAdapter
//
// This is the target architecture for Phase 2:
// - Consensus correctness independent of persistence correctness
// - Reorg becomes trivial: snapshot → mutate → restore on fail / commit on success
//
// ============================================================================

#include "consensus/phase2_reorg_guard.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/block_validation.h"
#include "consensus/block_undo.h"
#include "consensus/chain_state.h"
#include "storage/persistent_utxo_adapter.h"
#include "storage/chain_db.h"
#include "storage/chain_write_token.h"
#include "p2p/state_transition.h"
#include "common/logger.h"
#include <vector>
#include <string>

namespace dinero {
namespace consensus {

// Re-use types from p2p namespace
using p2p::BlockIndex;
using p2p::Hash256;

// ChainState is now provided by chain_state.h

// ============================================================================
// Phase2ActivateBestChainResult
// ============================================================================

struct Phase2ActivateBestChainResult {
    bool ok;
    std::string error;

    uint32_t blocks_disconnected;
    uint32_t blocks_connected;
    Hash256 old_tip;
    Hash256 new_tip;

    Phase2ActivateBestChainResult()
        : ok(true), error(""),
          blocks_disconnected(0), blocks_connected(0) {}

    static Phase2ActivateBestChainResult Ok(uint32_t disconnected, uint32_t connected,
                                             const Hash256& old_t, const Hash256& new_t) {
        Phase2ActivateBestChainResult result;
        result.ok = true;
        result.blocks_disconnected = disconnected;
        result.blocks_connected = connected;
        result.old_tip = old_t;
        result.new_tip = new_t;
        return result;
    }

    static Phase2ActivateBestChainResult Fail(const std::string& err) {
        Phase2ActivateBestChainResult result;
        result.ok = false;
        result.error = err;
        return result;
    }
};

// ============================================================================
// Phase2ActivateBestChain
// ============================================================================

/**
 * Phase2ActivateBestChain - Snapshot-based fork-choice orchestration
 *
 * ALGORITHM:
 * 1. Check if candidate is already active (no-op)
 * 2. Find fork point
 * 3. Take snapshot at fork point (CRITICAL)
 * 4. Disconnect blocks from active tip to fork point
 * 5. Connect blocks from fork point to candidate tip
 * 6. On failure: Restore snapshot (trivial rollback)
 * 7. On success: Commit via PersistentUTXOAdapter
 *
 * KEY INSIGHT:
 * - All mutations happen on pure in-memory ConsensusUTXOSet
 * - Snapshot/Restore provides trivial rollback
 * - Persistence happens only on success (edge)
 *
 * @param candidate_tip     BlockIndex of the candidate chain tip
 * @param chainstate        Current chainstate (will be updated on success)
 * @param chain_db          ChainDB for persistence
 * @param token             Write authorization token
 * @param utxo_set          ConsensusUTXOSet (pure in-memory)
 * @param adapter           PersistentUTXOAdapter for persistence
 * @param block_index_db    Block index database
 * @param undo_storage      Undo storage (for loading blocks)
 * @param validator         BlockValidator for connect/disconnect
 *
 * @return Phase2ActivateBestChainResult with reorg stats or error
 */
Phase2ActivateBestChainResult Phase2ActivateBestChain(
    const BlockIndex& candidate_tip,
    ChainState& chainstate,
    ChainDB& chain_db,
    ChainWriteToken& token,
    ConsensusUTXOSet& utxo_set,
    storage::PersistentUTXOAdapter& adapter,
    p2p::IBlockIndexDB& block_index_db,
    p2p::IUndoStorage& undo_storage,
    BlockValidator& validator
);

} // namespace consensus
} // namespace dinero
