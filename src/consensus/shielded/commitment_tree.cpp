/**
 * Incremental Poseidon Merkle tree — append-only commitment tree for the
 * shielded pool. See include/consensus/shielded/commitment_tree.h.
 *
 * Architecture rule: this tree is structurally separate from Utreexo.
 * No code path inserts a shielded commitment into the Utreexo accumulator.
 *
 * Hash function: Poseidon-2 over the secp256k1 scalar field (t=3, x^5,
 * 240 R1CS constraints). This MUST match the Poseidon gadget used in ZK
 * circuits — if they diverge, proofs generated off-chain won't verify.
 * The native evaluator poseidon2_bytes is the same function the gadget
 * constrains, guaranteeing bit-for-bit consistency.
 */

#include "consensus/shielded/commitment_tree.h"

// poseidon2_bytes — native Poseidon-2 evaluator. Header lives in src/
// rather than include/ because the ZK subsystem is not public API.
#include "poseidon_gadget.h"
#include "zk/zkvm/scalar.h"

#include <cstring>

namespace dinero::consensus::shielded {

namespace {

Hash Poseidon(const Hash& a, const Hash& b) {
    return dinero::zk::zkvm::poseidon2_bytes(a, b);
}

std::array<Hash, TREE_DEPTH + 1> ComputeEmptyRoots() {
    std::array<Hash, TREE_DEPTH + 1> roots{};
    // Empty leaf: Poseidon(0, 0)
    Hash zero{};
    roots[0] = Poseidon(zero, zero);
    for (size_t i = 1; i <= TREE_DEPTH; ++i) {
        roots[i] = Poseidon(roots[i - 1], roots[i - 1]);
    }
    return roots;
}

} // namespace

// ── Public hash functions ────────────────────────────────────────────

Hash PoseidonHash2(const Hash& left, const Hash& right) {
    return Poseidon(left, right);
}

// Phase 2 wave 5: address-binding tag scalar.
//
// "DIN/v7/shielded/addr/v1" (23 bytes ASCII) placed at the start of a
// 32-byte buffer, zero-padded after. The big-endian Scalar interpretation
// in the circuit reads byte 0 as the most-significant byte, so the DST
// string occupies the high end of the scalar.
const Hash& AddrBindTag() {
    static const Hash kTag = []() {
        Hash h{};
        constexpr const char kDst[] = "DIN/v7/shielded/addr/v1";
        constexpr size_t kDstLen = sizeof(kDst) - 1;  // exclude NUL
        static_assert(kDstLen <= HASH_BYTES, "ADDR_TAG must fit in 32 bytes");
        std::memcpy(h.data(), kDst, kDstLen);
        return h;
    }();
    return kTag;
}

Hash NoteCommitment(const Hash& d,
                    const Hash& recipient_pk,
                    const Hash& value_commitment,
                    const Hash& randomness) {
    // Phase 2 wave 5: addr_bind = Poseidon(ADDR_TAG, Poseidon(d, pk_d))
    const Hash addr_bind = Poseidon(AddrBindTag(), Poseidon(d, recipient_pk));
    // commitment = Poseidon(Poseidon(addr_bind, value), randomness)
    return Poseidon(Poseidon(addr_bind, value_commitment), randomness);
}

Hash ComputeNullifier(const Hash& secret_key, uint64_t leaf_index) {
    Hash idx{};
    const auto scalar_bytes = dinero::zk::zkvm::Scalar(leaf_index).bytes();
    std::memcpy(idx.data(), scalar_bytes.data(), HASH_BYTES);
    return Poseidon(secret_key, idx);
}

// ── CommitmentTree ───────────────────────────────────────────────────

const std::array<Hash, TREE_DEPTH + 1>& CommitmentTree::EmptyRoots() {
    static const auto roots = ComputeEmptyRoots();
    return roots;
}

CommitmentTree::CommitmentTree() {
    frontier_.fill(Hash{});
}

CommitmentTree::CommitmentTree(const CommitmentTree& other) {
    std::lock_guard<std::mutex> lock(other.mutex_);
    size_ = other.size_;
    frontier_ = other.frontier_;
    leaves_ = other.leaves_;
}

CommitmentTree& CommitmentTree::operator=(const CommitmentTree& other) {
    if (this == &other) {
        return *this;
    }
    std::scoped_lock lock(mutex_, other.mutex_);
    size_ = other.size_;
    frontier_ = other.frontier_;
    leaves_ = other.leaves_;
    return *this;
}

CommitmentTree::CommitmentTree(CommitmentTree&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.mutex_);
    size_ = other.size_;
    frontier_ = std::move(other.frontier_);
    leaves_ = std::move(other.leaves_);
}

CommitmentTree& CommitmentTree::operator=(CommitmentTree&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    std::scoped_lock lock(mutex_, other.mutex_);
    size_ = other.size_;
    frontier_ = std::move(other.frontier_);
    leaves_ = std::move(other.leaves_);
    return *this;
}

uint64_t CommitmentTree::Append(const Hash& commitment) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t index = size_;
    leaves_.push_back(commitment);
    Hash current = commitment;

    // Walk up the tree. At each level, if the current index bit is 0,
    // we're starting a new subtree (store in frontier). If 1, merge
    // with the frontier hash at this level.
    uint64_t idx = index;
    for (size_t depth = 0; depth < TREE_DEPTH; ++depth) {
        if ((idx & 1) == 0) {
            frontier_[depth] = current;
            // Right sibling doesn't exist yet — use empty root.
            // But we don't need to compute the full root here; just
            // store frontier and break. The root is computed lazily.
            break;
        } else {
            current = PoseidonHash2(frontier_[depth], current);
        }
        idx >>= 1;
    }

    ++size_;
    return index;
}

Hash CommitmentTree::Root() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Hash current = EmptyRoots()[0];
    for (size_t depth = 0; depth < TREE_DEPTH; ++depth) {
        if (((size_ >> depth) & 1) == 1) {
            current = PoseidonHash2(frontier_[depth], current);
        } else {
            current = PoseidonHash2(current, EmptyRoots()[depth]);
        }
    }
    return current;
}

std::optional<CommitmentTree::AuthPath> CommitmentTree::GetAuthPath(uint64_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= size_ || leaves_.size() != size_) {
        return std::nullopt;
    }

    AuthPath path{};
    path.leaf_index = index;

    std::vector<Hash> layer = leaves_;
    uint64_t pos = index;

    for (size_t depth = 0; depth < TREE_DEPTH; ++depth) {
        const size_t sibling_pos = static_cast<size_t>(pos ^ 1ULL);
        path.siblings[depth] =
            sibling_pos < layer.size() ? layer[sibling_pos] : EmptyRoots()[depth];

        std::vector<Hash> next_layer;
        next_layer.reserve((layer.size() + 1) / 2);
        for (size_t i = 0; i < layer.size(); i += 2) {
            const Hash& left = layer[i];
            const Hash& right =
                (i + 1 < layer.size()) ? layer[i + 1] : EmptyRoots()[depth];
            next_layer.push_back(PoseidonHash2(left, right));
        }

        layer = std::move(next_layer);
        pos >>= 1;
    }

    return path;
}

bool CommitmentTree::Truncate(uint64_t new_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (new_size > size_) {
        return false;
    }
    if (leaves_.size() != size_) {
        return false;
    }

    std::vector<Hash> retained(leaves_.begin(), leaves_.begin() + static_cast<std::ptrdiff_t>(new_size));
    frontier_.fill(Hash{});
    leaves_.clear();
    size_ = 0;

    for (const auto& leaf : retained) {
        const uint64_t index = size_;
        leaves_.push_back(leaf);
        Hash current = leaf;
        uint64_t idx = index;
        for (size_t depth = 0; depth < TREE_DEPTH; ++depth) {
            if ((idx & 1) == 0) {
                frontier_[depth] = current;
                break;
            } else {
                current = PoseidonHash2(frontier_[depth], current);
            }
            idx >>= 1;
        }
        ++size_;
    }

    return true;
}

std::vector<uint8_t> CommitmentTree::SerializeFrontier() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint8_t> out;
    out.resize(8 + TREE_DEPTH * HASH_BYTES);
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((size_ >> (8 * i)) & 0xFF);
    }
    for (size_t d = 0; d < TREE_DEPTH; ++d) {
        std::memcpy(out.data() + 8 + d * HASH_BYTES, frontier_[d].data(), HASH_BYTES);
    }
    return out;
}

bool CommitmentTree::DeserializeFrontier(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (len < 8 + TREE_DEPTH * HASH_BYTES) return false;
    size_ = 0;
    leaves_.clear();
    for (int i = 0; i < 8; ++i) {
        size_ |= static_cast<uint64_t>(data[i]) << (8 * i);
    }
    for (size_t d = 0; d < TREE_DEPTH; ++d) {
        std::memcpy(frontier_[d].data(), data + 8 + d * HASH_BYTES, HASH_BYTES);
    }
    return true;
}

} // namespace dinero::consensus::shielded
