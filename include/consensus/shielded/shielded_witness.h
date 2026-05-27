// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Shielded witness builder (M3).
 *
 * Pure helper that replays public shielded outputs from
 * `shielded_activation_height` through a client-pinned
 * `anchor_height`, materialises the full commitment tree at that
 * anchor, validates the anchor root, and returns the auth path for
 * a requested leaf. Powers the `shielded.witness.by_index` RPC the
 * M3 spend path calls to fetch trustless witnesses.
 *
 * Replay-based by design — see the M3 spec
 * (docs/superpowers/specs/2026-05-27-trustless-light-client-shielded-m3-spend-design.md)
 * for the justification. The daemon's live commitment tree only
 * retains a frontier (no per-leaf siblings); auth paths therefore
 * require a fresh full-leaf reconstruction. The same public output
 * walk that powers M2's `blockchain.shielded.outputs` produces the
 * leaves consumed here — single source of truth.
 *
 * Lives in `dinero_shielded` alongside the M2 output-feed extractor.
 * No view key, no wallet data — public chain bytes only.
 */

#include "consensus/shielded/commitment_tree.h"   // CommitmentTree, Hash, AuthPath
#include "consensus/shielded/shielded_output_feed.h" // BlockByHeightLookup

#include <cstdint>
#include <functional>
#include <optional>

namespace dinero { struct Block; }

namespace dinero::consensus::shielded {

/// Client-supplied request: which leaf, at which historical anchor.
struct ShieldedWitnessRequest {
    uint64_t leaf_index = 0;
    uint32_t anchor_height = 0;
    Hash     anchor_root{};
    uint32_t shielded_activation_height = 0;
};

/// Witness payload returned to the client. The auth_path siblings,
/// combined with the commitment and the leaf_index bit-decomposition,
/// reconstruct `anchor_root` deterministically — i.e., the client
/// verifies the witness against its own previously-trusted anchor.
struct ShieldedWitness {
    uint64_t                  leaf_index = 0;
    uint32_t                  anchor_height = 0;
    uint64_t                  tree_size = 0;
    Hash                      anchor_root{};
    Hash                      commitment{};
    CommitmentTree::AuthPath  auth_path{};
};

enum class ShieldedWitnessError : uint8_t {
    Ok                  = 0,
    MissingBlock        = 1,
    BundleDecodeFailed  = 2,
    LeafOutOfRange      = 3,
    AnchorMismatch      = 4,
};

/**
 * Build a witness for the requested leaf at the requested anchor.
 *
 * Replays blocks `[shielded_activation_height, anchor_height]`
 * inclusive, feeds each block through `ExtractShieldedOutputFeed`,
 * and appends every public output's commitment to a fresh
 * `CommitmentTree` in canonical order (same order the consensus
 * commitment tree applies them).
 *
 * After replay:
 *   - If `leaf_index >= tree.Size()` → `LeafOutOfRange`.
 *   - If `tree.Root() != req.anchor_root` → `AnchorMismatch`.
 *     (The client has staked their trust on `anchor_root`; if the
 *     daemon's replay disagrees, one side is wrong and the witness
 *     would not verify — refuse rather than ship a useless path.)
 *
 * On success, `out` carries the leaf's commitment, its 32-sibling
 * auth path, the tree size at the anchor, and a copy of the
 * canonical anchor_root.
 *
 * Pre-conditions:
 *   - `out != nullptr`
 *   - `lookup` is callable; nullopt = block missing at that height
 *
 * On any error, `out` is left in an undefined intermediate state.
 */
ShieldedWitnessError BuildWitnessByIndex(
    const ShieldedWitnessRequest& req,
    BlockByHeightLookup lookup,
    ShieldedWitness* out);

} // namespace dinero::consensus::shielded
