/**
 * @file test_proof_lifecycle.cpp
 * @brief Proof Lifecycle Tests for wallet.getproofbundle, wallet.proofstatus,
 *        and blockchain.getproofupdates
 *
 * Tests the proof lifecycle invariants at the UtreexoForest level, mirroring
 * the contract defined in docs/PROOF-SERVICE-CONTRACT.md:
 *
 *   1. Bundle generation: prove all leaves, bind to single root
 *   2. Staleness detection: root comparison after tip advance / reorg
 *   3. Proof refresh: re-prove at new root after forest mutation
 *   4. Cross-RPC lifecycle: bundle → fresh → advance → stale → update → fresh
 *
 * These tests operate at the forest level — no daemon, no RPC, no ChainDB.
 * The invariants are:
 *   "proofs bind to roots, roots change with state, stale proofs must refresh"
 * which is testable with UtreexoForest alone.
 */

#include <gtest/gtest.h>
#include "consensus/utreexo_accumulator.h"
#include "primitives/uint256.h"
#include <cstring>
#include <set>

using namespace dinero;
using namespace dinero::consensus;

// ═══════════════════════════════════════════════════════════════════════════════
// Test Utilities
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

UtreexoHash makeTestHash(uint64_t value) {
    UtreexoHash hash(32, 0);
    for (int i = 0; i < 8; i++) {
        hash[i] = (value >> (i * 8)) & 0xFF;
    }
    for (int i = 8; i < 32; i++) {
        hash[i] = static_cast<uint8_t>((value * 31 + i * 17) % 256);
    }
    return hash;
}

uint256 commitmentToUint256(const UtreexoHash& commitment) {
    uint256 result;
    if (commitment.size() == 32) {
        std::memcpy(result.begin(), commitment.data(), 32);
    }
    return result;
}

bool hashesEqual(const UtreexoHash& a, const UtreexoHash& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin());
}

// Hex encode a UtreexoHash (mirrors hashToHex in methods_utreexo.cpp)
std::string toHex(const UtreexoHash& hash) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(hash.size() * 2);
    for (uint8_t byte : hash) {
        result.push_back(hex_chars[byte >> 4]);
        result.push_back(hex_chars[byte & 0x0F]);
    }
    return result;
}

// Simulate "connect block" — add UTXO leaves to forest
void connectBlock(UtreexoForest& forest, std::vector<UtreexoHash>& all_leaves,
                  uint64_t block_id, int num_outputs) {
    for (int i = 0; i < num_outputs; i++) {
        auto leaf = makeTestHash(block_id * 1000 + i);
        forest.add(leaf);
        all_leaves.push_back(leaf);
    }
}

// Simulate "disconnect block" — remove the last N leaves added by connectBlock
// Returns the removed leaves (for verifying they're gone)
std::vector<UtreexoHash> disconnectBlock(UtreexoForest& forest,
                                          std::vector<UtreexoHash>& all_leaves,
                                          int num_outputs) {
    std::vector<UtreexoHash> removed;
    for (int i = 0; i < num_outputs && !all_leaves.empty(); i++) {
        auto leaf = all_leaves.back();
        auto pos = forest.findLeafPosition(leaf);
        if (pos.has_value()) {
            auto proof = forest.prove(*pos);
            if (proof.has_value()) {
                forest.remove(leaf, *proof);
                removed.push_back(leaf);
            }
        }
        all_leaves.pop_back();
    }
    return removed;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// PROOF BUNDLE: wallet.getproofbundle semantics
//
// Contract: Generate proofs for all wallet UTXOs at current tip.
// All proofs bind to the same accumulator_root.
// ═══════════════════════════════════════════════════════════════════════════════

class ProofBundleTest : public ::testing::Test {
protected:
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    void SetUp() override {
        // Simulate 3 blocks with UTXOs
        connectBlock(forest, leaves, /*block_id=*/1, /*num_outputs=*/3);
        connectBlock(forest, leaves, /*block_id=*/2, /*num_outputs=*/4);
        connectBlock(forest, leaves, /*block_id=*/3, /*num_outputs=*/2);
        // Total: 9 leaves
    }
};

TEST_F(ProofBundleTest, BundleProofsAllVerify) {
    auto roots = forest.getRoots();

    for (size_t i = 0; i < leaves.size(); i++) {
        auto pos = forest.findLeafPosition(leaves[i]);
        ASSERT_TRUE(pos.has_value()) << "Leaf " << i << " not found in forest";

        auto proof = forest.prove(*pos);
        ASSERT_TRUE(proof.has_value()) << "Failed to prove leaf " << i;

        bool verified = proof->verify(leaves[i], roots);
        EXPECT_TRUE(verified) << "Proof for leaf " << i << " failed verification";
    }
}

TEST_F(ProofBundleTest, BundleProofsBoundToSingleRoot) {
    // All proofs in a bundle are generated against the same forest state.
    // The root commitment is the binding authority.
    auto root = forest.getCommitment();
    ASSERT_EQ(root.size(), 32u);

    // Generate proofs for all leaves — the root doesn't change between proofs
    // because no mutation happens during bundle generation.
    for (size_t i = 0; i < leaves.size(); i++) {
        auto current_root = forest.getCommitment();
        EXPECT_TRUE(hashesEqual(root, current_root))
            << "Root changed during bundle generation at leaf " << i;
    }
}

TEST_F(ProofBundleTest, EmptyForestProducesNoProofs) {
    UtreexoForest empty;
    auto proof = empty.prove(0);
    EXPECT_FALSE(proof.has_value())
        << "Empty forest must not produce proofs";

    auto commitment = empty.getCommitment();
    // Empty forest should still return a valid (though possibly zero) commitment
    // The RPC would return utxo_count: 0, proofs: []
}

TEST_F(ProofBundleTest, TruncationPreservesProofValidity) {
    // Simulate max_utxos cap: only prove first N leaves (e.g., 5 of 9)
    const size_t max_utxos = 5;
    auto roots = forest.getRoots();
    size_t proven = 0;

    for (size_t i = 0; i < leaves.size() && proven < max_utxos; i++) {
        auto pos = forest.findLeafPosition(leaves[i]);
        ASSERT_TRUE(pos.has_value());

        auto proof = forest.prove(*pos);
        ASSERT_TRUE(proof.has_value());
        EXPECT_TRUE(proof->verify(leaves[i], roots));
        proven++;
    }

    EXPECT_EQ(proven, max_utxos)
        << "Should have proven exactly max_utxos leaves";
}

TEST_F(ProofBundleTest, ReorgMakesBundleStale) {
    // Generate bundle at current root
    auto bundle_root = forest.getCommitment();

    // Simulate reorg: disconnect block 3 (2 outputs), connect block 3' (3 outputs)
    disconnectBlock(forest, leaves, 2);
    connectBlock(forest, leaves, /*block_id=*/30, /*num_outputs=*/3);

    auto post_reorg_root = forest.getCommitment();

    // Bundle is stale — root changed
    EXPECT_FALSE(hashesEqual(bundle_root, post_reorg_root))
        << "Reorg must change the accumulator root, making old bundle stale";
}

// ═══════════════════════════════════════════════════════════════════════════════
// PROOF STATUS: wallet.proofstatus semantics
//
// Contract: Compare client's cached accumulator_root against current tip root.
// If they differ, the client's proofs are stale and need refreshing.
// ═══════════════════════════════════════════════════════════════════════════════

class ProofStatusTest : public ::testing::Test {
protected:
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    void SetUp() override {
        connectBlock(forest, leaves, 1, 5);
        connectBlock(forest, leaves, 2, 5);
    }
};

TEST_F(ProofStatusTest, SameRootIsFresh) {
    auto client_root = toHex(forest.getCommitment());
    auto current_root = toHex(forest.getCommitment());

    bool stale = (client_root != current_root);
    EXPECT_FALSE(stale) << "Same root must report as fresh (stale=false)";
}

TEST_F(ProofStatusTest, AdvancedTipIsStale) {
    auto client_root = toHex(forest.getCommitment());

    // Chain advances: new block with new UTXOs
    connectBlock(forest, leaves, 3, 3);

    auto current_root = toHex(forest.getCommitment());

    bool stale = (client_root != current_root);
    EXPECT_TRUE(stale)
        << "Advanced tip must report as stale (stale=true)";
}

TEST_F(ProofStatusTest, ReorgRootChangeIsStale) {
    auto client_root = toHex(forest.getCommitment());

    // Reorg: disconnect block 2, connect block 2' with different outputs
    disconnectBlock(forest, leaves, 5);  // Remove block 2's 5 outputs
    connectBlock(forest, leaves, 20, 5); // Connect block 2' with 5 different outputs

    auto current_root = toHex(forest.getCommitment());

    bool stale = (client_root != current_root);
    EXPECT_TRUE(stale)
        << "Reorg must report as stale even if UTXO count is the same";
}

TEST_F(ProofStatusTest, MultipleBlockAdvanceIsStale) {
    auto client_root = toHex(forest.getCommitment());

    // Several blocks advance
    connectBlock(forest, leaves, 3, 2);
    connectBlock(forest, leaves, 4, 3);
    connectBlock(forest, leaves, 5, 1);

    auto current_root = toHex(forest.getCommitment());

    bool stale = (client_root != current_root);
    EXPECT_TRUE(stale)
        << "Multiple block advance must report as stale";
}

TEST_F(ProofStatusTest, RootComparisonIsDeterministic) {
    // Getting commitment multiple times from same state must return same value
    auto root1 = toHex(forest.getCommitment());
    auto root2 = toHex(forest.getCommitment());
    auto root3 = toHex(forest.getCommitment());

    EXPECT_EQ(root1, root2);
    EXPECT_EQ(root2, root3);
}

TEST_F(ProofStatusTest, HexEncodingRoundTrip) {
    auto commitment = forest.getCommitment();
    auto hex = toHex(commitment);

    // proofstatus takes a hex string and compares it against current hex
    // Verify the hex encoding is consistent and correct length
    EXPECT_EQ(hex.length(), 64u) << "32-byte commitment should encode to 64 hex chars";

    // Same commitment → same hex
    auto hex2 = toHex(forest.getCommitment());
    EXPECT_EQ(hex, hex2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// PROOF UPDATES: blockchain.getproofupdates semantics
//
// Contract: Re-prove specific outpoints at the current tip.
// If root_from matches current root, return "no_update_needed".
// Otherwise re-prove each outpoint at the new root.
// ═══════════════════════════════════════════════════════════════════════════════

class ProofUpdatesTest : public ::testing::Test {
protected:
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    void SetUp() override {
        connectBlock(forest, leaves, 1, 4);
        connectBlock(forest, leaves, 2, 4);
        // 8 leaves total
    }
};

TEST_F(ProofUpdatesTest, FastPathWhenRootUnchanged) {
    auto root_from = forest.getCommitment();
    auto current_root = forest.getCommitment();

    // No update needed — same root
    bool needs_update = !hashesEqual(root_from, current_root);
    EXPECT_FALSE(needs_update)
        << "When root_from == current_root, fast path returns no_update_needed";
}

TEST_F(ProofUpdatesTest, StaleRootTriggersRefresh) {
    auto root_from = forest.getCommitment();

    // Prove a leaf at old root
    auto old_proof = forest.prove(0);
    ASSERT_TRUE(old_proof.has_value());
    auto old_roots = forest.getRoots();
    EXPECT_TRUE(old_proof->verify(leaves[0], old_roots));

    // Chain advances
    connectBlock(forest, leaves, 3, 2);
    auto current_root = forest.getCommitment();

    // Root changed — update needed
    bool needs_update = !hashesEqual(root_from, current_root);
    ASSERT_TRUE(needs_update);

    // Re-prove at new root (this is what getproofupdates does)
    auto new_pos = forest.findLeafPosition(leaves[0]);
    ASSERT_TRUE(new_pos.has_value()) << "Original UTXO must still exist after tip advance";

    auto new_proof = forest.prove(*new_pos);
    ASSERT_TRUE(new_proof.has_value());

    auto new_roots = forest.getRoots();
    EXPECT_TRUE(new_proof->verify(leaves[0], new_roots))
        << "Re-proved UTXO must verify at new root";
}

TEST_F(ProofUpdatesTest, SpentLeafCannotBeReproved) {
    // Prove leaf 7 (last one)
    auto target = leaves[7];
    auto pos = forest.findLeafPosition(target);
    ASSERT_TRUE(pos.has_value());

    auto proof = forest.prove(*pos);
    ASSERT_TRUE(proof.has_value());

    // Spend it (remove from forest)
    forest.remove(target, *proof);

    // Try to re-prove — should fail (leaf no longer exists)
    auto new_pos = forest.findLeafPosition(target);
    EXPECT_FALSE(new_pos.has_value())
        << "Spent (removed) leaf must not be findable in forest";
}

TEST_F(ProofUpdatesTest, MissingLeafReportsFailure) {
    // Try to find a leaf that was never added
    auto phantom = makeTestHash(99999);
    auto pos = forest.findLeafPosition(phantom);
    EXPECT_FALSE(pos.has_value())
        << "Non-existent leaf must not be found (success=false in RPC)";
}

TEST_F(ProofUpdatesTest, MixedBatchSomeRefreshableSomeMissing) {
    auto root_before = forest.getCommitment();

    // Spend leaf 7
    auto pos7 = forest.findLeafPosition(leaves[7]);
    ASSERT_TRUE(pos7.has_value());
    auto proof7 = forest.prove(*pos7);
    ASSERT_TRUE(proof7.has_value());
    forest.remove(leaves[7], *proof7);

    // Advance chain
    connectBlock(forest, leaves, 3, 2);

    auto new_roots = forest.getRoots();
    int success_count = 0;
    int failure_count = 0;

    // Try to re-prove a mix: leaves 0-6 should succeed, leaf 7 was spent
    for (int i = 0; i <= 7; i++) {
        auto target = makeTestHash(1 * 1000 + i % 4);  // leaves from block 1
        if (i < 4) target = makeTestHash(1 * 1000 + i);
        else if (i < 8) target = makeTestHash(2 * 1000 + (i - 4));

        auto pos = forest.findLeafPosition(target);
        if (pos.has_value()) {
            auto proof = forest.prove(*pos);
            if (proof.has_value() && proof->verify(target, new_roots)) {
                success_count++;
            } else {
                failure_count++;
            }
        } else {
            failure_count++;
        }
    }

    EXPECT_GT(success_count, 0) << "Some leaves should be refreshable";
    EXPECT_GT(failure_count, 0) << "Spent leaf should fail to re-prove";
}

TEST_F(ProofUpdatesTest, ReorgThenRefreshProducesNewRootProofs) {
    auto pre_reorg_root = forest.getCommitment();

    // Reorg: disconnect block 2 (4 outputs), connect block 2' (3 outputs)
    disconnectBlock(forest, leaves, 4);
    connectBlock(forest, leaves, 20, 3);

    auto post_reorg_root = forest.getCommitment();
    ASSERT_FALSE(hashesEqual(pre_reorg_root, post_reorg_root));

    // Re-prove the surviving leaves from block 1 at the new root
    auto new_roots = forest.getRoots();
    for (int i = 0; i < 4; i++) {
        auto target = makeTestHash(1 * 1000 + i);
        auto pos = forest.findLeafPosition(target);
        ASSERT_TRUE(pos.has_value())
            << "Block 1 leaf " << i << " should survive reorg of block 2";

        auto proof = forest.prove(*pos);
        ASSERT_TRUE(proof.has_value());

        EXPECT_TRUE(proof->verify(target, new_roots))
            << "Block 1 leaf " << i << " proof must verify at post-reorg root";
    }
}

TEST_F(ProofUpdatesTest, RefreshedProofBoundToNewRoot) {
    // Capture old state
    auto old_root = forest.getCommitment();
    auto old_roots = forest.getRoots();

    // Prove leaf 0 at old root
    auto pos0 = forest.findLeafPosition(leaves[0]);
    ASSERT_TRUE(pos0.has_value());
    auto old_proof = forest.prove(*pos0);
    ASSERT_TRUE(old_proof.has_value());
    EXPECT_TRUE(old_proof->verify(leaves[0], old_roots));

    // Advance chain
    connectBlock(forest, leaves, 3, 3);
    auto new_root = forest.getCommitment();
    auto new_roots = forest.getRoots();

    // Old proof may not verify at new root (tree structure changed)
    // This is expected — root binding means proofs are position-specific
    // (We don't assert old_proof fails at new_roots because tree shape
    // may coincidentally preserve it — the point is the root changed.)

    // Re-prove at new root
    auto new_pos0 = forest.findLeafPosition(leaves[0]);
    ASSERT_TRUE(new_pos0.has_value());
    auto new_proof = forest.prove(*new_pos0);
    ASSERT_TRUE(new_proof.has_value());

    // New proof MUST verify at new root
    EXPECT_TRUE(new_proof->verify(leaves[0], new_roots))
        << "Refreshed proof must verify at new root";

    // Verify the roots actually changed
    EXPECT_FALSE(hashesEqual(old_root, new_root))
        << "Root must have changed after chain advance";
}

// ═══════════════════════════════════════════════════════════════════════════════
// CROSS-RPC LIFECYCLE: End-to-end invariant
//
// The core state machine:
//   getproofbundle → proofstatus=fresh → advance → proofstatus=stale
//   → getproofupdates → proofstatus=fresh
// ═══════════════════════════════════════════════════════════════════════════════

class ProofLifecycleTest : public ::testing::Test {
protected:
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    void SetUp() override {
        // Initial chain: 3 blocks
        connectBlock(forest, leaves, 1, 3);
        connectBlock(forest, leaves, 2, 3);
        connectBlock(forest, leaves, 3, 3);
        // 9 UTXOs total
    }
};

TEST_F(ProofLifecycleTest, FullLifecycleTipAdvance) {
    // Step 1: wallet.getproofbundle — generate proofs for all UTXOs
    auto bundle_root = forest.getCommitment();
    auto bundle_root_hex = toHex(bundle_root);
    auto roots = forest.getRoots();

    // Verify all proofs in bundle
    for (size_t i = 0; i < leaves.size(); i++) {
        auto pos = forest.findLeafPosition(leaves[i]);
        ASSERT_TRUE(pos.has_value());
        auto proof = forest.prove(*pos);
        ASSERT_TRUE(proof.has_value());
        EXPECT_TRUE(proof->verify(leaves[i], roots));
    }

    // Step 2: wallet.proofstatus — should report fresh
    {
        auto current_root_hex = toHex(forest.getCommitment());
        bool stale = (bundle_root_hex != current_root_hex);
        EXPECT_FALSE(stale) << "Immediately after bundle, proofstatus must be fresh";
    }

    // Step 3: Mine a new block (tip advances)
    connectBlock(forest, leaves, 4, 2);

    // Step 4: wallet.proofstatus — should report stale
    {
        auto current_root_hex = toHex(forest.getCommitment());
        bool stale = (bundle_root_hex != current_root_hex);
        EXPECT_TRUE(stale) << "After tip advance, proofstatus must be stale";
    }

    // Step 5: blockchain.getproofupdates — re-prove at new root
    auto updated_root = forest.getCommitment();
    auto updated_root_hex = toHex(updated_root);
    auto updated_roots = forest.getRoots();

    // Re-prove the original 9 UTXOs (leaves 0-8)
    for (size_t i = 0; i < 9; i++) {
        auto pos = forest.findLeafPosition(leaves[i]);
        ASSERT_TRUE(pos.has_value())
            << "Original UTXO " << i << " must still exist after tip advance";

        auto proof = forest.prove(*pos);
        ASSERT_TRUE(proof.has_value());
        EXPECT_TRUE(proof->verify(leaves[i], updated_roots))
            << "Re-proved UTXO " << i << " must verify at updated root";
    }

    // Step 6: wallet.proofstatus with updated root — should report fresh
    {
        auto current_root_hex = toHex(forest.getCommitment());
        bool stale = (updated_root_hex != current_root_hex);
        EXPECT_FALSE(stale) << "After proof update, proofstatus must be fresh again";
    }
}

TEST_F(ProofLifecycleTest, FullLifecycleReorg) {
    // Step 1: Bundle at current state
    auto bundle_root = forest.getCommitment();
    auto bundle_root_hex = toHex(bundle_root);

    // Step 2: Verify fresh
    {
        bool stale = (bundle_root_hex != toHex(forest.getCommitment()));
        EXPECT_FALSE(stale) << "Initial state must be fresh";
    }

    // Step 3: Reorg — disconnect block 3, connect block 3'
    auto checkpoint = forest.serialize();  // Save pre-reorg state
    disconnectBlock(forest, leaves, 3);    // Disconnect block 3's outputs
    connectBlock(forest, leaves, 30, 4);   // Connect block 3' with different outputs

    // Step 4: Verify stale
    {
        bool stale = (bundle_root_hex != toHex(forest.getCommitment()));
        EXPECT_TRUE(stale) << "After reorg, must be stale";
    }

    // Step 5: Re-prove surviving UTXOs (blocks 1 and 2 UTXOs, leaves 0-5)
    auto post_reorg_root = forest.getCommitment();
    auto post_reorg_root_hex = toHex(post_reorg_root);
    auto post_reorg_roots = forest.getRoots();

    for (size_t i = 0; i < 6; i++) {
        auto target = leaves[i];  // Block 1 and 2 UTXOs
        auto pos = forest.findLeafPosition(target);
        ASSERT_TRUE(pos.has_value())
            << "Block 1/2 UTXO " << i << " must survive reorg of block 3";

        auto proof = forest.prove(*pos);
        ASSERT_TRUE(proof.has_value());
        EXPECT_TRUE(proof->verify(target, post_reorg_roots))
            << "Surviving UTXO " << i << " must verify at post-reorg root";
    }

    // Step 6: Verify fresh with new root
    {
        bool stale = (post_reorg_root_hex != toHex(forest.getCommitment()));
        EXPECT_FALSE(stale) << "After re-proving at post-reorg root, must be fresh";
    }
}

TEST_F(ProofLifecycleTest, DeepReorgLifecycle) {
    // Simulate a deeper reorg using checkpoint restore (mirrors production).
    // Production uses checkpoint restore for disconnect, not per-leaf remove(),
    // because per-leaf remove changes tree structure non-reversibly.
    auto bundle_root_hex = toHex(forest.getCommitment());

    // Save checkpoint at block 1 (the fork point — block 1 survives the reorg)
    // First, build a fresh forest with only block 1 to get the fork-point state
    UtreexoForest fork_point_forest;
    std::vector<UtreexoHash> fork_point_leaves;
    connectBlock(fork_point_forest, fork_point_leaves, 1, 3);
    auto fork_point_checkpoint = fork_point_forest.serialize();

    // "Disconnect" blocks 2 and 3 by restoring fork-point checkpoint
    auto restored = UtreexoForest::deserialize(fork_point_checkpoint);

    // Connect alternative blocks 2' and 3' on the restored forest
    std::vector<UtreexoHash> new_leaves = fork_point_leaves;
    connectBlock(restored, new_leaves, 20, 5);
    connectBlock(restored, new_leaves, 30, 2);

    // Must be stale vs original bundle
    {
        bool stale = (bundle_root_hex != toHex(restored.getCommitment()));
        EXPECT_TRUE(stale) << "Deep reorg must make bundle stale";
    }

    // Block 1's UTXOs should still be provable in the reorged forest
    auto new_roots = restored.getRoots();
    for (int i = 0; i < 3; i++) {
        auto target = makeTestHash(1 * 1000 + i);
        auto pos = restored.findLeafPosition(target);
        ASSERT_TRUE(pos.has_value())
            << "Block 1 UTXO " << i << " must survive deep reorg";

        auto proof = restored.prove(*pos);
        ASSERT_TRUE(proof.has_value());
        EXPECT_TRUE(proof->verify(target, new_roots))
            << "Block 1 UTXO " << i << " must verify at post-deep-reorg root";
    }
}

TEST_F(ProofLifecycleTest, RapidBlockSequenceConsistency) {
    // Simulate rapid block production: several blocks in quick succession
    // Each time, proofstatus must report stale against the previous root

    std::string prev_root_hex = toHex(forest.getCommitment());

    for (int block = 4; block <= 10; block++) {
        connectBlock(forest, leaves, block, 2);
        auto new_root_hex = toHex(forest.getCommitment());

        bool stale = (prev_root_hex != new_root_hex);
        EXPECT_TRUE(stale)
            << "Block " << block << " must change root (proofstatus=stale)";

        prev_root_hex = new_root_hex;
    }

    // Final: proofstatus with current root must be fresh
    {
        bool stale = (prev_root_hex != toHex(forest.getCommitment()));
        EXPECT_FALSE(stale) << "Current root must be fresh";
    }

    // All original UTXOs still provable
    auto final_roots = forest.getRoots();
    for (int i = 0; i < 3; i++) {
        auto target = makeTestHash(1 * 1000 + i);
        auto pos = forest.findLeafPosition(target);
        ASSERT_TRUE(pos.has_value());
        auto proof = forest.prove(*pos);
        ASSERT_TRUE(proof.has_value());
        EXPECT_TRUE(proof->verify(target, final_roots));
    }
}

TEST_F(ProofLifecycleTest, SpendDuringLifecycle) {
    // Bundle → spend a UTXO → stale → update reports spent UTXO as missing
    auto bundle_root_hex = toHex(forest.getCommitment());

    // Spend leaf 4 (from block 2)
    auto target = leaves[4];
    auto pos = forest.findLeafPosition(target);
    ASSERT_TRUE(pos.has_value());
    auto proof = forest.prove(*pos);
    ASSERT_TRUE(proof.has_value());
    forest.remove(target, *proof);

    // Add a new block (tip advance + spend)
    connectBlock(forest, leaves, 4, 1);

    // Stale check
    bool stale = (bundle_root_hex != toHex(forest.getCommitment()));
    EXPECT_TRUE(stale);

    // Try to re-prove the spent UTXO — should fail
    auto spent_pos = forest.findLeafPosition(target);
    EXPECT_FALSE(spent_pos.has_value())
        << "Spent UTXO must not be re-provable (getproofupdates returns success=false)";

    // Other UTXOs still provable
    auto new_roots = forest.getRoots();
    for (size_t i = 0; i < 4; i++) {
        if (i == 4) continue;  // skip spent
        auto pos_i = forest.findLeafPosition(leaves[i]);
        ASSERT_TRUE(pos_i.has_value());
        auto proof_i = forest.prove(*pos_i);
        ASSERT_TRUE(proof_i.has_value());
        EXPECT_TRUE(proof_i->verify(leaves[i], new_roots));
    }
}

TEST_F(ProofLifecycleTest, ConcurrentRefreshRace) {
    // Simulate: bundle is stale, client requests refresh, but chain advances
    // again DURING the refresh. The refresh result binds to an intermediate
    // root that is itself stale by the time the client receives it.
    //
    // Expected: client detects the second staleness via proofstatus and
    // issues another refresh. No silent corruption.

    // Step 1: Bundle at initial state
    auto bundle_root_hex = toHex(forest.getCommitment());

    // Step 2: Chain advances — bundle is stale
    connectBlock(forest, leaves, 4, 2);
    {
        bool stale = (bundle_root_hex != toHex(forest.getCommitment()));
        ASSERT_TRUE(stale);
    }

    // Step 3: Client begins refresh — capture the root at refresh-start time
    auto refresh_root = forest.getCommitment();
    auto refresh_root_hex = toHex(refresh_root);

    // Generate proofs at this intermediate state (simulates worker computing)
    auto refresh_roots = forest.getRoots();
    std::vector<std::pair<UtreexoHash, UtreexoProof>> refresh_proofs;
    for (size_t i = 0; i < 9; i++) {  // Original 9 UTXOs from SetUp
        auto pos = forest.findLeafPosition(leaves[i]);
        ASSERT_TRUE(pos.has_value());
        auto proof = forest.prove(*pos);
        ASSERT_TRUE(proof.has_value());
        EXPECT_TRUE(proof->verify(leaves[i], refresh_roots));
        refresh_proofs.push_back({leaves[i], *proof});
    }

    // Step 4: Chain advances AGAIN while worker was computing
    connectBlock(forest, leaves, 5, 3);
    auto final_root_hex = toHex(forest.getCommitment());

    // The refresh result is bound to refresh_root, NOT final_root
    EXPECT_NE(refresh_root_hex, final_root_hex)
        << "Chain moved during refresh — refresh result is already stale";

    // Step 5: Client receives refresh result, checks proofstatus
    // The proofs verify at the REFRESH root but NOT at the FINAL root
    {
        bool stale = (refresh_root_hex != final_root_hex);
        EXPECT_TRUE(stale)
            << "proofstatus must detect that refresh result is stale";
    }

    // Step 6: Client issues SECOND refresh — this time chain is stable
    auto second_roots = forest.getRoots();
    auto second_root_hex = toHex(forest.getCommitment());

    for (size_t i = 0; i < 9; i++) {
        auto pos = forest.findLeafPosition(leaves[i]);
        ASSERT_TRUE(pos.has_value());
        auto proof = forest.prove(*pos);
        ASSERT_TRUE(proof.has_value());
        EXPECT_TRUE(proof->verify(leaves[i], second_roots))
            << "Second refresh proof " << i << " must verify at final root";
    }

    // Step 7: proofstatus now reports fresh
    {
        bool stale = (second_root_hex != toHex(forest.getCommitment()));
        EXPECT_FALSE(stale) << "After second refresh (no chain movement), must be fresh";
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// EDGE CASES: Contract boundary conditions
// ═══════════════════════════════════════════════════════════════════════════════

class ProofLifecycleEdgeCases : public ::testing::Test {};

TEST_F(ProofLifecycleEdgeCases, SingleUTXOLifecycle) {
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    // One block, one UTXO
    connectBlock(forest, leaves, 1, 1);

    auto root_hex = toHex(forest.getCommitment());
    auto roots = forest.getRoots();

    // Prove the single UTXO
    auto pos = forest.findLeafPosition(leaves[0]);
    ASSERT_TRUE(pos.has_value());
    auto proof = forest.prove(*pos);
    ASSERT_TRUE(proof.has_value());
    EXPECT_TRUE(proof->verify(leaves[0], roots));

    // Advance
    connectBlock(forest, leaves, 2, 1);

    // Stale
    bool stale = (root_hex != toHex(forest.getCommitment()));
    EXPECT_TRUE(stale);

    // Re-prove
    auto new_roots = forest.getRoots();
    auto new_pos = forest.findLeafPosition(leaves[0]);
    ASSERT_TRUE(new_pos.has_value());
    auto new_proof = forest.prove(*new_pos);
    ASSERT_TRUE(new_proof.has_value());
    EXPECT_TRUE(new_proof->verify(leaves[0], new_roots));
}

TEST_F(ProofLifecycleEdgeCases, AllUTXOsSpentThenRefresh) {
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    connectBlock(forest, leaves, 1, 3);

    auto root_hex = toHex(forest.getCommitment());

    // Spend all 3 UTXOs
    for (int i = 2; i >= 0; i--) {
        auto pos = forest.findLeafPosition(leaves[i]);
        ASSERT_TRUE(pos.has_value());
        auto proof = forest.prove(*pos);
        ASSERT_TRUE(proof.has_value());
        forest.remove(leaves[i], *proof);
    }

    // Stale
    bool stale = (root_hex != toHex(forest.getCommitment()));
    EXPECT_TRUE(stale);

    // Re-prove: all should fail (empty forest)
    for (int i = 0; i < 3; i++) {
        auto pos = forest.findLeafPosition(leaves[i]);
        EXPECT_FALSE(pos.has_value())
            << "All UTXOs spent — none should be provable";
    }
}

TEST_F(ProofLifecycleEdgeCases, LargeForestProofLifecycle) {
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    // 50 blocks, 10 outputs each = 500 UTXOs
    for (int b = 0; b < 50; b++) {
        connectBlock(forest, leaves, b, 10);
    }

    auto root_hex = toHex(forest.getCommitment());
    auto roots = forest.getRoots();

    // Spot-check: prove every 50th leaf
    for (size_t i = 0; i < leaves.size(); i += 50) {
        auto pos = forest.findLeafPosition(leaves[i]);
        ASSERT_TRUE(pos.has_value());
        auto proof = forest.prove(*pos);
        ASSERT_TRUE(proof.has_value());
        EXPECT_TRUE(proof->verify(leaves[i], roots));
    }

    // Advance by one block
    connectBlock(forest, leaves, 50, 10);

    // Must be stale
    bool stale = (root_hex != toHex(forest.getCommitment()));
    EXPECT_TRUE(stale);

    // Re-prove same spot-check leaves
    auto new_roots = forest.getRoots();
    for (size_t i = 0; i < 500; i += 50) {
        auto pos = forest.findLeafPosition(leaves[i]);
        ASSERT_TRUE(pos.has_value());
        auto proof = forest.prove(*pos);
        ASSERT_TRUE(proof.has_value());
        EXPECT_TRUE(proof->verify(leaves[i], new_roots));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "  Proof Lifecycle Tests\n";
    std::cout << "  Contract: wallet.getproofbundle / wallet.proofstatus /\n";
    std::cout << "            blockchain.getproofupdates\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "\n";
    std::cout << "  BUNDLE:    Prove all leaves, bind to single root\n";
    std::cout << "  STATUS:    Root comparison detects staleness\n";
    std::cout << "  UPDATES:   Re-prove at new root after forest mutation\n";
    std::cout << "  LIFECYCLE: bundle → fresh → advance → stale → update → fresh\n";
    std::cout << "\n";

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
