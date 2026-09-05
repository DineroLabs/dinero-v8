// Copyright (c) 2026 Dinero Labs.
//
// Shielded-state root — the shielded half of a snapshot's chain binding.
//
// WHY THIS EXISTS
// ───────────────
// `header.utreexo_root` already binds a snapshot's UTXO/forest half to the
// chain: LoadSnapshot recomputes the forest commitment and refuses to load if
// it disagrees with the base block's header. The shielded half has no such
// binding, so today it rests on a hardcoded trust anchor (which needs a
// release per snapshot) or a signed manifest (which needs a key nobody can
// ever lose). This is the fingerprint a future header commitment would carry,
// so a node can verify shielded state against the chain it already validated
// by proof-of-work, and trust no publisher at all.
//
// RELATIONSHIP TO DSR2
// ────────────────────
// `ChainstateService::ComputeShieldedReorgStateHash()` (tag 'DSR2') hashes the
// same three shielded containers AND the utreexo forest. It is a reorg
// invertibility oracle, and it is deliberately NOT reused here:
//
//   1. The forest is already committed by `header.utreexo_root`. Including it
//      would commit it twice, and — worse — make a consensus value hostage to
//      forest serialization: any future change to forest encoding would alter
//      a header field.
//   2. DSR2 concatenates two variable-length blobs (nullifier content, then
//      anchor bytes) with no length framing. For a diagnostic oracle that is
//      harmless. For a consensus commitment it is not: without explicit
//      lengths, a byte moved across the boundary between the two sections can
//      produce the same preimage from different state. This layout prefixes
//      every variable-length section with its length.
//
// VERSION 2 — why the nullifier section is an accumulator digest
// ──────────────────────────────────────────────────────────────
// v1 hashed `NullifierSet::SerializeContent()` bytes directly, which inherited
// two properties from the storage layer that a header commitment must not
// depend on: the canonical ordering came from a SQLite `ORDER BY` (so it hung
// on BLOB collation and on that query never being reworked), and the function
// returns EMPTY BYTES to signal a read error — hashing to exactly the
// empty-set digest, making a local database fault indistinguishable from an
// attacker who deleted every nullifier. v2 commits to
// `ComputeNullifierAccumulator` (tag 'NUL1'), which sorts and de-duplicates
// itself and cannot express that confusion. Never activated at v1, so there
// is no compatibility obligation.
//
// LAYOUT (concatenated, then SHA-256'd)
// ─────────────────────────────────────
//   [tag 'SHR1']                    4 B
//   [version = 1]                   1 B
//   [shielded tree root]           32 B   (zeros if the root is not 32 B)
//   [shielded tree size LE]         8 B
//   [nullifier accumulator]        32 B   (ComputeNullifierAccumulator, tag 'NUL1')
//   [anchor history length LE]      8 B
//   [anchor history bytes]     variable   (AnchorHistory::SerializeBytes)
//
// The forest appears nowhere. That is the point.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_accumulator.h"
#include "consensus/shielded/nullifier_set.h"
#include "primitives/uint256.h"

namespace dinero {
namespace consensus {
namespace shielded {

/// Tag distinguishing this digest from DSR2's. A digest computed under one
/// tag can never be mistaken for the other.
inline constexpr char SHIELDED_ROOT_TAG[4] = {'S', 'H', 'R', '1'};
inline constexpr uint8_t SHIELDED_ROOT_VERSION = 2;

/// Build the preimage. Exposed so tests can assert on the exact bytes rather
/// than only on the digest — a layout change must be visible, not just felt.
std::vector<uint8_t> BuildShieldedRootPreimage(const std::vector<uint8_t>& tree_root,
                                               uint64_t tree_size,
                                               const uint256& nullifier_accumulator,
                                               const std::vector<uint8_t>& anchor_bytes);

/// Hash of the preimage above. Pure: no chainstate, no I/O, no globals — so
/// it is testable with vectors and cannot drift with daemon state.
uint256 ComputeShieldedRootFromParts(const std::vector<uint8_t>& tree_root,
                                     uint64_t tree_size,
                                     const uint256& nullifier_accumulator,
                                     const std::vector<uint8_t>& anchor_bytes);

/// Convenience overload over the live containers.
///
/// Returns `std::nullopt` when the nullifier set cannot be enumerated. A
/// caller must propagate that rather than substituting any digest: see
/// nullifier_accumulator.h for why an unreadable set and an empty set must
/// never collapse to the same value.
std::optional<uint256> ComputeShieldedRoot(const CommitmentTree& tree,
                                           const NullifierSet& nullifiers,
                                           const AnchorHistory& anchors);

}  // namespace shielded
}  // namespace consensus
}  // namespace dinero
