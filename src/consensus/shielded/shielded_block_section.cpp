// Copyright (c) 2026 Dinero Labs.
//
// See include/consensus/shielded/shielded_block_section.h.
//
// This is the connect-tail logic MOVED out of
// BlockValidator::ApplyBlockShieldedSection (block_validation.cpp) with
// mechanical substitutions: value-type refs instead of void*-casts, a
// nullable AnchorHistory* instead of a member, an out-param snapshot
// instead of undo.pre_reset_shielded_epoch, and reset/activation heights
// passed in instead of read from dinero::Params(). The checks and their
// order are unchanged. A later task delegates both existing copies
// (BlockValidator's method and the reindexer's inline block) to this
// function instead of duplicating it.

#include "consensus/shielded/shielded_block_section.h"

#include "consensus/shielded/shielded_block_validation.h"
#include "consensus/shielded/shielded_epoch.h"

namespace dinero::consensus::shielded {

bool ConnectBlockShieldedSection(
    const std::vector<ShieldedBundle>& bundles,
    const std::vector<int64_t>& deltas,
    uint32_t height,
    uint32_t reset_height,
    uint32_t activation_height,
    CommitmentTree& tree,
    NullifierSet& nullifiers,
    AnchorHistory* anchors,
    std::optional<ShieldedEpochSnapshot>& pre_reset_snapshot_out,
    std::string& error) {
    // ─────────────────────────────────────────────────────────────────────
    // Shielded epoch reset (hard-fork cutover). See shielded_epoch.h.
    // ─────────────────────────────────────────────────────────────────────
    // At exactly the reset height the pre-cutover pool is discarded to a fresh
    // empty epoch, making every old note unspendable. The cutover block must be
    // shielded-empty (wall rule) so the reset has nothing to race. Capture the
    // full pre-reset pool into the undo record FIRST (so a reorg disconnecting
    // across the cutover can restore the old epoch), THEN discard it.
    if (IsShieldedEpochResetHeight(height, reset_height)) {
        if (!bundles.empty()) {
            error = "shielded-tx-at-epoch-reset-height";
            return false;
        }
        if (!anchors) {
            error = "shielded-epoch-reset-missing-anchor-state";
            return false;
        }
        pre_reset_snapshot_out = CaptureShieldedEpoch(tree, *anchors, nullifiers);
        const auto& snap = *pre_reset_snapshot_out;
        if ((nullifiers.Size() > 0 && snap.nullifiers.empty()) ||
            (tree.Size() > 0 && snap.tree_frontier.empty())) {
            error = "shielded-epoch-reset-capture-failed";
            return false;
        }
        ResetShieldedEpoch(tree, *anchors, nullifiers);
    }

    if (!bundles.empty()) {
        BlockShieldedContext bctx;
        bctx.existing_nullifiers = &nullifiers;
        bctx.pre_block_tree = &tree;
        bctx.block_height = height;

        auto berr = ValidateBlockShielded(bundles, deltas, bctx);
        if (berr != BlockValidationError::Ok) {
            error = "shielded-block-validation-failed (code " +
                std::to_string(static_cast<int>(berr)) + ")";
            return false;
        }

        // Deterministic apply: commitments + nullifiers in block tx order.
        ApplyBlockShielded(bundles, &tree, &nullifiers, height);
    }

    // Record the post-block tree root in the AnchorHistory window once per
    // connected block, AFTER any of this block's shielded outputs have been
    // appended. Gate on shielded activation height. This fires for EVERY
    // block at/after activation — not just blocks with shielded bundles — on
    // purpose: a live-built node records an anchor for every block ≥
    // activation (including empty blocks and the post-reset (H, empty_root)
    // at the cutover), so recording only shielded-tx blocks here would
    // produce a shorter anchor_history and thus a different DSR2
    // shieldedStateHash between a live-built and a reindexed/replayed node —
    // a consensus split. RecordRoot overwrites on a repeated height, so this
    // is safe even when the block-level apply above recorded nothing.
    if (anchors && height >= activation_height) {
        anchors->RecordRoot(height, tree.Root());
    }
    return true;
}

}  // namespace dinero::consensus::shielded
