#pragma once
/**
 * Block-level shielded validation — enforces invariants that per-tx
 * validation alone cannot catch.
 *
 * Three block-scope invariants:
 *
 *   1. ALL nullifiers across ALL v5 txs in the block are unique
 *      (per-tx check prevents intra-tx duplicates; this prevents
 *      inter-tx duplicates within the same block)
 *
 *   2. Total shielded value balance across the block is consistent
 *      with transparent state changes (global conservation)
 *
 *   3. Commitment tree updates are applied in deterministic tx order
 *      (block's canonical transaction ordering defines insertion order)
 *
 * Called AFTER per-tx validation passes for all transactions, BEFORE
 * state is committed. If this fails, the entire block is rejected.
 */

#include "consensus/shielded/shielded_tx.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dinero::consensus::shielded {

enum class BlockValidationError : uint8_t {
    Ok                          = 0,
    InterTxNullifierDuplicate   = 1,
    GlobalConservationViolation = 2,
    DeterminismFailure          = 3,
};

struct BlockShieldedContext {
    const NullifierSet*    existing_nullifiers;
    const CommitmentTree*  pre_block_tree;
    uint32_t               block_height;
};

/**
 * Validate all shielded bundles in a block as a unit.
 *
 * @param bundles          ShieldedBundle for each v5 tx, in block tx order.
 * @param transparent_deltas  Per-tx (transparent_in - transparent_out - fee),
 *                            in the same order as bundles.
 * @param ctx              Pre-block consensus state (read-only).
 *
 * Checks:
 *   1. No nullifier appears in more than one bundle within this block
 *   2. Each bundle's value_balance matches its transparent_delta
 *   3. Global sum of value_balances is consistent
 */
BlockValidationError ValidateBlockShielded(
    const std::vector<ShieldedBundle>& bundles,
    const std::vector<int64_t>& transparent_deltas,
    const BlockShieldedContext& ctx);

/**
 * Apply all shielded bundles in deterministic order.
 *
 * Commitments are appended in block tx order (tx 0 outputs first,
 * then tx 1 outputs, etc.). Nullifiers are inserted in the same order.
 * This guarantees every node computes the same tree root and nullifier
 * count after the block.
 *
 * @param bundles  In block tx order.
 */
void ApplyBlockShielded(const std::vector<ShieldedBundle>& bundles,
                        CommitmentTree* tree,
                        NullifierSet* nullifiers,
                        uint32_t block_height);

} // namespace dinero::consensus::shielded
