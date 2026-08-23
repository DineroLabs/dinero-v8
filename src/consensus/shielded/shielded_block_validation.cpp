/**
 * Block-level shielded validation.
 * See include/consensus/shielded/shielded_block_validation.h.
 */

#include "consensus/shielded/shielded_block_validation.h"

#include "consensus/shielded/shielded_validation.h"  // ApplyShieldedBundle

#include <unordered_set>

namespace dinero::consensus::shielded {

namespace {

struct HashHasher {
    size_t operator()(const Hash& h) const {
        size_t r = 0;
        for (size_t i = 0; i < 8 && i < HASH_BYTES; ++i) {
            r ^= static_cast<size_t>(h[i]) << (i * 8);
        }
        return r;
    }
};

} // namespace

BlockValidationError ValidateBlockShielded(
    const std::vector<ShieldedBundle>& bundles,
    const std::vector<int64_t>& transparent_deltas,
    const BlockShieldedContext& ctx) {

    if (bundles.size() != transparent_deltas.size()) {
        return BlockValidationError::GlobalConservationViolation;
    }

    // 1. Cross-tx nullifier uniqueness within this block.
    //    Per-tx validation already checked intra-tx + against existing set.
    //    This catches inter-tx duplicates (same nullifier in two different txs).
    std::unordered_set<Hash, HashHasher> block_nullifiers;
    for (const auto& bundle : bundles) {
        for (const auto& spend : bundle.spends) {
            if (!block_nullifiers.insert(spend.nullifier).second) {
                return BlockValidationError::InterTxNullifierDuplicate;
            }
            // Also check against the pre-block nullifier set.
            if (ctx.existing_nullifiers &&
                ctx.existing_nullifiers->Contains(spend.nullifier)) {
                return BlockValidationError::InterTxNullifierDuplicate;
            }
        }
    }

    // 2. Per-tx conservation: each bundle's value_balance must match
    //    the transparent delta for that tx.
    for (size_t i = 0; i < bundles.size(); ++i) {
        if (bundles[i].IsEmpty()) continue;
        if (bundles[i].value_balance != transparent_deltas[i]) {
            return BlockValidationError::GlobalConservationViolation;
        }
    }

    // 3. Global conservation: the sum of all value_balances must equal
    //    the sum of all transparent deltas. This is implied by #2 but
    //    serves as a defense-in-depth check.
    int64_t total_balance = 0;
    int64_t total_delta = 0;
    for (size_t i = 0; i < bundles.size(); ++i) {
        total_balance += bundles[i].value_balance;
        total_delta += transparent_deltas[i];
    }
    if (total_balance != total_delta) {
        return BlockValidationError::GlobalConservationViolation;
    }

    return BlockValidationError::Ok;
}

bool ApplyBlockShielded(const std::vector<ShieldedBundle>& bundles,
                        CommitmentTree* tree,
                        NullifierSet* nullifiers,
                        uint32_t block_height) {
    // Deterministic ordering: process bundles in block tx order (0, 1, 2, ...).
    // Within each bundle, outputs are appended in their canonical order
    // (sorted by commitment — enforced by serialization). Nullifiers are
    // inserted in canonical order (sorted by nullifier).
    //
    // Audit gap #8 (resolved): the per-bundle apply lives in exactly one
    // place — `ApplyShieldedBundle` in shielded_validation.cpp. This wrapper
    // is a determinism-ordered loop over that primitive so block-level and
    // single-bundle callers cannot drift apart by maintaining two copies
    // of the same Append/Insert sequence.
    for (const auto& bundle : bundles) {
        if (!ApplyShieldedBundle(bundle, tree, nullifiers, block_height)) {
            return false;
        }
    }
    return true;
}

} // namespace dinero::consensus::shielded
