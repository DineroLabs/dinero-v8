#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include "crypto/hash.h"
#include "primitives/uint256.h"

namespace dinero {

// Forward declaration for UtreexoTransitionProof::generate/computeAdditionHashes
struct Block;

namespace consensus {

/**
 * @file utreexo_accumulator.h
 * @brief Utreexo: Dynamic Hash-Based Accumulator for UTXO Commitments
 *
 * Phase 34.1: Core Accumulator Implementation
 *
 * Utreexo is a cryptographic accumulator that allows nodes to:
 * - Represent the entire UTXO set in ~1KB (instead of gigabytes)
 * - Sync in seconds instead of hours
 * - Run full nodes on mobile devices
 * - Provide cryptographic proofs of UTXO existence
 *
 * How it works:
 * - UTXOs are stored as leaves in a forest of perfect binary Merkle trees
 * - Each tree has height = power of 2
 * - Example: 5 UTXOs = tree(4) + tree(1) = forest with 2 roots
 * - Proofs are Merkle paths: log₂(n) hashes per UTXO
 * - Commitment is hash(root1 || root2 || ... || rootN)
 *
 * References:
 * - MIT Paper: https://eprint.iacr.org/2019/611.pdf
 * - Bitcoin implementation: https://github.com/mit-dci/utreexo
 * - Specification: https://github.com/utreexo/utreexo
 */

// ═══════════════════════════════════════════════════════════════════════════
// UtreexoHash: 32-byte SHA256 hash (renamed to avoid conflict with p2p::UtreexoHash)
// ═══════════════════════════════════════════════════════════════════════════

using UtreexoHash = std::vector<uint8_t>;  // 32 bytes

// Forward declarations
struct BlockUtreexoProof;

// ═══════════════════════════════════════════════════════════════════════════
// Hardening Constants (Medium Priority Security Bounds)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Maximum number of leaves in the Utreexo forest
 *
 * This bounds the UTXO set size. 2^40 = ~1 trillion UTXOs.
 * Bitcoin currently has ~100 million UTXOs. This gives 10,000x headroom.
 * Prevents integer overflow in position arithmetic.
 */
static constexpr uint64_t MAX_UTREEXO_LEAVES = (1ULL << 40);  // 2^40 = ~1 trillion

/**
 * @brief Maximum number of unique hashes in a compressed proof dictionary
 *
 * For a batch of N spends, proof size is O(N * log(total_leaves)).
 * With 40-bit leaves, max ~40 hashes per target.
 * 10000 hashes supports batches of ~250 spends.
 */
static constexpr uint32_t MAX_PROOF_DICTIONARY_SIZE = 10000;

/**
 * @brief Maximum number of proof hashes in a single BlockUtreexoProof
 *
 * Similar reasoning to dictionary size.
 */
static constexpr uint32_t MAX_PROOF_HASHES = 10000;

/**
 * @brief Maximum targets (spends) in a single block proof
 *
 * Bitcoin blocks have ~2500 transactions average, ~10 inputs each = ~25000.
 * This provides 4x headroom.
 */
static constexpr uint32_t MAX_PROOF_TARGETS = 100000;

/**
 * @brief Maximum tree height (log2 of MAX_UTREEXO_LEAVES)
 */
static constexpr uint8_t MAX_TREE_HEIGHT = 40;

/**
 * @brief Maximum raw byte size for a BlockUtreexoProof before deserialization
 *
 * Non-consensus DoS protection. Proofs exceeding this are rejected before
 * allocating any buffers. 4 MB is generous (~100k targets at 40 bytes each).
 * This is POLICY, not consensus - does not affect proof validity.
 */
static constexpr size_t MAX_UTREEXO_PROOF_BYTES = 4 * 1024 * 1024;  // 4 MB

// ═══════════════════════════════════════════════════════════════════════════
// Checked Arithmetic Helpers
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Checked addition - returns false on overflow
 */
inline bool checked_add(uint64_t a, uint64_t b, uint64_t& result) {
    if (a > UINT64_MAX - b) return false;
    result = a + b;
    return true;
}

/**
 * @brief Checked multiplication - returns false on overflow
 */
inline bool checked_mul(uint64_t a, uint64_t b, uint64_t& result) {
    if (b != 0 && a > UINT64_MAX / b) return false;
    result = a * b;
    return true;
}

/**
 * @brief Checked left shift - returns false if result would overflow
 */
inline bool checked_shift_left(uint64_t val, uint8_t shift, uint64_t& result) {
    if (shift >= 64) return false;
    if (val > (UINT64_MAX >> shift)) return false;
    result = val << shift;
    return true;
}

/**
 * @brief Checked increment - returns false on overflow
 */
inline bool checked_increment(uint64_t& val) {
    if (val == UINT64_MAX) return false;
    val++;
    return true;
}

// Hash function for UtreexoHash (required for unordered_map)
struct UtreexoHashHasher {
    std::size_t operator()(const UtreexoHash& hash) const noexcept {
        if (hash.size() < sizeof(std::size_t)) {
            return 0;
        }
        // Use first sizeof(size_t) bytes as hash
        std::size_t result = 0;
        std::memcpy(&result, hash.data(), sizeof(std::size_t));
        return result;
    }
};

/**
 * @brief Hash two child nodes to create parent node
 *
 * Parent = SHA256(left || right)
 * This is the standard Merkle tree hash function.
 */
UtreexoHash HashNode(const UtreexoHash& left, const UtreexoHash& right);

/**
 * @brief Hash a single UTXO to create a domain-separated leaf
 *
 * Leaf = SHA256("DINERO-UTXO-LEAF-v1" || txid || vout || amount || scriptPubKey)
 *
 * Security invariant:
 * - A live accumulator must never contain two different UTXOs with the same leaf hash.
 * - OutPoint binding (txid+vout) is part of the preimage to prevent leaf rebinding.
 *
 * Domain tag is 19 bytes ASCII, active from genesis (no legacy mode).
 * See: DINERO-UTREEXO-SPEC.md §3
 */
UtreexoHash HashUTXOLegacy(const uint256& txid, uint32_t vout,
                 uint64_t amount, const std::vector<uint8_t>& scriptPubKey);

/**
 * @brief Hash a UTXO leaf with authenticated maturity metadata.
 *
 * Experimental v2 leaf format:
 * Leaf = SHA256("DINERO-UTXO-LEAF-v2" || txid || vout || amount ||
 *               scriptPubKey || created_height || flags)
 *
 * flags bit 0 = coinbase. Binding created_height + coinbase status lets a
 * stateless verifier enforce coinbase maturity from proof-supplied metadata:
 * dishonest metadata computes a different leaf and fails the accumulator proof.
 */
UtreexoHash HashUTXOV2(const uint256& txid, uint32_t vout,
                 uint64_t amount, const std::vector<uint8_t>& scriptPubKey,
                 uint32_t created_height, bool is_coinbase);

/**
 * @brief Hash a UTXO using the leaf format active when it was created.
 */
UtreexoHash HashUTXOForCreationHeight(const uint256& txid, uint32_t vout,
                 uint64_t amount, const std::vector<uint8_t>& scriptPubKey,
                 uint32_t created_height, bool is_coinbase);

// ═══════════════════════════════════════════════════════════════════════════
// Utreexo Proof: Merkle path proving UTXO existence
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Proof that a UTXO exists in the accumulator
 *
 * Contains:
 * - Merkle path (sibling hashes from leaf to root)
 * - Target position (which leaf in the forest)
 * - Total number of leaves (for validation)
 *
 * Proof size: O(log n) hashes
 */
struct UtreexoProof {
    std::vector<UtreexoHash> siblings;  // Sibling hashes (Merkle path)
    uint64_t position;              // Leaf position in forest
    uint64_t numLeaves;             // Total leaves in forest

    UtreexoProof() : position(0), numLeaves(0) {}

    /**
     * @brief Verify this proof against a leaf hash and forest roots
     *
     * @param leafHash Hash of the UTXO being verified
     * @param roots Current forest roots
     * @return true if proof is valid
     */
    bool verify(const UtreexoHash& leafHash, const std::vector<UtreexoHash>& roots) const;

    /**
     * @brief Serialize proof to bytes
     */
    std::vector<uint8_t> serialize() const;

    /**
     * @brief Deserialize proof from bytes
     */
    static UtreexoProof deserialize(const std::vector<uint8_t>& data);

    /**
     * @brief Get proof size in bytes
     */
    size_t size() const {
        return siblings.size() * 32 + 16;  // siblings + position + numLeaves
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Utreexo Forest: Dynamic Merkle forest accumulator
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Utreexo accumulator: Forest of perfect binary Merkle trees
 *
 * The forest represents N UTXOs as a set of perfect binary trees.
 * - N is decomposed into powers of 2: N = 2^k1 + 2^k2 + ... + 2^kn
 * - Each power of 2 corresponds to a tree root
 * - Example: 5 = 4 + 1 = 2^2 + 2^0 → 2 roots (tree of 4 leaves, tree of 1 leaf)
 *
 * Operations:
 * - add(): O(log n) - Add new UTXO leaf
 * - remove(): O(log n) - Remove UTXO leaf (requires proof)
 * - prove(): O(log n) - Generate proof for UTXO
 * - getRoots(): O(1) - Get current forest roots
 * - getCommitment(): O(1) - Get single 32-byte commitment hash
 *
 * Thread-safety: Not thread-safe (caller must synchronize)
 */
class UtreexoForest {
public:
    UtreexoForest();
    ~UtreexoForest();

    // Rule of Five, stated explicitly.
    //
    // `~UtreexoForest()` is user-declared (and empty), which under the Rule of
    // Five SUPPRESSES the implicit move constructor and move assignment. Every
    // rvalue then bound to the copy constructor, so `std::move(forest)` silently
    // deep-copied the roots/nodes vectors, the leaf-position map for every leaf,
    // and the deleted-position set.
    //
    // That is expensive and invisible: `std::is_move_constructible` still
    // reports true, because a const-lvalue-ref binds to an rvalue. Only the
    // NOTHROW form discriminates, which is what UtreexoForestMove asserts.
    //
    // Every member is a RAII container and the destructor does nothing, so
    // defaulting these is safe. Copying stays available and deliberate — the
    // validation paths use clone() and the copy constructor on purpose.
    UtreexoForest(UtreexoForest&&) noexcept = default;
    UtreexoForest& operator=(UtreexoForest&&) noexcept = default;
    UtreexoForest(const UtreexoForest&) = default;
    UtreexoForest& operator=(const UtreexoForest&) = default;

    // ───────────────────────────────────────────────────────────────────────
    // Core Operations
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Add a new UTXO to the accumulator
     *
     * Complexity: O(log n)
     * - Adds leaf to forest
     * - Merges trees when two trees of same height exist
     * - Updates roots
     *
     * @param leafHash Hash of the UTXO (from HashUTXO)
     * @return position of added leaf in forest
     */
    uint64_t add(const UtreexoHash& leafHash);

    /**
     * @brief Remove a UTXO from the accumulator
     *
     * Complexity: O(log n)
     * - Verifies proof is valid
     * - Removes leaf from forest
     * - Updates roots
     *
     * @param leafHash Hash of the UTXO being removed
     * @param proof Merkle proof of UTXO existence
     * @return true if removal succeeded
     */
    bool remove(const UtreexoHash& leafHash, const UtreexoProof& proof);

    /**
     * @brief Remove a UTXO from the accumulator at a known position.
     *
     * This is the internal variant used by block validation and mining
     * where the caller has already located the leaf via findLeafPosition()
     * and is operating on its own forest (or a clone of it), so there is
     * no adversarial proof to verify — the forest is trusted.
     *
     * Rationale: the proof-based variant re-verifies the proof against the
     * cached `roots_` vector, which can legitimately be out of sync with
     * the node tree in the presence of partial deletions (see Apr 13 2026
     * soak-test: `ComputeUtreexoRootPure` was failing on every covenant
     * spend because the climb-from-siblings hash did not match the cached
     * root). Trusted internal callers can bypass the check; untrusted
     * callers (network peers) must keep using the proof-based variant.
     *
     * Still enforces:
     *  - position is within numLeaves_
     *  - position is not already deleted
     *  - the leaf at `position` matches `leafHash` exactly
     *
     * After the deletion this calls recomputePath() to refresh the
     * affected subtree root, identical to the proof-based variant.
     *
     * @param position  Position of the UTXO (obtained from findLeafPosition)
     * @param leafHash  Hash of the UTXO being removed (cross-checked)
     * @return true if removal succeeded
     */
    bool removeAtKnownPosition(uint64_t position, const UtreexoHash& leafHash);

    /**
     * @brief Remove several trusted leaves and rebuild roots once.
     *
     * This is the block-transition equivalent of removeAtKnownPosition().
     * Every entry is validated before any mutation, so failure leaves the
     * forest unchanged. The resulting state is identical to performing the
     * same removals sequentially, but avoids recomputing an entire perfect
     * tree after every input in a mining template.
     */
    bool removeAtKnownPositions(
        const std::vector<std::pair<uint64_t, UtreexoHash>>& removals);

    /**
     * @brief Generate proof for a UTXO
     *
     * Complexity: O(log n)
     *
     * @param position Position of UTXO in forest
     * @return Proof, or nullopt if position invalid
     */
    std::optional<UtreexoProof> prove(uint64_t position) const;

    /**
     * @brief Prove many positions at once, sharing one subtree-hash cache.
     *
     * Element i is exactly what prove(positions[i]) returns (byte-identical
     * siblings; std::nullopt for deleted/out-of-range positions), but sibling
     * subtree hashes are memoized across the whole batch, so total cost is
     * O(forest) hashing instead of O(positions × forest). Use this for any
     * per-block proving loop — the per-target prove() loop in the block
     * connect path is what made 1600-input mainnet block 92742 take minutes
     * to connect (2026-08-21 incident).
     */
    std::vector<std::optional<UtreexoProof>> proveMany(
        const std::vector<uint64_t>& positions) const;

    /**
     * @brief Verify a batched Utreexo proof (STATEFUL - legacy)
     *
     * This is the legacy STATEFUL verification function that requires
     * leaf_positions_ to be populated. Use verifyBatchProofStateless() for
     * true stateless verification.
     *
     * @deprecated Use verifyBatchProofStateless() for stateless nodes
     *
     * @param targets Leaf hashes being proven (spent UTXOs)
     * @param proof_hashes Shared authentication path nodes (batched proof data)
     * @return true if all targets are valid and proof is correct
     */
    bool verifyBatchProof(const std::vector<UtreexoHash>& targets,
                         const std::vector<UtreexoHash>& proof_hashes) const;

    /**
     * @brief Verify a batched Utreexo proof (STATELESS - recommended)
     *
     * This is the CONSENSUS-CRITICAL stateless verification function for Utreexo.
     * It verifies proofs WITHOUT requiring local UTXO database or leaf_positions_.
     *
     * Algorithm:
     * 1. Validate inputs (targets.size() == positions.size())
     * 2. For each (target, position) pair:
     *    - Compute expected tree height from position
     *    - Extract required siblings from proof_hashes
     *    - Compute Merkle path from leaf to root
     * 3. Verify all computed roots match forest roots
     *
     * Complexity: O(k * log n) where k = number of targets
     *
     * @param targets Leaf hashes being proven (spent UTXOs)
     * @param positions Leaf positions in forest (one per target)
     * @param proof_hashes Shared authentication path nodes (batched proof data)
     * @param proofNumLeaves Total leaves in forest at proof generation time
     * @param expectedRoots Forest roots to verify against (from block header commitment)
     * @return true if all targets are valid and proof is correct
     */
    bool verifyBatchProofStateless(
        const std::vector<UtreexoHash>& targets,
        const std::vector<uint64_t>& positions,
        const std::vector<UtreexoHash>& proof_hashes,
        uint64_t proofNumLeaves,
        const std::vector<UtreexoHash>& expectedRoots) const;

    /**
     * @brief Generate batched proof for multiple leaves (miner proof generation)
     *
     * This is the MINER-SIDE proof generation function.
     * Given a set of leaf hashes, generates a batched Merkle proof that proves
     * all leaves exist in the accumulator.
     *
     * Algorithm:
     * 1. Find position of each target leaf in forest
     * 2. Generate individual Merkle paths for each target
     * 3. Combine and deduplicate sibling hashes (batching efficiency)
     * 4. Return minimal set of proof_hashes
     *
     * Complexity: O(k * log n) where k = number of targets
     *
     * @param targets Leaf hashes to prove (spent UTXOs)
     * @return Vector of proof hashes (deduplicated Merkle siblings)
     * @deprecated Use generateBlockProof() for stateless-compatible proofs
     */
    std::vector<UtreexoHash> generateBatchProof(const std::vector<UtreexoHash>& targets) const;

    /**
     * @brief Generate complete block proof with positions (stateless-compatible)
     *
     * This is the recommended proof generation function for miners.
     * Returns a BlockUtreexoProof with all data needed for stateless verification:
     * - targets: Leaf hashes being proven
     * - positions: Leaf positions in forest (one per target)
     * - proof_hashes: Deduplicated Merkle siblings
     * - numLeaves: Total leaves in forest at proof time
     *
     * Algorithm:
     * 1. Find position of each target leaf
     * 2. Generate individual Merkle paths
     * 3. Deduplicate proof hashes
     * 4. Package with positions and numLeaves
     *
     * @param targets Leaf hashes to prove (spent UTXOs)
     * @return Complete BlockUtreexoProof ready for stateless verification
     */
    BlockUtreexoProof generateBlockProof(const std::vector<UtreexoHash>& targets) const;
    BlockUtreexoProof generateBlockProof(const std::vector<UtreexoHash>& targets,
                                         uint8_t format_version) const;

    // ───────────────────────────────────────────────────────────────────────
    // Queries
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Get current forest roots
     *
     * Number of roots = number of 1-bits in binary representation of numLeaves
     * Example: 5 leaves = 0b101 → 2 roots
     *
     * @return Vector of root hashes (ordered by tree size, largest first)
     */
    std::vector<UtreexoHash> getRoots() const {
        // Filter out empty roots (std::nullopt)
        std::vector<UtreexoHash> nonEmptyRoots;
        for (const auto& root : roots_) {
            if (root.has_value()) {
                nonEmptyRoots.push_back(root.value());
            }
        }
        return nonEmptyRoots;
    }

    /**
     * @brief Get height-indexed roots (preserves nullopt for empty tree slots)
     *
     * Unlike getRoots() which filters out empty slots, this preserves the
     * height-to-root mapping needed for stump reconstruction after deletions
     * (where some tree slots may become empty).
     */
    const std::vector<std::optional<UtreexoHash>>& getIndexedRoots() const {
        return roots_;
    }

    /**
     * @brief Get single commitment hash for entire accumulator
     *
     * Commitment = SHA256(root1 || root2 || ... || rootN)
     * This is the value stored in block headers.
     *
     * @return 32-byte commitment hash
     */
    UtreexoHash getCommitment() const;

    /**
     * @brief Get number of leaves in forest
     */
    uint64_t getNumLeaves() const {
        return numLeaves_;
    }

    /**
     * @brief Get number of active (non-deleted) leaves
     *
     * Returns the count of UTXOs currently in the accumulator (not spent).
     * numLeaves_ tracks total ever added, this returns total - deleted.
     */
    uint64_t getActiveLeaves() const {
        return numLeaves_ - deleted_positions_.size();
    }

    /**
     * @brief Check if a position has been deleted
     */
    bool isDeleted(uint64_t position) const {
        return deleted_positions_.count(position) > 0;
    }

    /**
     * @brief Get number of roots in forest
     *
     * This equals the number of 1-bits in numLeaves
     * Example: 5 leaves = 0b101 → 2 roots
     */
    size_t getNumRoots() const {
        size_t count = 0;
        for (const auto& root : roots_) {
            if (root.has_value()) {
                count++;
            }
        }
        return count;
    }

    /**
     * @brief Check if accumulator is empty
     */
    bool isEmpty() const {
        return numLeaves_ == 0;
    }

    /**
     * @brief Clone the accumulator for simulation
     *
     * Creates a deep copy that can be modified without affecting the original.
     * Used during mining to compute AFTER-state commitment.
     *
     * @return Copy of this accumulator
     */
    UtreexoForest clone() const {
        UtreexoForest copy;
        copy.roots_ = roots_;
        copy.numLeaves_ = numLeaves_;
        copy.nodes_ = nodes_;
        copy.leaf_positions_ = leaf_positions_;
        copy.deleted_positions_ = deleted_positions_;  // FIX: Must copy deleted set for correct root computation
        copy.canonical_empty_roots_ = canonical_empty_roots_;  // Stage 3: carry the fork flag
        return copy;
    }

    /**
     * @brief Clone the accumulator and promote it to the semantics that
     *        apply at the given block height.
     *
     * This is the SINGLE, AUTHORITATIVE factory every consensus call site
     * must use when computing a commitment or proof for a specific block
     * height. Starts from a plain `clone()`, then — if the canonical-roots
     * fork is active at `height` and the clone doesn't yet have the flag
     * on — flips the flag and calls `rebuildRoots()` to re-canonicalize
     * any ghost slots left behind by pre-fork operations.
     *
     * All of the old scattered `IsUtreexoCanonicalRootsActive(height)` +
     * `setCanonicalEmptyRoots(true)` + `rebuildRoots()` triples in
     * block_validation.cpp / block_assembler.cpp / bridge_node.cpp / RPC
     * mining get collapsed into this one call. Defined out-of-line in
     * utreexo_accumulator.cpp so the implementation can include the
     * activation header without pulling it into every translation unit
     * that includes this header.
     *
     * @param height Block height that the commitment / proof will be
     *               committed to.
     * @return A forest clone whose `canonical_empty_roots_` flag matches
     *         `IsUtreexoCanonicalRootsActive(height)` and whose `roots_`
     *         are canonical-consistent with that flag.
     */
    UtreexoForest cloneForHeight(uint32_t height) const;

    /**
     * @brief Find position of a leaf by its hash
     *
     * Searches the forest for a leaf with the given hash.
     * Used during proof verification when validator needs position.
     *
     * @param leafHash The leaf hash to search for
     * @return Position if found, nullopt otherwise
     */
    std::optional<uint64_t> findLeafPosition(const UtreexoHash& leafHash) const;

    // ───────────────────────────────────────────────────────────────────────
    // Phase 4: Delta-Based Undo Operations
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Restore a previously deleted leaf (for block disconnection)
     *
     * Phase 4: Delta-based undo - instead of replacing entire forest,
     * restore individual deleted leaves to their original positions.
     *
     * Algorithm:
     * 1. Verify position was deleted
     * 2. Restore leaf hash to nodes_[position]
     * 3. Remove position from deleted_positions_
     * 4. Restore leaf_positions_ mapping
     * 5. Recompute path to root
     *
     * @param position Position where leaf should be restored
     * @param leafHash Hash of the leaf to restore
     * @return true if restoration succeeded
     */
    bool restoreDeletedLeaf(uint64_t position, const UtreexoHash& leafHash);

    /**
     * @brief Remove the last N added leaves (for block disconnection)
     *
     * Phase 4: Delta-based undo - undo leaf additions by removing them.
     *
     * During ConnectBlock, new UTXOs are added via add() which appends
     * to position numLeaves++. To undo, we remove the last N leaves.
     *
     * Algorithm:
     * 1. Verify we have at least N leaves
     * 2. For each of the last N positions:
     *    - Remove from nodes_
     *    - Remove from leaf_positions_
     *    - Decrement numLeaves_
     * 3. Recompute roots based on new numLeaves
     *
     * CRITICAL: This assumes:
     * - None of the last N leaves were deleted during the block
     * - We're removing in reverse order of addition
     *
     * @param count Number of leaves to remove from the end
     * @return true if removal succeeded
     */
    bool removeLastNLeaves(uint64_t count);

    // ───────────────────────────────────────────────────────────────────────
    // Serialization
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Serialize accumulator state to bytes
     *
     * Format: numLeaves (8 bytes) || root1 (32 bytes) || root2 || ...
     */
    std::vector<uint8_t> serialize() const;

    /**
     * @brief Deserialize accumulator state from bytes
     */
    static UtreexoForest deserialize(const std::vector<uint8_t>& data);

    // ───────────────────────────────────────────────────────────────────────
    // Statistics
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Get accumulator statistics
     */
    struct Stats {
        uint64_t numLeaves;      // Total UTXO count
        size_t numRoots;         // Number of forest roots
        size_t totalSize;        // Total memory usage (bytes)
        size_t avgProofSize;     // Average proof size (bytes)
    };

    Stats getStats() const;

public:
    /**
     * @brief Activate the canonical empty-roots fork (Apr 13 2026 Stage 3).
     *
     * When enabled:
     *  - `computeSubtreeHash()` returns a canonical zero-sentinel hash for
     *    fully-deleted subtrees instead of `std::nullopt`.
     *  - `recomputePath()` therefore preserves the invariant
     *    `roots_[h].has_value() ⟺ bit h of numLeaves_` even after the last
     *    leaf in a subtree is removed.
     *  - The next `add()` sees a populated slot at `h` and correctly takes
     *    the merge branch instead of placing a fresh leaf there.
     *
     * The caller (block validation / chainstate startup) is responsible for
     * flipping this on at the activation block and calling `rebuildRoots()`
     * once to re-canonicalize existing state. See
     * `consensus/utreexo_canonical_roots_activation.h` for the activation
     * heights.
     *
     * This is a CONSENSUS CHANGE: `getCommitment()` outputs differ before
     * and after activation, so every node on the network must flip the flag
     * at the same height.
     */
    void setCanonicalEmptyRoots(bool v) { canonical_empty_roots_ = v; }
    bool isCanonicalEmptyRoots() const { return canonical_empty_roots_; }

    /**
     * @brief Diagnostic — describe what state a leaf hash collides with.
     *
     * Used by add()-failure sites (ComputeUtreexoRootPure /
     * ConnectBlockInternal / reindexer) to surface WHY add() returned
     * UINT64_MAX. The default error string ("duplicate leaf/capacity")
     * doesn't say which leaf, where the existing entry lives, or whether
     * the rejection was capacity vs duplicate. This formats a one-line
     * string with: numLeaves, capacity_room, leaf_hash_hex, and (if a
     * duplicate exists) existing_position + its node-state + its
     * deleted-state. Intended for std::cerr / log lines, not for
     * machine parsing.
     */
    std::string describeAddFailure(const UtreexoHash& leafHash) const;

    /**
     * @brief Diagnostic — full internal-state dump in deterministic text format.
     *
     * Used by the height-9290 paired-snapshot comparison plan. Emits every
     * field that influences add()/remove() determinism — getCommitment()
     * by itself only hashes (numLeaves || roots[0..63]) which renders
     * std::nullopt and std::optional(ZERO_HASH) identically (32 zero bytes
     * either way), so two forests can match commitments while differing
     * internally. This dump exposes the difference.
     *
     * Format: stable, line-oriented, sortable. Not a serialization
     * format — strictly for human + diff-tool consumption.
     */
    std::string dumpInternalState() const;

    /**
     * @brief Recompute roots_ from scratch based on current nodes_ and numLeaves_.
     *
     * Exposed publicly because the canonical-roots fork activation needs to
     * call it once at the activation block, after flipping
     * `canonical_empty_roots_` on, to re-canonicalize a forest that was
     * built by the pre-fork code. Same implementation used internally after
     * `removeLastNLeaves()`.
     */
    void rebuildRoots();

private:
    // Current forest roots (ordered by tree height, ascending)
    // ARCHITECTURE NOTE: We use std::optional<UtreexoHash> to explicitly
    // represent empty slots. This prevents any confusion between
    // "no root at this height" (std::nullopt) and "root whose hash is all zeros"
    // (std::optional containing a 32-byte vector of zeros).
    std::vector<std::optional<UtreexoHash>> roots_;

    // Total number of leaves ever added to forest (including deleted)
    uint64_t numLeaves_;

    // Apr 13 2026 Stage 3 — canonical-empty-roots fork flag.
    // When true, `computeSubtreeHash()` returns a deterministic zero-sentinel
    // for fully-deleted subtrees instead of `std::nullopt`. This preserves
    // the `roots_[h].has_value() ⟺ bit h of numLeaves_` invariant and fixes
    // the proof.verify cascade that made covenant spends unmineable.
    // Flipped on at `UTREEXO_CANONICAL_ROOTS_HEIGHT_MAINNET` (2870).
    // Copied by `clone()` so cloned snapshots inherit the flag.
    bool canonical_empty_roots_ = false;

    // Internal forest representation (for proof generation)
    // Map: position -> hash
    // std::nullopt = deleted/empty node
    std::vector<std::optional<UtreexoHash>> nodes_;

    // Leaf lookup map (for proof generation by hash)
    // Map: leaf_hash -> position (single position; live leaf hashes must be unique)
    // Required for generateBatchProof() to find leaf positions
    std::unordered_map<UtreexoHash, uint64_t, UtreexoHashHasher> leaf_positions_;

    // Track deleted leaf positions (for spent UTXOs)
    std::unordered_set<uint64_t> deleted_positions_;

    // Internal consistency audit for side-indexed forest state.
    bool validateLeafIndexConsistency() const;

    /**
     * @brief Get height of tree containing position
     *
     * This finds which tree in the forest contains the given leaf position.
     */
    uint8_t getTreeHeight(uint64_t position) const;

    /**
     * @brief Compute parent hash from two children
     */
    UtreexoHash parentHash(const UtreexoHash& left, const UtreexoHash& right) const;

    /**
     * @brief Merge two roots of same height
     */
    void mergeRoots();

    /**
     * @brief Compute hash of a subtree covering leaves [start, start + size)
     * @return Hash of subtree, or std::nullopt if subtree is entirely empty/deleted
     */
    std::optional<UtreexoHash> computeSubtreeHash(uint64_t start, uint64_t size) const;

    using SubtreeHashCache =
        std::vector<std::unordered_map<uint64_t, std::optional<UtreexoHash>>>;
    std::optional<UtreexoHash> computeSubtreeHashCached(
        uint64_t start, uint64_t size, SubtreeHashCache& cache) const;
    std::optional<UtreexoProof> proveWithCache(
        uint64_t position, SubtreeHashCache* cache) const;

    /**
     * @brief Recompute parent hashes along path from position to root
     *
     * Called after removing a leaf to update affected parent hashes.
     * @param position The leaf position that was modified
     */
    void recomputePath(uint64_t position);

    // rebuildRoots() is declared in the public section above so the
    // canonical-roots fork activation can call it.
};

// ═══════════════════════════════════════════════════════════════════════════
// Block-Level Proof Structures (Phase 1: Proof Data Structures)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Spent output metadata (for stateless Utreexo validation)
 *
 * In Utreexo, nodes verify proofs WITHOUT looking up UTXOs from local database.
 * The block must provide enough data to compute leaf hashes for spent outputs.
 *
 * Minimal transparent set required:
 * - value: uint64_t (amount in una)
 * - scriptPubKey: bytes (locking script)
 *
 * Confidential extension (format v5):
 * - is_confidential: bool
 * - commitment: bytes (Pedersen commitment for CT prevouts)
 *
 * Combined with outpoint (txid + vout) from transaction input, this allows
 * computing: leafHash = SHA256(txid || vout || value || scriptPubKey)
 *
 * Design notes:
 * - This is overhead (~30-100 bytes per spent output)
 * - But it enables stateless validation (no UTXO set required)
 * - Trade-off: block size vs validation requirements
 */
struct SpentOutputData {
    uint64_t value;                  // Amount in una
    std::vector<uint8_t> scriptPubKey;  // Locking script
    uint32_t created_height = 0;
    bool is_coinbase = false;
    bool is_confidential = false;
    std::vector<uint8_t> commitment;

    SpentOutputData() : value(0) {}
    SpentOutputData(uint64_t v, const std::vector<uint8_t>& spk)
        : value(v), scriptPubKey(spk) {}
    SpentOutputData(uint64_t v, const std::vector<uint8_t>& spk, bool confidential,
                    const std::vector<uint8_t>& commit)
        : value(v), scriptPubKey(spk), is_confidential(confidential), commitment(commit) {}
    SpentOutputData(uint64_t v, const std::vector<uint8_t>& spk, uint32_t h,
                    bool coinbase)
        : value(v), scriptPubKey(spk), created_height(h), is_coinbase(coinbase) {}
    SpentOutputData(uint64_t v, const std::vector<uint8_t>& spk, uint32_t h,
                    bool coinbase, bool confidential, const std::vector<uint8_t>& commit)
        : value(v), scriptPubKey(spk), created_height(h), is_coinbase(coinbase),
          is_confidential(confidential), commitment(commit) {}

    /**
     * @brief Serialize to bytes
     */
    std::vector<uint8_t> serialize(uint8_t format_version = 5) const;

    /**
     * @brief Deserialize from bytes
     */
    static SpentOutputData deserialize(const std::vector<uint8_t>& data, size_t& offset,
                                       uint8_t format_version = 5);

    /**
     * @brief Get size in bytes
     */
    size_t size(uint8_t format_version = 5) const {
        size_t total = 8 + 4 + scriptPubKey.size();
        if (format_version >= 6) {
            total += 4 + 1;
        }
        if (format_version >= 5) {
            total += 1 + 4 + commitment.size();
        }
        return total;
    }
};

/**
 * @brief Block-level aggregated Utreexo proof
 *
 * Unlike single-UTXO proofs (UtreexoProof above), this structure contains
 * a batched proof for ALL spent inputs in a block.
 *
 * Design: Per-block aggregated proofs (Bitcoin Core research direction)
 * - Proofs are part of the block, not individual transactions
 * - Single proof covers all spends in the block
 * - Proves: "These spent UTXOs existed in the accumulator before this block"
 *
 * Phase 1: Define data structure only (no enforcement yet)
 */
struct BlockUtreexoProof {
    std::vector<UtreexoHash> targets;       // Leaf hashes being proven (one per spent input)
    std::vector<uint64_t> positions;        // Leaf positions in forest (one per target, same order)
    std::vector<UtreexoHash> proof_hashes;  // Authentication path nodes (Merkle proof data)
    uint64_t numLeaves;                     // Total leaves in forest at proof generation time
    uint8_t format_version;                 // Serialized proof/spent-output metadata version

    BlockUtreexoProof() : numLeaves(0), format_version(5) {}

    /**
     * @brief Serialize proof to bytes
     *
     * Format (v5 - stateless with CT-aware spent-output metadata):
     * - version (1 byte = 5)
     * - numLeaves (8 bytes)
     * - target_count (4 bytes)
     * - targets (32 bytes each)
     * - positions (8 bytes each, same count as targets)
     * - proof_hash_count (4 bytes)
     * - proof_hashes (32 bytes each)
     */
    std::vector<uint8_t> serialize() const;

    /**
     * @brief Deserialize proof from bytes
     */
    static BlockUtreexoProof deserialize(const std::vector<uint8_t>& data);

    /**
     * @brief Check if proof is empty
     */
    bool isEmpty() const {
        return targets.empty() && positions.empty() && proof_hashes.empty();
    }

    /**
     * @brief Get proof size in bytes
     */
    size_t size() const {
        // version(1) + numLeaves(8) + counts(8) + targets + positions + proof_hashes
        return 17 + (targets.size() * 32) + (positions.size() * 8) + (proof_hashes.size() * 32);
    }

    /**
     * @brief Validate proof structure
     * @return true if proof is structurally valid (positions match targets)
     */
    bool isValid() const {
        // positions must match targets (one position per target)
        return positions.size() == targets.size();
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 9.1: Compression (Hash Deduplication)
    // ═════════════════════════════════════════════════════════════════════════

    /**
     * @brief Compress proof using hash deduplication
     * @return Compressed representation with dictionary encoding
     *
     * **Phase 9.1 Hash Deduplication:**
     * Creates a dictionary of unique hashes and replaces duplicate hashes with indices.
     * Typical savings: 30-40% for blocks with >10 inputs.
     *
     * Format:
     * [version: 1 byte = 2]
     * [dict_size: varint]
     * [hash_0: 32 bytes]
     * [hash_1: 32 bytes]
     * ...
     * [num_targets: varint]
     * [target_idx_0: varint]
     * ...
     * [num_proofs: varint]
     * [proof_idx_0: varint]
     * ...
     */
    std::vector<uint8_t> serializeCompressed() const;

    /**
     * @brief Decompress proof from deduplicated format
     * @param data Compressed proof data (version 2)
     * @return Decompressed proof
     *
     * **Security:** Validates all indices < dictionary size
     */
    static BlockUtreexoProof deserializeCompressed(const std::vector<uint8_t>& data);

    /**
     * @brief Estimate compressed size
     * @return Estimated size in bytes after compression
     */
    size_t estimateCompressedSize() const;

    // ═════════════════════════════════════════════════════════════════════════
    // Phase 9.2: zstd Compression Framing
    // ═════════════════════════════════════════════════════════════════════════

    /**
     * @brief Compress proof with zstd framing
     * @return Compressed proof with zstd (version 3) or deduplicated (version 2)
     *
     * **Phase 9.2 zstd Compression:**
     * Applies zstd compression on top of hash deduplication for additional savings.
     *
     * Algorithm:
     * 1. Generate version 2 (deduplicated) proof
     * 2. If size <= 256 bytes, return v2 (threshold not met)
     * 3. Compress v2 proof with zstd level 3
     * 4. Return version 3 format
     *
     * Format (version 3):
     * [version: 1 byte = 3]
     * [uncompressed_size: 4 bytes]
     * [compressed_size: 4 bytes]
     * [zstd_compressed_data: variable]
     *
     * Expected savings: 10-20% additional on top of deduplication
     * Threshold: Only compresses if proof > 256 bytes
     * Fallback: Returns v2 on compression failure
     */
    std::vector<uint8_t> serializeCompressedWithZstd() const;

    /**
     * @brief Decompress proof from zstd-framed format
     * @param data Compressed proof data (version 2 or 3)
     * @return Decompressed proof
     *
     * **Security:**
     * - Validates uncompressed_size <= 100 KB (decompression bomb protection)
     * - Rejects compression ratios > 100:1 (suspicious)
     * - Verifies size matches after decompression
     * - Handles both v2 (deduplicated) and v3 (zstd) formats
     */
    static BlockUtreexoProof deserializeCompressedWithZstd(const std::vector<uint8_t>& data);
};

/**
 * @brief Block-level Utreexo data container
 *
 * This structure wraps all Utreexo-related data for a block:
 * - Batched proof for all spends
 * - Accumulator root BEFORE applying this block
 * - Spent output metadata (for stateless validation)
 *
 * The "before" root is critical for:
 * - Verifying proofs against correct state
 * - Detecting stale proofs
 * - Rollback during reorgs
 *
 * Spent outputs metadata enables stateless validation:
 * - One SpentOutputData per spent input (in order)
 * - Allows computing leaf hashes without UTXO database lookup
 * - Essential for Utreexo's stateless property
 *
 * Phase 1: Blocks can carry this data (may be empty for now)
 * Phase 2: Shadow verification (verify but don't enforce)
 * Phase 3: Enforce at activation height
 */
struct BlockUtreexoData {
    BlockUtreexoProof spend_proof;            // Proof for all spent inputs in block
    UtreexoHash accumulator_root_before;          // Accumulator root before applying block
    std::vector<SpentOutputData> spent_outputs;  // Metadata for each spent input (ordered)

    BlockUtreexoData() = default;

    /**
     * @brief Serialize to bytes
     *
     * Format:
     * - root_before (32 bytes)
     * - spend_proof (variable)
     */
    std::vector<uint8_t> serialize() const;

    /**
     * @brief Deserialize from bytes
     */
    static BlockUtreexoData deserialize(const std::vector<uint8_t>& data);

    /**
     * @brief Check if data is empty
     */
    bool isEmpty() const {
        return accumulator_root_before.empty() && spend_proof.isEmpty() && spent_outputs.empty();
    }

    /**
     * @brief Get total size in bytes
     */
    size_t size() const {
        size_t spent_outputs_size = 0;
        for (const auto& spent : spent_outputs) {
            spent_outputs_size += spent.size(spend_proof.format_version);
        }
        return 32 + spend_proof.size() + spent_outputs_size;  // root + proof + spent_outputs
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Utreexo Transition Proof
// ═══════════════════════════════════════════════════════════════════════════
//
// Proves a complete accumulator state transition for a block:
//   roots_before → (deletions, additions) → commitment_after
//
// The proof carries intermediate roots (roots_after_deletions) so that
// verification requires ONLY stump operations (no full forest needed).
//
// Security model:
//   1. Batch proof verifies deletion targets exist in roots_before
//   2. Stump adds additions to roots_after_deletions
//   3. Result must match commitment_after (SHA-256 binding)
//   An attacker providing fake roots_after_deletions would need a
//   SHA-256 collision — infeasible.
//
// ═══════════════════════════════════════════════════════════════════════════

struct UtreexoTransitionProof {
    // Pre-state: forest roots indexed by tree height
    // (nullopt = no tree at that height; determined by numLeaves bit pattern)
    std::vector<std::optional<UtreexoHash>> roots_before;
    uint64_t num_leaves_before = 0;

    // Deletion proof (mirrors BlockUtreexoProof fields)
    std::vector<UtreexoHash> deletion_targets;
    std::vector<uint64_t> deletion_positions;
    std::vector<UtreexoHash> deletion_proof_hashes;

    // Intermediate state: roots after PASS 1 (deletions), before PASS 2 (additions)
    // Height-indexed: some slots may become nullopt if all leaves in that tree deleted.
    // Computed by full node (has the forest), verified transitively by stump.
    std::vector<std::optional<UtreexoHash>> roots_after_deletions;

    // Additions: leaf hashes of new UTXOs (canonical order from block.vtx)
    std::vector<UtreexoHash> addition_hashes;

    // Post-state
    UtreexoHash commitment_after;
    uint64_t num_leaves_after = 0;  // = num_leaves_before + addition_hashes.size()

    // ═══════════════════════════════════════════════════════════════════════
    // Verification (pure — stump-based, no external state needed)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Self-contained verification:
     * 1. Reconstruct stump from roots_before, verify deletion proof
     * 2. Reconstruct stump from roots_after_deletions, apply additions
     * 3. Check resulting commitment == commitment_after
     */
    bool verify() const;

    // ═══════════════════════════════════════════════════════════════════════
    // Generation (requires full forest — full node / miner)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Generate transition proof from forest + block.
     * Clones the forest, applies REMOVE ALL then ADD ALL, captures
     * intermediate roots between the two passes.
     */
    static UtreexoTransitionProof generate(
        const UtreexoForest& forest_before,
        const Block& block,
        const BlockUtreexoProof& spend_proof,
        uint32_t block_height = 0);

    /**
     * Ownership-taking overload: the caller already holds a private forest it
     * does not need afterwards, so generate() adopts it instead of cloning.
     *
     * ConnectBlock previously copied the live forest out from under
     * LockForestShared() purely to escape the lock, then handed that copy to
     * the const& overload, which cloned it AGAIN — two full deep copies of a
     * ~300k-leaf structure per block. This overload removes the second one.
     */
    static UtreexoTransitionProof generate(
        UtreexoForest&& forest_snapshot,
        const Block& block,
        const BlockUtreexoProof& spend_proof,
        uint32_t block_height = 0);

    /**
     * Compute canonical addition hashes from block transactions.
     * MUST match forest-clone PASS 2 in ConnectBlockInternal.
     * Iterates all txs, all outputs, computes the activation-aware leaf for each.
     * No OP_RETURN skip (matches forest behavior).
     */
    static std::vector<UtreexoHash> computeAdditionHashes(const Block& block, uint32_t block_height = 0);

    // ═══════════════════════════════════════════════════════════════════════
    // Serialization
    // ═══════════════════════════════════════════════════════════════════════

    std::vector<uint8_t> serialize() const;
    static UtreexoTransitionProof deserialize(const std::vector<uint8_t>& data);
    size_t serializedSize() const;

    bool isEmpty() const;
};

// ═══════════════════════════════════════════════════════════════════════════
// Batch Operations (for block processing)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Batch accumulator update
 *
 * For efficient block processing:
 * - Add multiple UTXOs at once
 * - Remove multiple UTXOs at once
 * - Single commitment update
 */
struct UtreexoBatchUpdate {
    std::vector<UtreexoHash> adds;     // UTXOs to add
    std::vector<std::pair<UtreexoHash, UtreexoProof>> removes;  // UTXOs to remove (with proofs)

    /**
     * @brief Apply batch update to accumulator
     *
     * @param forest Accumulator to update
     * @return true if all operations succeeded
     */
    bool apply(UtreexoForest& forest) const;
};

} // namespace consensus
} // namespace dinero
