#include "consensus/utreexo_stump.h"
#include "consensus/script_interpreter.h"  // For SHA256_Hash
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace dinero {
namespace consensus {

// 32 bytes of 0x00 — used in place of deleted children when computing parent hashes.
// Matches forest's computeSubtreeHash() semantics.
static const UtreexoHash ZERO_HASH(32, 0);

// ═══════════════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════════════

UtreexoStump::UtreexoStump() : numLeaves_(0) {
    // All roots start as nullopt (empty)
    roots_.fill(std::nullopt);
}

UtreexoStump UtreexoStump::fromRoots(
    const std::vector<std::optional<UtreexoHash>>& roots,
    uint64_t numLeaves
) {
    UtreexoStump stump;
    stump.numLeaves_ = numLeaves;

    // Copy provided roots (up to max)
    for (size_t i = 0; i < roots.size() && i < STUMP_MAX_ROOTS; ++i) {
        stump.roots_[i] = roots[i];
    }

    return stump;
}

UtreexoStump UtreexoStump::fromCommitment(
    const UtreexoHash& commitment,
    uint64_t numLeaves
) {
    // v2 commitment is a hash of (numLeaves + 64 fixed root slots). It is
    // not invertible, so individual roots cannot be reconstructed here.
    // Keep this as a leaf-count-only helper for opaque checkpoint metadata.
    // Callers that need proof verification must use fromRoots()/fromForest().
    (void)commitment;

    UtreexoStump stump;
    stump.numLeaves_ = numLeaves;

    return stump;
}

UtreexoStump UtreexoStump::fromForest(const UtreexoForest& forest) {
    UtreexoStump stump;
    stump.numLeaves_ = forest.getNumLeaves();

    // Get non-empty roots from forest and place at correct heights
    auto forestRoots = forest.getRoots();

    // The forest returns roots ordered by tree size (largest first in some impls).
    // We need to place them at the correct height indices based on numLeaves.

    uint64_t n = forest.getNumLeaves();
    size_t rootIdx = 0;

    for (uint8_t h = 0; h < STUMP_MAX_ROOTS && n > 0; ++h) {
        if (n & 1) {
            // There's a tree of height h
            if (rootIdx < forestRoots.size()) {
                stump.roots_[h] = forestRoots[rootIdx++];
            }
        }
        n >>= 1;
    }

    return stump;
}

// ═══════════════════════════════════════════════════════════════════════════
// Verification
// ═══════════════════════════════════════════════════════════════════════════

bool UtreexoStump::verifyBatchProof(
    const std::vector<UtreexoHash>& targets,
    const std::vector<uint64_t>& positions,
    const std::vector<UtreexoHash>& proof_hashes
) const {
    // Empty proof is valid if no targets
    if (targets.empty()) {
        return true;
    }

    // Must have same number of positions as targets
    if (positions.size() != targets.size()) {
        return false;
    }

    // Verify each target
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto& target = targets[i];
        uint64_t position = positions[i];

        // Extract siblings for this target from batched proof
        auto siblings = extractSiblings(i, targets, positions, proof_hashes);

        // Compute root from leaf and proof
        UtreexoHash computedRoot = computeRoot(target, siblings, position);

        // Determine which root this should match
        uint8_t rootIdx = getRootIndexForPosition(position);

        // Verify against stored root
        if (!roots_[rootIdx].has_value()) {
            return false;  // No root at this height
        }

        if (computedRoot != roots_[rootIdx].value()) {
            return false;  // Root mismatch
        }
    }

    return true;
}

bool UtreexoStump::verifyBlockProof(const BlockUtreexoProof& proof) const {
    return verifyBatchProof(proof.targets, proof.positions, proof.proof_hashes);
}

bool UtreexoStump::verifyTransition(
    const BlockUtreexoProof& proof,
    const std::vector<UtreexoHash>& roots_after_deletions,
    const std::vector<UtreexoHash>& additions,
    const UtreexoHash& expectedCommitmentAfter
) const {
    // Step 1: Verify deletions exist in current state
    if (!proof.isEmpty() && !verifyBlockProof(proof)) {
        return false;
    }

    // Step 2: Build post-deletion stump from intermediate roots
    // numLeaves unchanged (deletions don't change numLeaves in Utreexo)
    std::vector<std::optional<UtreexoHash>> indexed;
    uint64_t n = numLeaves_;
    size_t idx = 0;
    for (uint8_t h = 0; h < STUMP_MAX_ROOTS && n > 0; ++h) {
        if (n & 1) {
            if (idx < roots_after_deletions.size()) {
                if (indexed.size() <= h) {
                    indexed.resize(h + 1, std::nullopt);
                }
                indexed[h] = roots_after_deletions[idx++];
            }
        }
        n >>= 1;
    }
    UtreexoStump mid = UtreexoStump::fromRoots(indexed, numLeaves_);

    // Step 3: Apply additions
    for (const auto& leaf : additions) {
        mid.addSingle(leaf);
    }

    // Step 4: Check commitment matches expected
    return mid.verifyCommitment(expectedCommitmentAfter);
}

// ═══════════════════════════════════════════════════════════════════════════
// State Update
// ═══════════════════════════════════════════════════════════════════════════

bool UtreexoStump::modify(
    const BlockUtreexoProof& proof,
    const std::vector<UtreexoHash>& additions
) {
    // Step 1: Verify the proof (confirms deletion targets exist)
    if (!proof.isEmpty() && !verifyBlockProof(proof)) {
        return false;
    }

    // Step 2: Apply deletions (recompute affected roots)
    if (!proof.isEmpty()) {
        if (!applyDeletions(proof.targets, proof.positions, proof.proof_hashes)) {
            return false;
        }
    }

    // Step 3: Apply additions
    for (const auto& leaf : additions) {
        addSingle(leaf);
    }

    return true;
}

void UtreexoStump::add(const std::vector<UtreexoHash>& additions) {
    for (const auto& leaf : additions) {
        addSingle(leaf);
    }
}

void UtreexoStump::addSingle(const UtreexoHash& leaf) {
    // Adding a leaf to Utreexo forest:
    // 1. New leaf goes at position numLeaves (becomes a tree of height 0)
    // 2. If there's already a tree of height 0, merge them into height 1
    // 3. Continue merging until no collision

    UtreexoHash current = leaf;
    uint8_t height = 0;

    while (height < STUMP_MAX_ROOTS) {
        if (!roots_[height].has_value()) {
            // No tree at this height - place our tree here
            roots_[height] = current;
            break;
        } else {
            // Tree exists at this height - merge and continue
            UtreexoHash existing = roots_[height].value();
            roots_[height] = std::nullopt;  // Clear this slot

            // Merge: new parent = Hash(existing || current)
            // Note: existing is "left" (added first), current is "right"
            current = parentHash(existing, current);
            height++;
        }
    }

    numLeaves_++;
}

// ═══════════════════════════════════════════════════════════════════════════
// Queries
// ═══════════════════════════════════════════════════════════════════════════

std::vector<UtreexoHash> UtreexoStump::getRoots() const {
    std::vector<UtreexoHash> result;
    for (const auto& root : roots_) {
        if (root.has_value()) {
            result.push_back(root.value());
        }
    }
    return result;
}

UtreexoHash UtreexoStump::getCommitment() const {
    // Canonical commitment v2: SHA256(numLeaves_LE64 || slot[0] || slot[1] || ... || slot[63])
    // Must match UtreexoForest::getCommitment() exactly!
    // Each slot is 32 bytes: the root hash if present, or 32 zero bytes if absent.
    // numLeaves is committed so the forest shape is unambiguous.
    // Total preimage: 8 + 64*32 = 2056 bytes.
    static constexpr size_t NUM_SLOTS = 64;
    static const UtreexoHash ZERO_ROOT(32, 0);

    std::vector<uint8_t> preimage;
    preimage.reserve(8 + NUM_SLOTS * 32);

    // 1. numLeaves as 8 bytes little-endian
    uint64_t n = numLeaves_;
    for (int i = 0; i < 8; ++i) {
        preimage.push_back(static_cast<uint8_t>(n & 0xFF));
        n >>= 8;
    }

    // 2. 64 fixed root slots (32 bytes each) — roots_ is std::array<optional, 64>
    for (size_t h = 0; h < NUM_SLOTS; ++h) {
        if (roots_[h].has_value()) {
            const auto& root = roots_[h].value();
            preimage.insert(preimage.end(), root.begin(), root.end());
        } else {
            preimage.insert(preimage.end(), ZERO_ROOT.begin(), ZERO_ROOT.end());
        }
    }

    return SHA256_Hash(preimage);
}

size_t UtreexoStump::getNumRoots() const {
    size_t count = 0;
    for (const auto& root : roots_) {
        if (root.has_value()) {
            count++;
        }
    }
    return count;
}

bool UtreexoStump::verifyCommitment(const UtreexoHash& expected) const {
    return getCommitment() == expected;
}

// ═══════════════════════════════════════════════════════════════════════════
// Serialization
// ═══════════════════════════════════════════════════════════════════════════

std::vector<uint8_t> UtreexoStump::serialize() const {
    std::vector<uint8_t> data;

    // numLeaves (8 bytes LE)
    for (int i = 0; i < 8; ++i) {
        data.push_back((numLeaves_ >> (i * 8)) & 0xFF);
    }

    // Count non-empty roots
    uint8_t numRoots = 0;
    for (const auto& root : roots_) {
        if (root.has_value()) numRoots++;
    }
    data.push_back(numRoots);

    // For each non-empty root: height (1 byte) + hash (32 bytes)
    for (size_t h = 0; h < STUMP_MAX_ROOTS; ++h) {
        if (roots_[h].has_value()) {
            data.push_back(static_cast<uint8_t>(h));
            const auto& hash = roots_[h].value();
            data.insert(data.end(), hash.begin(), hash.end());
        }
    }

    return data;
}

UtreexoStump UtreexoStump::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 9) {
        throw std::runtime_error("Stump data too short");
    }

    UtreexoStump stump;

    // numLeaves (8 bytes LE)
    stump.numLeaves_ = 0;
    for (int i = 0; i < 8; ++i) {
        stump.numLeaves_ |= (static_cast<uint64_t>(data[i]) << (i * 8));
    }

    // numRoots
    uint8_t numRoots = data[8];

    // Read each root
    size_t offset = 9;
    for (uint8_t i = 0; i < numRoots; ++i) {
        if (offset + 33 > data.size()) {
            throw std::runtime_error("Stump data truncated");
        }

        uint8_t height = data[offset++];
        if (height >= STUMP_MAX_ROOTS) {
            throw std::runtime_error("Invalid root height");
        }

        UtreexoHash hash(data.begin() + offset, data.begin() + offset + 32);
        offset += 32;

        stump.roots_[height] = hash;
    }

    return stump;
}

size_t UtreexoStump::serializedSize() const {
    return 8 + 1 + getNumRoots() * 33;  // numLeaves + count + (height + hash) per root
}

// ═══════════════════════════════════════════════════════════════════════════
// Debugging
// ═══════════════════════════════════════════════════════════════════════════

std::string UtreexoStump::toString() const {
    std::ostringstream ss;
    ss << "UtreexoStump{leaves=" << numLeaves_ << ", roots=[";

    bool first = true;
    for (size_t h = 0; h < STUMP_MAX_ROOTS; ++h) {
        if (roots_[h].has_value()) {
            if (!first) ss << ", ";
            first = false;
            ss << "h" << h << ":";
            const auto& hash = roots_[h].value();
            for (size_t i = 0; i < std::min(size_t(4), hash.size()); ++i) {
                ss << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(hash[i]);
            }
            ss << "...";
        }
    }

    ss << "]}";
    return ss.str();
}

bool UtreexoStump::operator==(const UtreexoStump& other) const {
    if (numLeaves_ != other.numLeaves_) return false;

    for (size_t i = 0; i < STUMP_MAX_ROOTS; ++i) {
        if (roots_[i].has_value() != other.roots_[i].has_value()) return false;
        if (roots_[i].has_value() && roots_[i].value() != other.roots_[i].value()) {
            return false;
        }
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Internal Helpers
// ═══════════════════════════════════════════════════════════════════════════

UtreexoHash UtreexoStump::parentHash(const UtreexoHash& left, const UtreexoHash& right) {
    return HashNode(left, right);
}

uint8_t UtreexoStump::getTreeHeight(uint64_t position) const {
    // Determine which tree contains this position
    // Trees are ordered largest-first in position space (MSB→LSB scan)
    // Matches UtreexoForest::getTreeHeight() at utreexo_accumulator.cpp:1413

    uint64_t n = numLeaves_;
    if (n == 0) return 0;

    // Find highest set bit
    int maxBit = 63;
    while (maxBit >= 0 && ((n >> maxBit) & 1) == 0) {
        maxBit--;
    }

    // Scan MSB to LSB — largest tree occupies lowest positions
    uint64_t offset = 0;
    for (int h = maxBit; h >= 0; h--) {
        if ((n >> h) & 1) {
            uint64_t treeSize = 1ULL << h;
            if (position >= offset && position < offset + treeSize) {
                return static_cast<uint8_t>(h);
            }
            offset += treeSize;
        }
    }

    return 0;
}

uint8_t UtreexoStump::getRootIndexForPosition(uint64_t position) const {
    // Find which root index this position rolls up to
    // Trees are ordered largest-first (MSB→LSB), matching forest layout

    uint64_t n = numLeaves_;
    if (n == 0) return 0;

    // Find highest set bit
    int maxBit = 63;
    while (maxBit >= 0 && ((n >> maxBit) & 1) == 0) {
        maxBit--;
    }

    // Scan MSB to LSB — largest tree occupies lowest positions
    uint64_t offset = 0;
    for (int h = maxBit; h >= 0; h--) {
        if ((n >> h) & 1) {
            uint64_t treeSize = 1ULL << h;
            if (position < offset + treeSize) {
                return static_cast<uint8_t>(h);
            }
            offset += treeSize;
        }
    }

    return 0;
}

UtreexoHash UtreexoStump::computeRoot(
    const UtreexoHash& leafHash,
    const std::vector<UtreexoHash>& siblings,
    uint64_t position
) const {
    if (siblings.empty()) {
        return leafHash;  // Leaf is the root (single-node tree)
    }

    UtreexoHash current = leafHash;

    for (size_t i = 0; i < siblings.size(); ++i) {
        // Determine if current node is left or right child
        // based on the bit at this level
        bool isRight = (position >> i) & 1;

        if (isRight) {
            // Current is right child: parent = Hash(sibling || current)
            current = parentHash(siblings[i], current);
        } else {
            // Current is left child: parent = Hash(current || sibling)
            current = parentHash(current, siblings[i]);
        }
    }

    return current;
}

std::vector<UtreexoHash> UtreexoStump::extractSiblings(
    size_t targetIndex,
    const std::vector<UtreexoHash>& targets,
    const std::vector<uint64_t>& positions,
    const std::vector<UtreexoHash>& proof_hashes
) const {
    // Sequential (non-deduplicated) proof format: proof_hashes are laid out
    // as [target0_sib0, ..., target0_sibN, target1_sib0, ...].
    // Each target gets treeHeight siblings. This is the canonical format
    // produced by per-target proof generation in the forest.
    uint64_t pos = positions[targetIndex];
    uint8_t treeHeight = getTreeHeight(pos);

    // Number of siblings = tree height (one per level)
    size_t numSiblings = treeHeight;

    // Calculate offset into proof_hashes
    size_t offset = 0;
    for (size_t i = 0; i < targetIndex; ++i) {
        offset += getTreeHeight(positions[i]);
    }

    // Extract siblings
    std::vector<UtreexoHash> siblings;
    for (size_t i = 0; i < numSiblings && offset + i < proof_hashes.size(); ++i) {
        siblings.push_back(proof_hashes[offset + i]);
    }

    return siblings;
}

UtreexoHash UtreexoStump::computeRootAfterDeletion(
    const UtreexoHash& deletedLeaf,
    const std::vector<UtreexoHash>& siblings,
    uint64_t position
) const {
    // Delegate to applyDeletions on a temporary copy
    UtreexoStump temp = *this;
    if (!temp.applyDeletions({deletedLeaf}, {position}, siblings)) {
        return UtreexoHash(32, 0);
    }
    uint8_t rootIdx = getRootIndexForPosition(position);
    if (temp.roots_[rootIdx].has_value()) {
        return temp.roots_[rootIdx].value();
    }
    return UtreexoHash(32, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Deletion Algorithm
// ═══════════════════════════════════════════════════════════════════════════

uint64_t UtreexoStump::getTreeStartPosition(uint8_t rootIndex) const {
    uint64_t n = numLeaves_;
    if (n == 0) return 0;

    int maxBit = 63;
    while (maxBit >= 0 && ((n >> maxBit) & 1) == 0) maxBit--;

    uint64_t offset = 0;
    for (int h = maxBit; h >= 0; h--) {
        if ((n >> h) & 1) {
            if (static_cast<uint8_t>(h) == rootIndex) {
                return offset;
            }
            offset += (1ULL << h);
        }
    }
    return 0;
}

bool UtreexoStump::applyDeletions(
    const std::vector<UtreexoHash>& targets,
    const std::vector<uint64_t>& positions,
    const std::vector<UtreexoHash>& proof_hashes
) {
    if (targets.empty()) return true;
    if (positions.size() != targets.size()) return false;

    // Group target indices by tree (root index = tree height)
    std::unordered_map<uint8_t, std::vector<size_t>> by_tree;
    for (size_t i = 0; i < positions.size(); ++i) {
        uint8_t rootIdx = getRootIndexForPosition(positions[i]);
        by_tree[rootIdx].push_back(i);
    }

    // Process each affected tree
    for (auto& [rootIdx, indices] : by_tree) {
        if (!roots_[rootIdx].has_value()) return false;

        uint8_t H = rootIdx;  // tree height
        uint64_t tree_start = getTreeStartPosition(rootIdx);

        // Guard #2: sort targets within tree by ascending local position
        std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
            return positions[a] < positions[b];
        });

        // Sparse level maps: level_nodes[L][local_pos] = optional<hash>
        // nullopt = deleted/empty, has_value = known hash
        std::vector<std::unordered_map<uint64_t, std::optional<UtreexoHash>>> level_nodes(H + 1);

        // Step 1: Mark deleted leaves as nullopt
        for (size_t idx : indices) {
            uint64_t local_pos = positions[idx] - tree_start;
            level_nodes[0][local_pos] = std::nullopt;
        }

        // Step 2: Place proof siblings (Guard #1: keyed by (level, position))
        for (size_t idx : indices) {
            uint64_t local_pos = positions[idx] - tree_start;
            auto siblings = extractSiblings(idx, targets, positions, proof_hashes);

            for (size_t level = 0; level < siblings.size() && level < H; ++level) {
                uint64_t sib_pos = (local_pos >> level) ^ 1;
                // Only place if position not already occupied
                if (level_nodes[level].find(sib_pos) == level_nodes[level].end()) {
                    level_nodes[level][sib_pos] = siblings[level];
                }
            }
        }

        // Step 3: Bottom-up recomputation
        for (uint8_t level = 0; level < H; ++level) {
            std::unordered_set<uint64_t> parent_positions;
            for (const auto& [pos, _] : level_nodes[level]) {
                parent_positions.insert(pos >> 1);
            }

            for (uint64_t pp : parent_positions) {
                uint64_t left_pos = pp << 1;
                uint64_t right_pos = left_pos | 1;

                auto left_it = level_nodes[level].find(left_pos);
                auto right_it = level_nodes[level].find(right_pos);

                bool have_left = (left_it != level_nodes[level].end());
                bool have_right = (right_it != level_nodes[level].end());

                std::optional<UtreexoHash> left_val = have_left ? left_it->second : std::nullopt;
                std::optional<UtreexoHash> right_val = have_right ? right_it->second : std::nullopt;

                std::optional<UtreexoHash> parent;
                if (left_val.has_value() && right_val.has_value()) {
                    parent = parentHash(left_val.value(), right_val.value());
                } else if (left_val.has_value()) {
                    parent = parentHash(left_val.value(), ZERO_HASH);
                } else if (right_val.has_value()) {
                    parent = parentHash(ZERO_HASH, right_val.value());
                } else {
                    parent = std::nullopt;  // entire subtree deleted
                }

                level_nodes[level + 1][pp] = parent;
            }
        }

        // Step 4: Extract new root
        auto root_it = level_nodes[H].find(0);
        if (root_it != level_nodes[H].end()) {
            roots_[rootIdx] = root_it->second;
        }
        // If root not in map, tree was not fully affected — root unchanged
    }

    // numLeaves_ unchanged (deletions don't reduce leaf count)
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Block Processing Helper
// ═══════════════════════════════════════════════════════════════════════════

bool ProcessBlockWithStump(
    UtreexoStump& stump,
    const BlockUtreexoProof& proof,
    const std::vector<UtreexoHash>& additions,
    const UtreexoHash& expectedCommitmentAfter
) {
    // Step 1: Verify proof against current state
    if (!proof.isEmpty() && !stump.verifyBlockProof(proof)) {
        return false;
    }

    // Step 2: Apply modifications
    if (!stump.modify(proof, additions)) {
        return false;
    }

    // Step 3: Verify resulting commitment matches expected
    if (!stump.verifyCommitment(expectedCommitmentAfter)) {
        return false;
    }

    return true;
}

} // namespace consensus
} // namespace dinero
