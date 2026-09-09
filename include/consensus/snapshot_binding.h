// Copyright (c) 2026 Dinero Labs.
//
// state_commitment_v1 snapshot binding proof (v5 snapshots).
//
// WHY THIS EXISTS
// ───────────────
// A v4 snapshot's shielded section is authenticated only by the snapshot's own
// checksum until the genesis→base replay completes, hours later. That is
// self-certification: a forged section carries a valid checksum of itself.
// The binding proof closes the gap at LOAD time with the chain of custody
//
//   DNRS commitment ─→ proven coinbase ─→ tx merkle root ─→ selected
//                                                           best-work header
//
// so the snapshot's claimed shielded state is bound to a PoW-authenticated
// header before a single post-load block is processed. The replay comparison
// remains as the independent second defense.
//
// The functions here are PURE and side-effect-free so the boundary can be
// tested exhaustively — every input is passed in, nothing reads Params() or
// touches a chainstate, and tests can sweep the quadrants directly.
//
// FAILURE CLASSES, not per-byte verdicts: the enum distinguishes classes a
// caller or test must be able to tell apart (a missing proof and a malformed
// commitment must never collapse into one "rejected"), while mutations within
// one class (payload vs claimed-root tampering) legitimately share a verdict
// (both surface as CommitmentMismatch).

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "consensus/state_commitment.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"

namespace dinero::consensus {

enum class SnapshotBindingVerdict {
    Ok,
    /// v5 section absent (or unparseable) where enforcement requires it.
    MissingProof,
    /// The carried coinbase does not hash up the carried branch to the base
    /// header's transaction merkle root (or the branch is degenerate).
    InvalidMerkleProof,
    /// The proven transaction is not a coinbase.
    InvalidCoinbase,
    /// FindStateCommitment on the proven coinbase did not yield exactly one
    /// well-formed DNRS commitment (missing, duplicate, or malformed — the
    /// lookup's own status distinguishes those three for diagnostics; as a
    /// binding-failure CLASS they are one).
    MalformedOrDuplicateCommitment,
    /// The proven DNRS value does not equal the full SHR1 shielded root
    /// computed from the restored snapshot state (tree root + size +
    /// nullifier accumulator + anchors — never the tree root alone).
    CommitmentMismatch,
    /// The base is not an ancestor of the selected best-work header tip, or
    /// lies fewer than burial-depth blocks below it. Ancestry is part of the
    /// definition: height difference alone would let a high-but-not-best
    /// side chain look buried.
    InsufficientBurialOrNonAncestry,
};

/// Stable names for logs and test assertions.
const char* SnapshotBindingVerdictName(SnapshotBindingVerdict v);

/// Merkle branch for the coinbase (transaction index 0): the sibling at each
/// level of the tree built by ComputeMerkleRoot. For index 0 the node is the
/// LEFT child at every level, so verification is a simple left-fold; a
/// single-transaction block has an empty branch (root == txid).
/// Empty input yields an empty branch (the zero-hash root convention of
/// ComputeMerkleRoot has no coinbase to prove).
std::vector<uint256> ComputeCoinbaseMerkleBranch(const std::vector<Transaction>& vtx);

/// Fold `txid` up `branch` (index-0 rule, double-SHA256 pairs — the same
/// pairing ComputeMerkleRoot uses) and compare against `merkle_root`.
bool VerifyCoinbaseMerkleBranch(const uint256& txid,
                                const std::vector<uint256>& branch,
                                const uint256& merkle_root);

/// The pure verification core for a carried proof: branch → coinbase type → exactly-one DNRS
/// parse → compare against the full SHR1 root computed from the restored
/// state. Burial/ancestry and proof PRESENCE are decided by the caller (they
/// need chain context and file context respectively); this function assumes
/// the proof was carried and answers whether it binds.
SnapshotBindingVerdict EvaluateSnapshotBinding(
    const Transaction& coinbase,
    const std::vector<uint256>& branch,
    const uint256& header_merkle_root,
    const uint256& computed_shielded_root);

/// Burial policy, pure form. `ancestor_at_base_height` is the hash of the
/// selected best-work header chain's ancestor AT the base height (nullopt when
/// the best chain has no entry there — not an ancestor, or headers missing:
/// both fail closed). Passing the resolved ancestor rather than a chainstate
/// keeps the policy sweepable; the caller resolves it via the header
/// selector's best-chain walk, which is what makes "ancestor of best-work"
/// true by construction rather than by height arithmetic.
SnapshotBindingVerdict EvaluateSnapshotBurial(
    const uint256& base_hash,
    uint32_t base_height,
    const std::optional<uint256>& ancestor_at_base_height,
    uint32_t best_header_height,
    uint32_t burial_depth);

}  // namespace dinero::consensus
