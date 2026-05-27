// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Shielded output feed extractor (M2).
 *
 * Pure helper that walks a block's transactions and emits the public
 * shielded output and spend-nullifier metadata that the
 * `blockchain.shielded.outputs` RPC serves to thin clients.
 *
 * No view key, no recipient-specific filter — by design. See
 * docs/superpowers/specs/2026-05-27-trustless-light-client-shielded-m2-design.md
 * for the justification. The daemon cannot derive a recipient-tagged
 * compact filter from the current v5/v6 wire format (epk is sender-
 * random, d/pk_d are encrypted), so M2 ships a public output feed and
 * the client still does the trial-decrypt.
 *
 * Lives in `dinero_shielded` so the helper is reusable from RPC
 * handlers, indexer code, and gtest fixtures without dragging
 * `dinero_wallet` into the consensus layer.
 */

#include "common/status.h"                       // StatusOr
#include "consensus/shielded/commitment_tree.h"  // Hash, HASH_BYTES
#include "primitives/hash_domains.h"              // TxId
#include "primitives/uint256.h"                   // uint256

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

// Forward-declare Block in the correct namespace; full definition
// pulled in by the .cpp via "primitives/block.h".
namespace dinero { struct Block; }

namespace dinero::consensus::shielded {

/// Current decryptable shielded note size (epk || ct || tag =
/// 32 + 563 + 16). Older consensus-valid wallet paths emitted shorter
/// placeholder notes; the feed still returns those bytes so clients can
/// maintain a complete commitment tree and simply skip trial-decrypt.
constexpr size_t kShieldedEncryptedNoteBytes = 611;

/// One public output entry the feed emits to thin clients.
struct ShieldedOutputFeedEntry {
    uint256              block_hash;
    uint32_t             height = 0;
    TxId                 txid;
    uint32_t             tx_index = 0;
    uint32_t             output_index = 0;
    uint64_t             leaf_index = 0;
    Hash                 commitment{};
    std::vector<uint8_t> encrypted_note;
};

/// One public spend-nullifier entry. Nullifiers do NOT increment
/// `leaf_index`; only outputs append to the commitment tree.
struct ShieldedNullifierFeedEntry {
    uint256  block_hash;
    uint32_t height = 0;
    TxId     txid;
    uint32_t tx_index = 0;
    uint32_t spend_index = 0;
    Hash     nullifier{};
};

/// Aggregate result of extracting a single block. `next_leaf_index` is
/// `first_leaf_index + total_outputs_seen` — callers chain it across
/// blocks in canonical height order.
struct ShieldedOutputFeedResult {
    std::vector<ShieldedOutputFeedEntry>     outputs;
    std::vector<ShieldedNullifierFeedEntry>  spent_nullifiers;
    uint64_t                                 next_leaf_index = 0;
};

enum class ShieldedOutputFeedError : uint8_t {
    Ok                       = 0,
    BundleDecodeFailed       = 1,
};

/**
 * Extract the public shielded output + nullifier feed for `block`.
 *
 * Walks `block.vtx` in canonical block order. Skips transactions that
 * are not v5/v6 or whose `shielded_bundle_bytes` is empty. For shielded
 * transactions, deserialises the bundle via `DeserializeShieldedBundle`
 * (preserves canonical sorted-by-commitment within-bundle order),
 * and emits one feed entry per output + one nullifier entry per spend.
 * The encrypted note is copied verbatim regardless of length: length is
 * not a consensus validity condition, and legacy 96-byte placeholders
 * must still reach light clients for tree-completeness.
 *
 * Leaf indices: `leaf_index = first_leaf_index + outputs_seen_so_far`
 * (across the entire block, in tx-then-output order — same order the
 * commitment tree appends them).
 *
 * Spend nullifiers do NOT advance `leaf_index`. `out->next_leaf_index`
 * is set to `first_leaf_index + out->outputs.size()`.
 *
 * Returns `Ok` on success. On any decode failure, `out` is left in an
 * undefined intermediate state and the corresponding error code is
 * returned so the RPC layer can surface a structured error instead of
 * silently dropping bundles.
 *
 * Pre-conditions: `out != nullptr`.
 */
ShieldedOutputFeedError ExtractShieldedOutputFeed(
    const dinero::Block& block,
    uint32_t height,
    uint64_t first_leaf_index,
    ShieldedOutputFeedResult* out);

// ── Leaf-index walk helper (T2) ────────────────────────────────────

/// Look up a block by height. Returns nullopt if no block exists at
/// that height (chain tip is below it, or the block is not in storage).
using BlockByHeightLookup = std::function<std::optional<dinero::Block>(uint32_t height)>;

/**
 * Count the total number of shielded outputs in blocks
 * `[shielded_activation_height, from_height)` — i.e., the
 * `first_leaf_index` the next block (at `from_height`) would consume.
 *
 * This is the deterministic source of truth for leaf indexing that the
 * `blockchain.shielded.outputs` RPC needs when servicing a height
 * range. For M2 the walk is O(chain_height) at the requested boundary
 * and has no cache — the plan calls this out explicitly so the sanity
 * log can measure honestly. If benchmarks show the walk dominates, a
 * follow-up commit adds a sidecar cache; do NOT preemptively add one.
 *
 * Returns `Status::NotFound` if `lookup` returns nullopt for any height
 * in the walk window (chain not synced past `from_height-1` yet).
 * Returns `Status::Serialization` if a historical shielded bundle
 * fails to decode. Returns the count on success.
 *
 * If `from_height <= shielded_activation_height`, returns 0 — no
 * shielded outputs exist before activation.
 */
StatusOr<uint64_t> CountShieldedOutputsBeforeHeight(
    uint32_t from_height,
    uint32_t shielded_activation_height,
    BlockByHeightLookup lookup);

} // namespace dinero::consensus::shielded
