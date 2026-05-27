// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "consensus/shielded/shielded_witness.h"

#include "consensus/shielded/shielded_output_feed.h"
#include "primitives/block.h"

namespace dinero::consensus::shielded {

ShieldedWitnessError BuildWitnessByIndex(
    const ShieldedWitnessRequest& req,
    BlockByHeightLookup lookup,
    ShieldedWitness* out) {

    if (out == nullptr) {
        return ShieldedWitnessError::MissingBlock;
    }

    // Fresh full-leaf tree: GetAuthPath() needs the complete leaf set,
    // which the daemon's live consensus tree (frontier-only) cannot
    // provide. We rebuild from scratch using the same public output
    // sequence M2 ships.
    CommitmentTree tree;
    bool captured_commitment = false;
    Hash target_commitment{};

    // Replay [shielded_activation_height, anchor_height] inclusive.
    uint64_t next_leaf = 0;
    for (uint32_t h = req.shielded_activation_height;
         h <= req.anchor_height;
         ++h) {
        std::optional<::dinero::Block> block_opt = lookup(h);
        if (!block_opt.has_value()) {
            return ShieldedWitnessError::MissingBlock;
        }
        const ::dinero::Block& block = *block_opt;

        ShieldedOutputFeedResult feed{};
        const auto status = ExtractShieldedOutputFeed(block, h, next_leaf, &feed);
        if (status != ShieldedOutputFeedError::Ok) {
            return ShieldedWitnessError::BundleDecodeFailed;
        }
        for (const auto& entry : feed.outputs) {
            tree.Append(entry.commitment);
            if (entry.leaf_index == req.leaf_index) {
                target_commitment   = entry.commitment;
                captured_commitment = true;
            }
        }
        next_leaf = feed.next_leaf_index;

        // Guard against the loop bound when anchor_height == UINT32_MAX
        // (degenerate request); incrementing past it would overflow.
        if (h == req.anchor_height) break;
    }

    // Bounds check before anchor check — leaf-out-of-range is the
    // simpler, more local failure to surface to the client.
    if (req.leaf_index >= tree.Size()) {
        return ShieldedWitnessError::LeafOutOfRange;
    }

    // Anchor validation: the client staked trust on `anchor_root`.
    // If the daemon's deterministic replay disagrees, refuse rather
    // than ship a witness that would not verify.
    const Hash derived_root = tree.Root();
    if (derived_root != req.anchor_root) {
        return ShieldedWitnessError::AnchorMismatch;
    }

    auto auth_path_opt = tree.GetAuthPath(req.leaf_index);
    if (!auth_path_opt.has_value() || !captured_commitment) {
        // Should be unreachable given the size check above (replay
        // preserves the full leaf set, and a leaf that lives in the
        // tree was captured during this pass), but defensive.
        return ShieldedWitnessError::LeafOutOfRange;
    }

    out->leaf_index    = req.leaf_index;
    out->anchor_height = req.anchor_height;
    out->tree_size     = tree.Size();
    out->anchor_root   = derived_root;
    out->commitment    = target_commitment;
    out->auth_path     = *auth_path_opt;
    return ShieldedWitnessError::Ok;
}

} // namespace dinero::consensus::shielded
