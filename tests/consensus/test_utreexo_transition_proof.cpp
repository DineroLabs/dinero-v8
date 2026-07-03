/**
 * Utreexo Transition Proof Test Suite
 *
 * Tests the UtreexoTransitionProof type:
 * - Addition-only transitions (coinbase blocks)
 * - Single-spend transitions
 * - Multi-spend transitions
 * - Tampered proof rejection
 * - Serialization round-trip
 * - Forest-stump agreement
 */

#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_stump.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <cstring>

using namespace dinero;
using namespace dinero::consensus;

// ============================================================================
// Helpers
// ============================================================================

static UtreexoHash makeLeafHash(uint64_t utxo_id, uint64_t value) {
    uint256 txid;
    std::memset(txid.data, 0, 32);
    std::memcpy(txid.data, &utxo_id, sizeof(utxo_id));

    std::vector<uint8_t> spk = {0x51, 0x20};
    spk.resize(34, 0x00);

    return HashUTXOLegacy(txid, 0, value, spk);
}

static int tests_passed = 0;
static int tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        assert(false); \
    } else { \
        tests_passed++; \
    } \
} while(0)

// ============================================================================
// Test 1: Addition-only transition (no deletions)
// ============================================================================

void test_addition_only_transition() {
    std::cout << "Test 1: Addition-only transition..." << std::endl;

    UtreexoForest forest;

    // Pre-state: empty forest
    auto roots_before = forest.getIndexedRoots();
    uint64_t leaves_before = forest.getNumLeaves();

    // Simulate adding 3 new UTXOs (like a coinbase block)
    std::vector<UtreexoHash> additions;
    for (uint64_t i = 0; i < 3; ++i) {
        additions.push_back(makeLeafHash(100 + i, 5000 + i));
    }

    // Build transition proof manually (no block needed for unit test)
    UtreexoTransitionProof tp;
    tp.roots_before = roots_before;
    tp.num_leaves_before = leaves_before;
    // No deletions
    tp.roots_after_deletions = roots_before;  // No deletions → same roots
    tp.addition_hashes = additions;

    // Apply additions to forest to get commitment_after
    UtreexoForest snapshot = forest.clone();
    for (const auto& leaf : additions) {
        snapshot.add(leaf);
    }
    tp.commitment_after = snapshot.getCommitment();
    tp.num_leaves_after = tp.num_leaves_before + additions.size();

    // Verify
    TEST_ASSERT(tp.verify(), "Addition-only transition proof should verify");
    TEST_ASSERT(tp.num_leaves_after == 3, "Should have 3 leaves after");

    // Verify stump agrees with forest
    UtreexoStump stump = UtreexoStump::fromForest(snapshot);
    TEST_ASSERT(stump.verifyCommitment(tp.commitment_after),
                "Stump commitment should match forest commitment");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 2: Single-spend transition
// ============================================================================

void test_single_spend_transition() {
    std::cout << "Test 2: Single-spend transition..." << std::endl;

    UtreexoForest forest;

    // Add 3 UTXOs to the forest
    std::vector<UtreexoHash> initial_leaves;
    for (uint64_t i = 0; i < 3; ++i) {
        UtreexoHash leaf = makeLeafHash(i, 1000 * (i + 1));
        initial_leaves.push_back(leaf);
        forest.add(leaf);
    }

    auto roots_before = forest.getIndexedRoots();
    uint64_t leaves_before = forest.getNumLeaves();

    // Spend leaf 1 (the second one)
    UtreexoHash target = initial_leaves[1];
    auto pos_opt = forest.findLeafPosition(target);
    TEST_ASSERT(pos_opt.has_value(), "Target leaf should exist");

    auto proof_opt = forest.prove(pos_opt.value());
    TEST_ASSERT(proof_opt.has_value(), "Should be able to prove target");

    // Build spend proof
    BlockUtreexoProof spend_proof;
    spend_proof.targets = {target};
    spend_proof.positions = {pos_opt.value()};
    spend_proof.proof_hashes = proof_opt.value().siblings;
    spend_proof.numLeaves = leaves_before;

    // Clone and apply deletion
    UtreexoForest snapshot = forest.clone();
    snapshot.remove(target, proof_opt.value());
    auto roots_after_del = snapshot.getIndexedRoots();

    // Add 2 new outputs
    std::vector<UtreexoHash> additions;
    for (uint64_t i = 0; i < 2; ++i) {
        additions.push_back(makeLeafHash(200 + i, 7000 + i));
    }
    for (const auto& leaf : additions) {
        snapshot.add(leaf);
    }

    // Build transition proof
    UtreexoTransitionProof tp;
    tp.roots_before = roots_before;
    tp.num_leaves_before = leaves_before;
    tp.deletion_targets = spend_proof.targets;
    tp.deletion_positions = spend_proof.positions;
    tp.deletion_proof_hashes = spend_proof.proof_hashes;
    tp.roots_after_deletions = roots_after_del;
    tp.addition_hashes = additions;
    tp.commitment_after = snapshot.getCommitment();
    tp.num_leaves_after = leaves_before + additions.size();

    // Verify
    TEST_ASSERT(tp.verify(), "Single-spend transition proof should verify");
    TEST_ASSERT(tp.num_leaves_after == 5, "Should have 5 leaves (3 + 2 added)");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 3: Multi-spend transition
// ============================================================================

void test_multi_spend_transition() {
    std::cout << "Test 3: Multi-spend transition..." << std::endl;

    UtreexoForest forest;

    // Add 5 UTXOs
    std::vector<UtreexoHash> initial_leaves;
    for (uint64_t i = 0; i < 5; ++i) {
        UtreexoHash leaf = makeLeafHash(i, 500 * (i + 1));
        initial_leaves.push_back(leaf);
        forest.add(leaf);
    }

    auto roots_before = forest.getIndexedRoots();
    uint64_t leaves_before = forest.getNumLeaves();

    // Spend leaves 0, 2, 4
    std::vector<UtreexoHash> targets;
    std::vector<uint64_t> positions;
    std::vector<UtreexoHash> all_proof_hashes;

    // IMPORTANT: Prove all targets against the ORIGINAL forest first
    // (batch proof must be against pre-state, not sequential post-removal states)
    for (int idx : {0, 2, 4}) {
        UtreexoHash target = initial_leaves[idx];
        auto pos_opt = forest.findLeafPosition(target);
        TEST_ASSERT(pos_opt.has_value(), "Target should exist in forest");

        auto proof_opt = forest.prove(pos_opt.value());
        TEST_ASSERT(proof_opt.has_value(), "Should be able to prove target");

        targets.push_back(target);
        positions.push_back(pos_opt.value());
        all_proof_hashes.insert(all_proof_hashes.end(),
            proof_opt.value().siblings.begin(),
            proof_opt.value().siblings.end());
    }

    // Now do removals on a clone to get roots_after_deletions
    UtreexoForest snapshot = forest.clone();
    for (size_t i = 0; i < targets.size(); ++i) {
        auto pos_opt = snapshot.findLeafPosition(targets[i]);
        TEST_ASSERT(pos_opt.has_value(), "Target should exist in snapshot");
        auto proof_opt = snapshot.prove(pos_opt.value());
        TEST_ASSERT(proof_opt.has_value(), "Should be able to prove in snapshot");
        snapshot.remove(targets[i], proof_opt.value());
    }

    auto roots_after_del = snapshot.getIndexedRoots();

    // Add 4 new outputs
    std::vector<UtreexoHash> additions;
    for (uint64_t i = 0; i < 4; ++i) {
        additions.push_back(makeLeafHash(300 + i, 9000 + i));
    }
    for (const auto& leaf : additions) {
        snapshot.add(leaf);
    }

    // Build transition proof
    UtreexoTransitionProof tp;
    tp.roots_before = roots_before;
    tp.num_leaves_before = leaves_before;
    tp.deletion_targets = targets;
    tp.deletion_positions = positions;
    tp.deletion_proof_hashes = all_proof_hashes;
    tp.roots_after_deletions = roots_after_del;
    tp.addition_hashes = additions;
    tp.commitment_after = snapshot.getCommitment();
    tp.num_leaves_after = leaves_before + additions.size();

    // Verify
    TEST_ASSERT(tp.verify(), "Multi-spend transition proof should verify");
    TEST_ASSERT(tp.num_leaves_after == 9, "Should have 9 leaves (5 + 4 added)");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 4: Tampered commitment rejection
// ============================================================================

void test_tampered_commitment_rejection() {
    std::cout << "Test 4: Tampered commitment rejection..." << std::endl;

    UtreexoForest forest;

    // Add a leaf
    UtreexoHash leaf = makeLeafHash(42, 1000);
    forest.add(leaf);

    // Build valid addition-only transition
    std::vector<UtreexoHash> additions = {makeLeafHash(99, 2000)};

    UtreexoForest snapshot = forest.clone();
    for (const auto& a : additions) snapshot.add(a);

    UtreexoTransitionProof tp;
    tp.roots_before = forest.getIndexedRoots();
    tp.num_leaves_before = forest.getNumLeaves();
    tp.roots_after_deletions = tp.roots_before;
    tp.addition_hashes = additions;
    tp.commitment_after = snapshot.getCommitment();
    tp.num_leaves_after = tp.num_leaves_before + additions.size();

    // Valid first
    TEST_ASSERT(tp.verify(), "Valid proof should verify");

    // Tamper with commitment
    UtreexoTransitionProof tampered = tp;
    if (!tampered.commitment_after.empty()) {
        tampered.commitment_after[0] ^= 0xFF;
    }
    TEST_ASSERT(!tampered.verify(), "Tampered commitment should fail");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 5: Tampered addition rejection
// ============================================================================

void test_tampered_addition_rejection() {
    std::cout << "Test 5: Tampered addition rejection..." << std::endl;

    UtreexoForest forest;
    UtreexoHash leaf = makeLeafHash(42, 1000);
    forest.add(leaf);

    std::vector<UtreexoHash> additions = {makeLeafHash(99, 2000)};

    UtreexoForest snapshot = forest.clone();
    for (const auto& a : additions) snapshot.add(a);

    UtreexoTransitionProof tp;
    tp.roots_before = forest.getIndexedRoots();
    tp.num_leaves_before = forest.getNumLeaves();
    tp.roots_after_deletions = tp.roots_before;
    tp.addition_hashes = additions;
    tp.commitment_after = snapshot.getCommitment();
    tp.num_leaves_after = tp.num_leaves_before + additions.size();

    TEST_ASSERT(tp.verify(), "Valid proof should verify");

    // Tamper with addition
    UtreexoTransitionProof tampered = tp;
    if (!tampered.addition_hashes.empty() && !tampered.addition_hashes[0].empty()) {
        tampered.addition_hashes[0][0] ^= 0xFF;
    }
    TEST_ASSERT(!tampered.verify(), "Tampered addition should fail");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 6: Tampered intermediate roots rejection
// ============================================================================

void test_tampered_deletion_rejection() {
    std::cout << "Test 6: Tampered intermediate roots rejection..." << std::endl;

    UtreexoForest forest;

    // Add 3 leaves
    std::vector<UtreexoHash> leaves;
    for (uint64_t i = 0; i < 3; ++i) {
        UtreexoHash leaf = makeLeafHash(i, 1000);
        leaves.push_back(leaf);
        forest.add(leaf);
    }

    // Build proof for spending leaf 0
    auto pos = forest.findLeafPosition(leaves[0]);
    auto proof = forest.prove(pos.value());

    UtreexoForest snapshot = forest.clone();
    snapshot.remove(leaves[0], proof.value());

    std::vector<UtreexoHash> additions = {makeLeafHash(50, 500)};
    for (const auto& a : additions) snapshot.add(a);

    UtreexoTransitionProof tp;
    tp.roots_before = forest.getIndexedRoots();
    tp.num_leaves_before = forest.getNumLeaves();
    tp.deletion_targets = {leaves[0]};
    tp.deletion_positions = {pos.value()};
    tp.deletion_proof_hashes = proof.value().siblings;
    tp.roots_after_deletions = snapshot.getIndexedRoots();
    // Re-add to get real roots_after_deletions (we removed + added)
    // Actually we need intermediate: after delete, before add
    UtreexoForest snap2 = forest.clone();
    snap2.remove(leaves[0], proof.value());
    tp.roots_after_deletions = snap2.getIndexedRoots();
    // Re-apply additions to snap2
    for (const auto& a : additions) snap2.add(a);
    tp.commitment_after = snap2.getCommitment();

    tp.addition_hashes = additions;
    tp.num_leaves_after = tp.num_leaves_before + additions.size();

    TEST_ASSERT(tp.verify(), "Valid proof with deletion should verify");

    // Tamper with roots_after_deletions (intermediate state)
    // This breaks the addition path: additions applied to wrong intermediate roots
    // will produce a different commitment.
    UtreexoTransitionProof tampered = tp;
    for (auto& root : tampered.roots_after_deletions) {
        if (root.has_value() && !root.value().empty()) {
            root.value()[0] ^= 0xFF;
            break;
        }
    }
    TEST_ASSERT(!tampered.verify(), "Tampered intermediate roots should fail verification");

    // Tamper with deletion targets (now caught by batch proof verification)
    UtreexoTransitionProof tampered2 = tp;
    if (!tampered2.deletion_targets.empty() && !tampered2.deletion_targets[0].empty()) {
        tampered2.deletion_targets[0][0] ^= 0xFF;
    }
    TEST_ASSERT(!tampered2.verify(), "Tampered deletion targets should fail batch proof");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 7: Serialization round-trip
// ============================================================================

void test_serialization_round_trip() {
    std::cout << "Test 7: Serialization round-trip..." << std::endl;

    UtreexoForest forest;
    UtreexoHash leaf = makeLeafHash(42, 1000);
    forest.add(leaf);

    std::vector<UtreexoHash> additions = {makeLeafHash(99, 2000), makeLeafHash(100, 3000)};

    UtreexoForest snapshot = forest.clone();
    for (const auto& a : additions) snapshot.add(a);

    UtreexoTransitionProof tp;
    tp.roots_before = forest.getIndexedRoots();
    tp.num_leaves_before = forest.getNumLeaves();
    tp.roots_after_deletions = tp.roots_before;
    tp.addition_hashes = additions;
    tp.commitment_after = snapshot.getCommitment();
    tp.num_leaves_after = tp.num_leaves_before + additions.size();

    // Serialize
    auto serialized = tp.serialize();
    TEST_ASSERT(!serialized.empty(), "Serialized data should not be empty");
    TEST_ASSERT(serialized.size() == tp.serializedSize(), "Size should match");

    // Deserialize
    auto tp2 = UtreexoTransitionProof::deserialize(serialized);

    // Compare fields
    TEST_ASSERT(tp2.num_leaves_before == tp.num_leaves_before, "num_leaves_before match");
    TEST_ASSERT(tp2.num_leaves_after == tp.num_leaves_after, "num_leaves_after match");
    TEST_ASSERT(tp2.roots_before.size() == tp.roots_before.size(), "roots_before size match");
    TEST_ASSERT(tp2.addition_hashes.size() == tp.addition_hashes.size(), "additions size match");
    TEST_ASSERT(tp2.commitment_after == tp.commitment_after, "commitment_after match");

    // Verify deserialized proof
    TEST_ASSERT(tp2.verify(), "Deserialized proof should verify");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 8: Empty transition (no deletions, no additions)
// ============================================================================

void test_empty_transition() {
    std::cout << "Test 8: Empty transition..." << std::endl;

    UtreexoForest forest;
    // Add a leaf so forest is non-empty
    forest.add(makeLeafHash(1, 100));

    UtreexoTransitionProof tp;
    tp.roots_before = forest.getIndexedRoots();
    tp.num_leaves_before = forest.getNumLeaves();
    tp.roots_after_deletions = tp.roots_before;
    // No additions
    tp.commitment_after = forest.getCommitment();  // Should stay the same
    tp.num_leaves_after = tp.num_leaves_before;

    TEST_ASSERT(tp.verify(), "Empty transition (no changes) should verify");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 9: Forest-Stump Proof Agreement
// ============================================================================

void test_forest_stump_proof_agreement() {
    std::cout << "Test 9: Forest-Stump Proof Agreement..." << std::endl;

    // For various numLeaves values, build forest, extract stump,
    // prove each leaf from forest, verify each proof against stump.
    std::vector<uint64_t> test_sizes = {2, 3, 4, 5, 6, 7, 8, 13, 16, 17};

    for (uint64_t n : test_sizes) {
        UtreexoForest forest;
        std::vector<UtreexoHash> leaves;

        for (uint64_t i = 0; i < n; ++i) {
            UtreexoHash leaf = makeLeafHash(1000 + n * 100 + i, 100 + i);
            leaves.push_back(leaf);
            forest.add(leaf);
        }

        UtreexoStump stump = UtreexoStump::fromForest(forest);

        // Verify each leaf individually
        for (uint64_t i = 0; i < n; ++i) {
            auto pos_opt = forest.findLeafPosition(leaves[i]);
            TEST_ASSERT(pos_opt.has_value(),
                "Leaf should exist in forest (n=" + std::to_string(n) +
                ", i=" + std::to_string(i) + ")");

            auto proof_opt = forest.prove(pos_opt.value());
            TEST_ASSERT(proof_opt.has_value(),
                "Should get proof from forest (n=" + std::to_string(n) +
                ", i=" + std::to_string(i) + ")");

            // Build BlockUtreexoProof for stump verification
            BlockUtreexoProof bp;
            bp.targets = {leaves[i]};
            bp.positions = {pos_opt.value()};
            bp.proof_hashes = proof_opt.value().siblings;
            bp.numLeaves = forest.getNumLeaves();

            TEST_ASSERT(stump.verifyBlockProof(bp),
                "Stump should verify forest proof (n=" + std::to_string(n) +
                ", i=" + std::to_string(i) + ")");
        }
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 10: Chained transition proof verification (CSN stump-only mode)
// ============================================================================

void test_chained_csn_stump_verification() {
    std::cout << "Test 10: Chained CSN stump-only verification (5 blocks)..." << std::endl;

    UtreexoForest forest;

    // CSN stump-only state: just commitment + numLeaves (no forest)
    UtreexoHash csn_commitment = forest.getCommitment();
    uint64_t csn_num_leaves = forest.getNumLeaves();

    // Global leaf counter for unique utxo IDs
    uint64_t next_utxo_id = 1;

    // Track all live leaves for spending in future blocks
    std::vector<UtreexoHash> live_leaves;

    for (int block = 0; block < 5; ++block) {
        auto roots_before = forest.getIndexedRoots();
        uint64_t leaves_before = forest.getNumLeaves();

        // Determine deletions: spend some existing UTXOs (starting from block 2)
        std::vector<UtreexoHash> del_targets;
        std::vector<uint64_t> del_positions;
        std::vector<UtreexoHash> del_proof_hashes;

        size_t num_spends = 0;
        if (block >= 2 && !live_leaves.empty()) {
            num_spends = std::min((size_t)2, live_leaves.size());
        }

        // Prove targets against original forest
        for (size_t i = 0; i < num_spends; ++i) {
            UtreexoHash target = live_leaves[i];
            auto pos_opt = forest.findLeafPosition(target);
            TEST_ASSERT(pos_opt.has_value(),
                "Target should exist in forest (block " + std::to_string(block) + ")");

            auto proof_opt = forest.prove(pos_opt.value());
            TEST_ASSERT(proof_opt.has_value(),
                "Should prove target (block " + std::to_string(block) + ")");

            del_targets.push_back(target);
            del_positions.push_back(pos_opt.value());
            del_proof_hashes.insert(del_proof_hashes.end(),
                proof_opt.value().siblings.begin(),
                proof_opt.value().siblings.end());
        }

        // Clone forest for intermediate state capture
        UtreexoForest snapshot = forest.clone();

        // Apply deletions on snapshot
        for (size_t i = 0; i < del_targets.size(); ++i) {
            auto pos_opt = snapshot.findLeafPosition(del_targets[i]);
            TEST_ASSERT(pos_opt.has_value(),
                "Target should exist in snapshot (block " + std::to_string(block) + ")");
            auto proof_opt = snapshot.prove(pos_opt.value());
            TEST_ASSERT(proof_opt.has_value(),
                "Should prove in snapshot (block " + std::to_string(block) + ")");
            snapshot.remove(del_targets[i], proof_opt.value());
        }
        auto roots_after_del = snapshot.getIndexedRoots();

        // Add 3 new UTXOs per block
        std::vector<UtreexoHash> additions;
        for (int i = 0; i < 3; ++i) {
            additions.push_back(makeLeafHash(next_utxo_id++, 1000 + block * 100 + i));
        }
        for (const auto& leaf : additions) {
            snapshot.add(leaf);
        }

        // Build transition proof
        UtreexoTransitionProof tp;
        tp.roots_before = roots_before;
        tp.num_leaves_before = leaves_before;
        tp.deletion_targets = del_targets;
        tp.deletion_positions = del_positions;
        tp.deletion_proof_hashes = del_proof_hashes;
        tp.roots_after_deletions = roots_after_del;
        tp.addition_hashes = additions;
        tp.commitment_after = snapshot.getCommitment();
        tp.num_leaves_after = leaves_before + additions.size();

        // === CSN verification (stump-only, no forest!) ===

        // Step 1: Commitment continuity
        auto stump_before = UtreexoStump::fromRoots(tp.roots_before, tp.num_leaves_before);
        UtreexoHash tp_commitment_before = stump_before.getCommitment();
        TEST_ASSERT(tp_commitment_before == csn_commitment,
            "CSN commitment continuity must hold (block " + std::to_string(block) + ")");
        TEST_ASSERT(tp.num_leaves_before == csn_num_leaves,
            "CSN numLeaves continuity must hold (block " + std::to_string(block) + ")");

        // Step 2: Stateless verification
        TEST_ASSERT(tp.verify(),
            "Transition proof must verify (block " + std::to_string(block) + ")");

        // Step 3: Update CSN stump state
        csn_commitment = tp.commitment_after;
        csn_num_leaves = tp.num_leaves_after;

        // === Advance the real forest (for next block's proof generation) ===
        // Apply deletions to real forest
        for (size_t i = 0; i < del_targets.size(); ++i) {
            auto pos_opt = forest.findLeafPosition(del_targets[i]);
            auto proof_opt = forest.prove(pos_opt.value());
            forest.remove(del_targets[i], proof_opt.value());
        }
        // Apply additions to real forest
        for (const auto& leaf : additions) {
            forest.add(leaf);
        }

        // Update live_leaves: remove spent, add new
        if (num_spends > 0) {
            live_leaves.erase(live_leaves.begin(), live_leaves.begin() + num_spends);
        }
        live_leaves.insert(live_leaves.end(), additions.begin(), additions.end());

        // Verify CSN state matches forest state
        TEST_ASSERT(csn_commitment == forest.getCommitment(),
            "CSN commitment must match forest after block " + std::to_string(block));
        TEST_ASSERT(csn_num_leaves == forest.getNumLeaves(),
            "CSN numLeaves must match forest after block " + std::to_string(block));
    }

    // Final: CSN tracked 5 blocks with stump-only state
    // numLeaves is monotonically increasing: 5 blocks × 3 additions = 15
    TEST_ASSERT(csn_num_leaves == 15,
        "CSN should track correct leaf count (numLeaves is monotonic)");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "═══════════════════════════════════════════════════" << std::endl;
    std::cout << "  Utreexo Transition Proof Test Suite" << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;

    test_addition_only_transition();
    test_single_spend_transition();
    test_multi_spend_transition();
    test_tampered_commitment_rejection();
    test_tampered_addition_rejection();
    test_tampered_deletion_rejection();
    test_serialization_round_trip();
    test_empty_transition();
    test_forest_stump_proof_agreement();
    test_chained_csn_stump_verification();

    std::cout << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;
    std::cout << "  All " << tests_passed << "/" << tests_total << " assertions passed!" << std::endl;
    std::cout << "═══════════════════════════════════════════════════" << std::endl;

    return 0;
}
