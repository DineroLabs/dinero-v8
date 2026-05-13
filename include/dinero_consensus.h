#pragma once

// ============================================================================
// DINERO CONSENSUS LIBRARY - PUBLIC API
// ============================================================================
//
// libdinero-consensus: The rules of money, verifiable anywhere.
//
// This library is PURE:
//   - NO IO (no filesystem, no database)
//   - NO networking
//   - NO threads (no mutex, no atomic for sync)
//   - NO logging (except debug assertions)
//   - NO globals (except const)
//   - NO clocks
//   - NO randomness
//
// What this enables:
//   - Mobile wallets verify blocks themselves
//   - Hardware wallets validate transactions
//   - Browsers verify consensus via WASM
//   - Light clients don't trust, they verify
//
// Inputs are parameters. Outputs are return values. State is explicit.
//
// ============================================================================

#include <string>
#include <cstdint>
#include <vector>

// =============================================================================
// PURE CONSENSUS TYPES
// =============================================================================

// Core UTXO types (pure, no IO)
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include "consensus/utxo_snapshot_state.h"
#include "consensus/block_undo.h"

// Pure UTXO set (no database, no threading)
#include "consensus/consensus_utxo_set.h"

// Consensus invariants (forensic-grade assertions)
#include "consensus/consensus_invariants.h"

// Utreexo accumulator (pure cryptographic accumulator)
#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_delta.h"

// Consensus rules (pure constants and functions)
#include "consensus/subsidy.h"
#include "consensus/limits.h"
#include "consensus/coinbase_maturity.h"

// Cryptographic primitives (pure math)
#include "consensus/merkle_root.h"
#include "consensus/pow.h"

// Primitives (data structures)
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "primitives/amount.h"

namespace dinero {
namespace consensus {

// ============================================================================
// Library Version
// ============================================================================

/**
 * Get consensus library version
 *
 * Format: MAJOR.MINOR.PATCH
 * Any change to consensus rules increments MAJOR.
 */
constexpr const char* CONSENSUS_LIB_VERSION = "1.0.0";

/**
 * Get consensus protocol version
 *
 * This identifies the consensus rules. All nodes with the same
 * protocol version will reach the same consensus.
 */
constexpr uint32_t CONSENSUS_PROTOCOL_VERSION = 1;

// ============================================================================
// Convenience Functions (delegate to ConsensusUTXOSet)
// ============================================================================

/**
 * Create a snapshot of the UTXO set
 *
 * Returns an immutable copy that can be used for:
 *   - Stateless verification
 *   - Rollback (trivial reorg)
 *   - Parallel validation
 *
 * @param utxo_set The UTXO set to snapshot
 * @return Immutable snapshot
 */
inline UTXOSnapshot CreateSnapshot(const ConsensusUTXOSet& utxo_set) {
    return utxo_set.Snapshot();
}

/**
 * Restore UTXO set from a snapshot
 *
 * Replaces current state with snapshot contents.
 * Used for trivial reorg rollback.
 *
 * @param utxo_set The UTXO set to restore
 * @param snapshot The snapshot to restore from
 */
inline void RestoreSnapshot(ConsensusUTXOSet& utxo_set, const UTXOSnapshot& snapshot) {
    utxo_set.Restore(snapshot);
}

// ============================================================================
// Subsidy and Supply (delegate to ConsensusSubsidy)
// ============================================================================

/**
 * Get block subsidy at height
 *
 * @param height Block height
 * @return Block reward in una
 */
inline AmountUna GetBlockSubsidy(uint32_t height) {
    return ConsensusSubsidy::GetBlockSubsidy(height);
}

/**
 * Get maximum possible supply at height
 *
 * @param height Block height
 * @return Maximum supply in una
 */
inline uint64_t GetMaxSupplyAtHeight(uint32_t height) {
    return ConsensusSubsidy::GetTotalIssuedAtHeight(height);
}

} // namespace consensus
} // namespace dinero
