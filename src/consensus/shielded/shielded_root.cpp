// Copyright (c) 2026 Dinero Labs.
//
// See shielded_root.h for the layout and why it is not DSR2.

#include "consensus/shielded/shielded_root.h"

#include <cstring>

#include "crypto/sha256.h"

namespace dinero {
namespace consensus {
namespace shielded {

namespace {

void AppendLE64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

// Length-prefixed. This is the difference from DSR2 that matters: without an
// explicit length, the boundary between two adjacent variable-length sections
// is inferred from content, so a byte can move across it without changing the
// preimage. For a diagnostic hash that is a curiosity; for a value committed
// in a block header it is a forgery vector.
void AppendLengthPrefixed(std::vector<uint8_t>& out, const std::vector<uint8_t>& bytes) {
    AppendLE64(out, static_cast<uint64_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

}  // namespace

std::vector<uint8_t> BuildShieldedRootPreimage(const std::vector<uint8_t>& tree_root,
                                               uint64_t tree_size,
                                               const uint256& nullifier_accumulator,
                                               const std::vector<uint8_t>& anchor_bytes) {
    std::vector<uint8_t> pre;
    pre.reserve(96 + anchor_bytes.size());

    pre.insert(pre.end(), std::begin(SHIELDED_ROOT_TAG), std::end(SHIELDED_ROOT_TAG));
    pre.push_back(SHIELDED_ROOT_VERSION);

    // Fixed 32 bytes, zero-filled if the root is not exactly 32. Never
    // truncated and never skipped: either would shift every later field and
    // silently change what the digest means.
    if (tree_root.size() == 32) {
        pre.insert(pre.end(), tree_root.begin(), tree_root.end());
    } else {
        pre.insert(pre.end(), 32, 0);
    }
    AppendLE64(pre, tree_size);

    // Fixed 32 bytes: the nullifier set enters through its own canonical
    // accumulator (tag 'NUL1'), which owns the ordering rule and cannot report
    // an unreadable set as an empty one. See nullifier_accumulator.h.
    pre.insert(pre.end(), nullifier_accumulator.data, nullifier_accumulator.data + 32);

    AppendLengthPrefixed(pre, anchor_bytes);

    // No forest. header.utreexo_root already commits it; committing it twice
    // would tie this consensus value to forest serialization.
    return pre;
}

uint256 ComputeShieldedRootFromParts(const std::vector<uint8_t>& tree_root,
                                     uint64_t tree_size,
                                     const uint256& nullifier_accumulator,
                                     const std::vector<uint8_t>& anchor_bytes) {
    const auto pre =
        BuildShieldedRootPreimage(tree_root, tree_size, nullifier_accumulator, anchor_bytes);
    dinero::crypto::CSHA256 hasher;
    hasher.Write(pre.data(), pre.size());
    uint8_t digest[32];
    hasher.Finalize(digest);
    uint256 out;
    std::memcpy(out.data, digest, 32);
    return out;
}

std::optional<uint256> ComputeShieldedRoot(const CommitmentTree& tree,
                                           const NullifierSet& nullifiers,
                                           const AnchorHistory& anchors) {
    // Fail closed: no accumulator, no root. Substituting any digest here would
    // reintroduce exactly the confusion the accumulator removes.
    const auto acc = AccumulateNullifierSet(nullifiers);
    if (!acc.has_value()) {
        return std::nullopt;
    }
    const auto root = tree.Root();
    return ComputeShieldedRootFromParts(std::vector<uint8_t>(root.begin(), root.end()),
                                        tree.Size(), *acc, anchors.SerializeBytes());
}

}  // namespace shielded
}  // namespace consensus
}  // namespace dinero
