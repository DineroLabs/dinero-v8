// Copyright (c) 2026 Dinero Labs.
//
// See snapshot_binding.h for the design. The pairing here MUST stay
// byte-identical to ComputeMerkleRoot's: same double-SHA256, same odd-count
// self-duplication. A divergence would let a branch verify against a root no
// block ever had (or reject one it does have).

#include "consensus/snapshot_binding.h"

#include <cstring>

#include "crypto/sha256.h"

namespace dinero::consensus {

namespace {

uint256 HashPair(const uint256& left, const uint256& right) {
    uint8_t buf[64];
    std::memcpy(buf, left.data, 32);
    std::memcpy(buf + 32, right.data, 32);
    uint8_t mid[32];
    crypto::CSHA256().Write(buf, 64).Finalize(mid);
    uint256 out;
    crypto::CSHA256().Write(mid, 32).Finalize(out.data);
    return out;
}

}  // namespace

const char* SnapshotBindingVerdictName(SnapshotBindingVerdict v) {
    switch (v) {
        case SnapshotBindingVerdict::Ok: return "ok";
        case SnapshotBindingVerdict::MissingProof: return "missing-proof";
        case SnapshotBindingVerdict::InvalidMerkleProof: return "invalid-merkle-proof";
        case SnapshotBindingVerdict::InvalidCoinbase: return "invalid-coinbase";
        case SnapshotBindingVerdict::MalformedOrDuplicateCommitment:
            return "malformed-or-duplicate-commitment";
        case SnapshotBindingVerdict::CommitmentMismatch: return "commitment-mismatch";
        case SnapshotBindingVerdict::InsufficientBurialOrNonAncestry:
            return "insufficient-burial-or-non-ancestry";
    }
    return "unknown";  // unreachable with a valid enum; no default above so a
                       // new verdict is a compiler warning at the switch
}

std::vector<uint256> ComputeCoinbaseMerkleBranch(const std::vector<Transaction>& vtx) {
    std::vector<uint256> branch;
    if (vtx.size() <= 1) return branch;  // root == txid; nothing to carry

    std::vector<uint256> layer;
    layer.reserve(vtx.size());
    for (const auto& tx : vtx) {
        layer.push_back(tx.GetTxid().AsUint256());
    }

    // Index 0 is the leftmost leaf at every level, so its sibling is always
    // layer[1] (a layer of size 1 is the root and the walk is over).
    while (layer.size() > 1) {
        branch.push_back(layer[1]);
        std::vector<uint256> next;
        next.reserve((layer.size() + 1) / 2);
        for (size_t i = 0; i < layer.size(); i += 2) {
            const bool have_right = (i + 1 < layer.size());
            const uint256& left = layer[i];
            const uint256& right = have_right ? layer[i + 1] : layer[i];
            next.push_back(HashPair(left, right));
        }
        layer = std::move(next);
    }
    return branch;
}

bool VerifyCoinbaseMerkleBranch(const uint256& txid,
                                const std::vector<uint256>& branch,
                                const uint256& merkle_root) {
    // Cap the fold depth: 2^32 leaves need 32 levels; anything deeper is a
    // malformed proof, not a bigger block. Without the cap an attacker-sized
    // branch buys free hashing work before the inevitable mismatch.
    if (branch.size() > 32) return false;
    uint256 node = txid;
    for (const auto& sibling : branch) {
        node = HashPair(node, sibling);  // index 0: always the left child
    }
    return node == merkle_root;
}

SnapshotBindingVerdict EvaluateSnapshotBinding(
    const Transaction& coinbase,
    const std::vector<uint256>& branch,
    const uint256& header_merkle_root,
    const uint256& computed_shielded_root) {
    // Order matters for diagnosability: prove the coinbase is IN the header
    // first, then read what it commits to. Reversed, a mismatch verdict could
    // be raised for a coinbase that was never the block's coinbase at all.
    if (!VerifyCoinbaseMerkleBranch(coinbase.GetTxid().AsUint256(), branch,
                                    header_merkle_root)) {
        return SnapshotBindingVerdict::InvalidMerkleProof;
    }

    // Merkle membership alone does not establish transaction type. Requiring
    // the null coinbase prevout also rules out a short non-coinbase encoding
    // being interpreted as a 64-byte internal merkle node.
    if (!coinbase.IsCoinbase()) {
        return SnapshotBindingVerdict::InvalidCoinbase;
    }
    const StateCommitmentLookup lookup = FindStateCommitment(coinbase);
    if (lookup.status != StateCommitmentStatus::Ok) {
        return SnapshotBindingVerdict::MalformedOrDuplicateCommitment;
    }

    // Full SHR1 root, never the tree root alone: a comparison against only
    // the tree root would let forged nullifiers or anchors ride through.
    if (lookup.root != computed_shielded_root) {
        return SnapshotBindingVerdict::CommitmentMismatch;
    }
    return SnapshotBindingVerdict::Ok;
}

SnapshotBindingVerdict EvaluateSnapshotBurial(
    const uint256& base_hash,
    uint32_t base_height,
    const std::optional<uint256>& ancestor_at_base_height,
    uint32_t best_header_height,
    uint32_t burial_depth) {
    // Ancestry first: the best-work chain must actually CONTAIN the base.
    // A missing ancestor (no entry at that height, headers not synced, or a
    // side-chain base) fails closed.
    if (!ancestor_at_base_height.has_value() ||
        *ancestor_at_base_height != base_hash) {
        return SnapshotBindingVerdict::InsufficientBurialOrNonAncestry;
    }
    // Depth second. Guard the subtraction: an attacker-claimed base height
    // above the best header must not wrap into a huge "depth".
    if (best_header_height < base_height ||
        best_header_height - base_height < burial_depth) {
        return SnapshotBindingVerdict::InsufficientBurialOrNonAncestry;
    }
    return SnapshotBindingVerdict::Ok;
}

}  // namespace dinero::consensus
