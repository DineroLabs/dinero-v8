#pragma once
#include "consensus/utreexo_stump.h"
#include <map>
#include <set>

namespace dinero::consensus {
// Pure canonical-empty-root transition. This validates Merkle arithmetic, not
// chain selection or coin ownership. Its caller must own an authenticated
// selected parent and bind every input and addition to the validated block.
// Historical noncanonical stump methods and contracts are deliberately separate.
// Failure (including an exception) never changes the supplied parent.
inline std::optional<UtreexoStump> OrchardCanonicalStumpTransition(
    const UtreexoStump& parent, const BlockUtreexoProof& proof,
    const std::vector<UtreexoHash>& additions,
    const UtreexoHash& expected_commitment) {
    const auto leaves = parent.getNumLeaves();
    const UtreexoHash zero(32, 0);
    if (leaves > MAX_UTREEXO_LEAVES ||
        additions.size() > MAX_UTREEXO_LEAVES - leaves ||
        proof.numLeaves != leaves || proof.targets.size() != proof.positions.size() ||
        expected_commitment.size() != 32) return {};
    auto roots = parent.getAllRoots();
    for (unsigned h = 0; h < 64; ++h) {
        if (roots[h].has_value() != bool((leaves >> h) & 1)) return {};
        if (roots[h] && roots[h]->size() != 32) return {};
    }
    std::set<UtreexoHash> new_leaves;
    for (const auto& leaf : additions)
        if (leaf.size() != 32 || leaf == zero || !new_leaves.insert(leaf).second) return {};
    // Roots cannot prove absence of a previously created output. The canonical
    // caller must use its complete authenticated transaction-history catalog to
    // refuse reused transaction IDs before deriving these addition leaves.

    struct TreeProof {
        // Positions are local to a single original tree, indexed by level.
        std::vector<std::map<uint64_t, UtreexoHash>> original;
        std::set<uint64_t> deletions;
    };
    std::map<unsigned, TreeProof> trees;
    std::set<uint64_t> positions;
    size_t sibling_offset = 0;
    for (size_t i = 0; i < proof.targets.size(); ++i) {
        const auto position = proof.positions[i];
        const auto& leaf = proof.targets[i];
        if (position >= leaves || leaf.size() != 32 || leaf == zero ||
            !positions.insert(position).second) return {};
        // Original trees occupy position space largest-first. Bounds use
        // subtraction; no addition can wrap near a tree boundary.
        uint64_t start = 0;
        unsigned height = 0;
        bool located = false;
        for (int h = 63; h >= 0; --h) {
            if (!((leaves >> h) & 1)) continue;
            const uint64_t size = uint64_t(1) << h;
            if (position >= start && position - start < size) {
                height = unsigned(h); located = true; break;
            }
            start += size;
        }
        if (!located || sibling_offset > proof.proof_hashes.size() ||
            height > proof.proof_hashes.size() - sibling_offset) return {};
        auto& tree = trees[height];
        if (tree.original.empty()) tree.original.resize(height + 1);
        const auto place = [&](unsigned level, uint64_t pos, const UtreexoHash& hash) {
            auto [it, inserted] = tree.original[level].emplace(pos, hash);
            return inserted || it->second == hash;
        };
        uint64_t local = position - start;
        tree.deletions.insert(local);
        UtreexoHash current = leaf;
        if (!place(0, local, current)) return {};
        for (unsigned level = 0; level < height; ++level) {
            const auto& sibling = proof.proof_hashes[sibling_offset++];
            if (sibling.size() != 32 || !place(level, local ^ 1, sibling)) return {};
            current = (local & 1) ? HashNode(sibling, current) : HashNode(current, sibling);
            local >>= 1;
            if (!place(level + 1, local, current)) return {};
        }
        if (current != *roots[height]) return {};
    }
    // Includes empty proofs: extra positions and extra siblings never disappear
    // through an isEmpty shortcut, and every supplied sibling must be consumed.
    if (sibling_offset != proof.proof_hashes.size()) return {};

    for (auto& [height, tree] : trees) {
        std::map<uint64_t, UtreexoHash> changed;
        for (const auto position : tree.deletions) changed.emplace(position, zero);
        for (unsigned level = 0; level < height; ++level) {
            std::map<uint64_t, UtreexoHash> next;
            for (const auto& [position, ignored] : changed) {
                const uint64_t at = position >> 1;
                if (next.contains(at)) continue;
                const auto child = [&](uint64_t pos) -> const UtreexoHash* {
                    const auto updated = changed.find(pos);
                    if (updated != changed.end()) return &updated->second;
                    const auto original = tree.original[level].find(pos);
                    return original == tree.original[level].end() ? nullptr : &original->second;
                };
                const auto* left = child(at << 1);
                const auto* right = child((at << 1) | 1);
                if (!left || !right) return {};
                // Fully deleted subtrees remain recursive HashNode(zero,zero)
                // trees. A missing node is an error, never a zero subtree.
                next.emplace(at, HashNode(*left, *right));
            }
            changed.swap(next);
        }
        if (changed.size() != 1 || !changed.contains(0)) return {};
        roots[height] = changed.at(0);
    }
    auto next = UtreexoStump::fromRoots(
        std::vector<std::optional<UtreexoHash>>(roots.begin(), roots.end()), leaves);
    // Shape and aggregate capacity were checked before any carry addition.
    next.add(additions);
    if (next.getCommitment() != expected_commitment) return {};
    return next;
}
} // namespace dinero::consensus
