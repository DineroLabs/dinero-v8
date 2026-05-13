#pragma once

#include <vector>
#include <cstdint>
#include <optional>
#include <array>
#include <string>
#include "consensus/utreexo_accumulator.h"  // For UtreexoHash, HashNode, BlockUtreexoProof

namespace dinero {
namespace consensus {

/**
 * @file utreexo_stump.h
 * @brief Utreexo Stump: Roots-Only Accumulator for Compact State Nodes (CSN)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * WHAT IS STUMP?
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Stump is a minimal Utreexo accumulator that stores ONLY the forest roots.
 * It's designed for Compact State Nodes (CSN) - lightweight clients that:
 *   - Verify proofs (can validate blocks)
 *   - Cannot generate proofs (requires full Pollard)
 *   - Use ~2KB storage instead of gigabytes
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │                     POLLARD vs STUMP COMPARISON                         │
 * ├─────────────────────────────────────────────────────────────────────────┤
 * │                        │ Pollard (Bridge Node)  │ Stump (CSN/Mobile)    │
 * │────────────────────────┼────────────────────────┼───────────────────────│
 * │ Storage                │ Full tree (~GB)        │ Roots only (~2KB)     │
 * │ Verify proofs          │ ✓                      │ ✓                     │
 * │ Generate proofs        │ ✓                      │ ✗                     │
 * │ Update state           │ ✓                      │ ✓ (with proof data)   │
 * │ Mobile-friendly        │ ✗                      │ ✓                     │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * HOW IT WORKS
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Utreexo represents N UTXOs as a forest of perfect binary Merkle trees.
 * The number of trees = number of 1-bits in binary(N).
 *
 * Example: 13 UTXOs = 1101₂ = 8 + 4 + 1 = three trees with roots r₃, r₂, r₀
 *
 *        r₃ (8 leaves)     r₂ (4 leaves)     r₀ (1 leaf)
 *           /\                 /\                |
 *          /  \               /  \              leaf
 *         /    \             /    \
 *        /      \           /      \
 *       ○        ○         ○        ○
 *      /\       /\        /\       /\
 *     ○  ○     ○  ○      ○  ○     ○  ○
 *
 * Stump stores: [r₃, r₂, _, r₀] (roots array, indexed by tree height)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * VERIFICATION
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * To verify a proof:
 * 1. Start with leaf hash
 * 2. Hash with siblings from proof, walking up the tree
 * 3. Final hash must equal one of the stored roots
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * STATE UPDATE (The Key Innovation)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Stump can update roots WITHOUT storing the full tree by using proof data:
 *
 * DELETION:
 *   When verifying a deletion proof, we compute intermediate hashes.
 *   After "deleting" the leaf, we recompute the path with empty/modified nodes.
 *   The proof siblings give us everything needed to compute the new root.
 *
 * ADDITION:
 *   New leaves are added at position numLeaves++.
 *   When two trees of the same height exist, they merge:
 *     new_root = Hash(left_root || right_root)
 *   This only requires the roots, not the full trees.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * USAGE
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   // Initialize from checkpoint (e.g., block 1's commitment)
 *   UtreexoStump stump = UtreexoStump::fromRoots(roots, numLeaves);
 *
 *   // Process a block
 *   if (stump.verifyBatchProof(proof)) {
 *       stump.applyBlockUpdate(deletions, additions, proof);
 *   }
 *
 *   // Get commitment for header verification
 *   auto commitment = stump.getCommitment();
 *
 * References:
 * - Utreexo paper: https://eprint.iacr.org/2019/611.pdf
 * - rustreexo stump: https://github.com/mit-dci/rustreexo
 */

// Maximum tree height (log2 of max possible leaves)
// 64 trees can represent up to 2^64 leaves
static constexpr size_t STUMP_MAX_ROOTS = 64;

/**
 * @brief Utreexo Stump - Roots-only accumulator for lightweight verification
 *
 * This is the CSN (Compact State Node) accumulator. It stores only the
 * forest roots and can verify/update state using provided proof data.
 *
 * Thread-safety: Not thread-safe (caller must synchronize)
 */
class UtreexoStump {
public:
    // ═══════════════════════════════════════════════════════════════════════
    // Construction
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Construct empty stump (genesis state)
     */
    UtreexoStump();

    /**
     * @brief Construct stump from known roots
     *
     * Used for:
     * - Loading from checkpoint
     * - Restoring from serialized state
     * - Testing
     *
     * @param roots Forest roots (indexed by tree height, empty slots = nullopt)
     * @param numLeaves Total leaves in accumulator
     */
    static UtreexoStump fromRoots(
        const std::vector<std::optional<UtreexoHash>>& roots,
        uint64_t numLeaves
    );

    /**
     * @brief Construct stump from commitment + leaf count
     *
     * IMPORTANT (v2 commitment):
     * The commitment is SHA256(numLeaves_LE64 || 64 fixed root slots), so
     * roots cannot be recovered from commitment alone.
     *
     * This constructor only preserves leaf count and returns an "opaque" stump
     * with unknown roots (all slots empty). It cannot verify proofs and its
     * getCommitment() is not expected to equal the input commitment.
     *
     * Use fromRoots() (or fromForest()) whenever verification is required.
     *
     * @param commitment 32-byte accumulator commitment
     * @param numLeaves Total leaves in accumulator
     */
    static UtreexoStump fromCommitment(
        const UtreexoHash& commitment,
        uint64_t numLeaves
    );

    /**
     * @brief Extract stump from full forest (for bridge nodes creating CSN data)
     *
     * @param forest Full Utreexo forest
     * @return Stump with only the roots
     */
    static UtreexoStump fromForest(const UtreexoForest& forest);

    // ═══════════════════════════════════════════════════════════════════════
    // Verification
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Verify a batch proof against current roots
     *
     * This is the core verification function. It proves that the given
     * target leaves exist in the accumulator.
     *
     * Algorithm:
     * 1. For each (target, position) pair:
     *    a. Determine which tree contains this position
     *    b. Extract siblings from proof_hashes
     *    c. Compute path from leaf to root
     *    d. Verify computed root matches stored root
     *
     * Complexity: O(k * log n) where k = targets.size()
     *
     * @param targets Leaf hashes being proven
     * @param positions Leaf positions in forest (one per target)
     * @param proof_hashes Merkle proof siblings
     * @return true if all targets verified successfully
     */
    bool verifyBatchProof(
        const std::vector<UtreexoHash>& targets,
        const std::vector<uint64_t>& positions,
        const std::vector<UtreexoHash>& proof_hashes
    ) const;

    /**
     * @brief Verify a BlockUtreexoProof
     *
     * Convenience wrapper for block-level proofs.
     *
     * @param proof Block proof containing targets, positions, proof_hashes
     * @return true if proof is valid
     */
    bool verifyBlockProof(const BlockUtreexoProof& proof) const;

    /**
     * @brief Verify a complete state transition (transition proof path)
     *
     * Verifies that:
     * 1. Deletion targets exist in current state (batch proof)
     * 2. Starting from roots_after_deletions + applying additions
     *    produces the expected commitment
     *
     * Does NOT mutate this stump.
     *
     * @param proof Deletion proof (targets, positions, proof_hashes)
     * @param roots_after_deletions Intermediate roots after deletions
     * @param additions New leaf hashes to add
     * @param expectedCommitmentAfter Expected commitment after transition
     * @return true if transition is valid
     */
    bool verifyTransition(
        const BlockUtreexoProof& proof,
        const std::vector<UtreexoHash>& roots_after_deletions,
        const std::vector<UtreexoHash>& additions,
        const UtreexoHash& expectedCommitmentAfter
    ) const;

    // ═══════════════════════════════════════════════════════════════════════
    // State Update
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Apply deletions and additions to update roots
     *
     * This is the key function that allows Stump to track state changes
     * WITHOUT storing the full tree.
     *
     * Algorithm for deletions:
     * 1. Verify all deletion proofs (must be valid to update)
     * 2. For each deletion, use proof siblings to compute new subtree root
     *    with the target removed (replaced with "empty" marker)
     * 3. Deletions are "lazy" - we track deleted positions but roots
     *    only change when trees merge
     *
     * Algorithm for additions:
     * 1. Each new leaf is added at position numLeaves
     * 2. If this creates two trees of same height, merge:
     *    new_root = Hash(existing_root || new_leaf_or_subtree)
     * 3. Repeat merging until no two trees have same height
     *
     * @param proof Block proof (must be verified first)
     * @param additions New leaf hashes to add (created UTXOs)
     * @return true if update succeeded
     */
    bool modify(
        const BlockUtreexoProof& proof,
        const std::vector<UtreexoHash>& additions
    );

    /**
     * @brief Add new leaves to the accumulator
     *
     * Simplified addition-only update (no deletions).
     * Used during IBD when trusting block data.
     *
     * @param additions Leaf hashes to add
     */
    void add(const std::vector<UtreexoHash>& additions);

    /**
     * @brief Add a single leaf
     */
    void addSingle(const UtreexoHash& leaf);

    // ═══════════════════════════════════════════════════════════════════════
    // Queries
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Get current forest roots
     *
     * Returns non-empty roots only, in height order (smallest tree first).
     */
    std::vector<UtreexoHash> getRoots() const;

    /**
     * @brief Get all roots including empty slots
     *
     * Returns full roots array with nullopt for empty slots.
     * Useful for serialization.
     */
    const std::array<std::optional<UtreexoHash>, STUMP_MAX_ROOTS>& getAllRoots() const {
        return roots_;
    }

    /**
     * @brief Get 32-byte commitment hash
     *
     * Commitment = SHA256(root0 || root1 || ... || rootN)
     * This matches the value in block headers.
     */
    UtreexoHash getCommitment() const;

    /**
     * @brief Get number of leaves
     */
    uint64_t getNumLeaves() const {
        return numLeaves_;
    }

    /**
     * @brief Get number of active roots (non-empty)
     */
    size_t getNumRoots() const;

    /**
     * @brief Check if stump is empty (genesis state)
     */
    bool isEmpty() const {
        return numLeaves_ == 0;
    }

    /**
     * @brief Verify commitment matches current state
     */
    bool verifyCommitment(const UtreexoHash& expected) const;

    // ═══════════════════════════════════════════════════════════════════════
    // Serialization
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Serialize stump to bytes
     *
     * Format:
     * - numLeaves (8 bytes LE)
     * - numRoots (1 byte) - count of non-empty roots
     * - For each non-empty root:
     *   - height (1 byte)
     *   - hash (32 bytes)
     *
     * Maximum size: 8 + 1 + 64*(1+32) = 2121 bytes
     */
    std::vector<uint8_t> serialize() const;

    /**
     * @brief Deserialize stump from bytes
     */
    static UtreexoStump deserialize(const std::vector<uint8_t>& data);

    /**
     * @brief Get serialized size in bytes
     */
    size_t serializedSize() const;

    // ═══════════════════════════════════════════════════════════════════════
    // Debugging / Testing
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Get human-readable state description
     */
    std::string toString() const;

    /**
     * @brief Compare two stumps for equality
     */
    bool operator==(const UtreexoStump& other) const;
    bool operator!=(const UtreexoStump& other) const { return !(*this == other); }

private:
    // Forest roots indexed by tree height
    // roots_[h] = root of tree with 2^h leaves, or nullopt if no such tree
    std::array<std::optional<UtreexoHash>, STUMP_MAX_ROOTS> roots_;

    // Total leaves in accumulator (determines forest structure)
    uint64_t numLeaves_;

    // ───────────────────────────────────────────────────────────────────────
    // Internal Helpers
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Compute parent hash from two children
     */
    static UtreexoHash parentHash(const UtreexoHash& left, const UtreexoHash& right);

    /**
     * @brief Get tree height for a given position
     *
     * Returns which tree (by height) contains the given leaf position.
     */
    uint8_t getTreeHeight(uint64_t position) const;

    /**
     * @brief Get root index for a given tree height
     *
     * Maps from leaf position to which root it rolls up to.
     */
    uint8_t getRootIndexForPosition(uint64_t position) const;

    /**
     * @brief Compute root from leaf and proof path
     *
     * Given a leaf hash and its Merkle siblings, compute the root.
     * Used during both verification and deletion.
     *
     * @param leafHash Starting leaf hash
     * @param siblings Proof path (sibling hashes)
     * @param position Leaf position (determines left/right at each level)
     * @return Computed root hash
     */
    UtreexoHash computeRoot(
        const UtreexoHash& leafHash,
        const std::vector<UtreexoHash>& siblings,
        uint64_t position
    ) const;

    /**
     * @brief Extract siblings for a target from batched proof
     *
     * @param targetIndex Index of target in batch
     * @param targets All targets in batch
     * @param positions All positions in batch
     * @param proof_hashes Batched proof hashes
     * @return Siblings for this specific target
     */
    std::vector<UtreexoHash> extractSiblings(
        size_t targetIndex,
        const std::vector<UtreexoHash>& targets,
        const std::vector<uint64_t>& positions,
        const std::vector<UtreexoHash>& proof_hashes
    ) const;

    /**
     * @brief Compute new root after deletion (deprecated — delegates to applyDeletions)
     */
    UtreexoHash computeRootAfterDeletion(
        const UtreexoHash& deletedLeaf,
        const std::vector<UtreexoHash>& siblings,
        uint64_t position
    ) const;

    /**
     * @brief Apply batch deletions and update affected roots
     *
     * Core deletion algorithm: sparse bottom-up recomputation.
     * For each tree containing deletions:
     * 1. Mark deleted leaves as nullopt in sparse level map
     * 2. Place proof siblings at computed positions (ascending order within tree)
     * 3. Bottom-up: recompute parent hashes using ZERO_HASH for missing children
     * 4. Update roots_[h] with new root (may become nullopt if entire tree deleted)
     *
     * numLeaves_ is NOT changed (deletions don't reduce leaf count in Utreexo).
     */
    bool applyDeletions(
        const std::vector<UtreexoHash>& targets,
        const std::vector<uint64_t>& positions,
        const std::vector<UtreexoHash>& proof_hashes
    );

    /**
     * @brief Get starting global position of tree at given root index
     *
     * Trees are ordered MSB→LSB (largest first in position space).
     * Root index equals tree height.
     */
    uint64_t getTreeStartPosition(uint8_t rootIndex) const;
};

// ═══════════════════════════════════════════════════════════════════════════
// Block Processing Helper
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Process a block with Stump (CSN block validation)
 *
 * High-level function that:
 * 1. Verifies the block's Utreexo proof against current stump
 * 2. Updates stump state (deletions + additions)
 * 3. Verifies resulting commitment matches block header
 *
 * @param stump Stump to update (modified in place)
 * @param proof Block's Utreexo proof
 * @param additions New UTXOs created in this block (leaf hashes)
 * @param expectedCommitmentAfter Expected commitment after applying block
 * @return true if block is valid and stump updated
 */
bool ProcessBlockWithStump(
    UtreexoStump& stump,
    const BlockUtreexoProof& proof,
    const std::vector<UtreexoHash>& additions,
    const UtreexoHash& expectedCommitmentAfter
);

} // namespace consensus
} // namespace dinero
