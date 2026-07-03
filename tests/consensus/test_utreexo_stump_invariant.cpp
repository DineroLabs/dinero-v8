/**
 * Utreexo Pollard/Stump Commitment Invariant Test
 *
 * CRITICAL INVARIANT:
 *   Pollard.getCommitment() == Stump.getCommitment()
 *
 * This test verifies that:
 * 1. Pollard (full forest) and Stump (roots-only) produce identical commitments
 * 2. Both handle additions identically
 * 3. Both handle empty/single/multiple root cases
 * 4. Stump extracted from Pollard matches the original
 *
 * If this test ever fails, there is a CONSENSUS BUG in either:
 * - UtreexoForest::getCommitment()
 * - UtreexoStump::getCommitment()
 * - UtreexoStump::fromForest()
 * - The add/merge algorithms
 */

#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_stump.h"
#include "primitives/uint256.h"
#include <iostream>
#include <sstream>
#include <cassert>
#include <iomanip>

using namespace dinero::consensus;
using dinero::uint256;

// Helper: Print hash as hex string
static std::string hashToHex(const UtreexoHash& hash) {
    std::ostringstream oss;
    for (uint8_t byte : hash) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

// Helper: Create dummy leaf hash
static UtreexoHash createDummyLeaf(int index) {
    std::ostringstream oss;
    oss << std::hex << std::setw(64) << std::setfill('0') << index;
    std::string txid_hex = oss.str();
    if (txid_hex.length() > 64) {
        txid_hex = txid_hex.substr(txid_hex.length() - 64);
    }
    uint256 txid = uint256::FromHexUnsafe(txid_hex);

    std::vector<uint8_t> scriptPubKey = {0x76, 0xa9, 0x14};
    for (int i = 0; i < 20; i++) {
        scriptPubKey.push_back(static_cast<uint8_t>(index + i));
    }
    scriptPubKey.push_back(0x88);
    scriptPubKey.push_back(0xac);

    return HashUTXOLegacy(txid, 0, 10000000000ULL, scriptPubKey);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: Empty State Invariant
// ═══════════════════════════════════════════════════════════════════════════
void test_empty_invariant() {
    std::cout << "\n[Test 1] Empty State Invariant\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest pollard;
    UtreexoStump stump;

    auto pollard_commitment = pollard.getCommitment();
    auto stump_commitment = stump.getCommitment();

    std::cout << "Pollard commitment: " << hashToHex(pollard_commitment) << "\n";
    std::cout << "Stump commitment:   " << hashToHex(stump_commitment) << "\n";

    assert(pollard_commitment == stump_commitment);
    std::cout << "✅ Empty state: Pollard == Stump\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: Single Leaf Invariant
// ═══════════════════════════════════════════════════════════════════════════
void test_single_leaf_invariant() {
    std::cout << "\n[Test 2] Single Leaf Invariant\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest pollard;
    UtreexoStump stump;

    auto leaf = createDummyLeaf(1);
    pollard.add(leaf);
    stump.addSingle(leaf);

    auto pollard_commitment = pollard.getCommitment();
    auto stump_commitment = stump.getCommitment();

    std::cout << "Pollard commitment: " << hashToHex(pollard_commitment) << "\n";
    std::cout << "Stump commitment:   " << hashToHex(stump_commitment) << "\n";
    std::cout << "Pollard leaves: " << pollard.getNumLeaves() << "\n";
    std::cout << "Stump leaves:   " << stump.getNumLeaves() << "\n";

    assert(pollard_commitment == stump_commitment);
    assert(pollard.getNumLeaves() == stump.getNumLeaves());
    std::cout << "✅ Single leaf: Pollard == Stump\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 3: Multi-Leaf Invariant (powers of 2)
// ═══════════════════════════════════════════════════════════════════════════
void test_multi_leaf_invariant() {
    std::cout << "\n[Test 3] Multi-Leaf Invariant\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    // Test various counts: 2, 3, 4, 5, 7, 8, 13, 16, 17, 32, 100
    std::vector<int> test_counts = {2, 3, 4, 5, 7, 8, 13, 16, 17, 32, 100};

    for (int count : test_counts) {
        UtreexoForest pollard;
        UtreexoStump stump;

        for (int i = 0; i < count; i++) {
            auto leaf = createDummyLeaf(i);
            pollard.add(leaf);
            stump.addSingle(leaf);
        }

        auto pollard_commitment = pollard.getCommitment();
        auto stump_commitment = stump.getCommitment();

        if (pollard_commitment != stump_commitment) {
            std::cerr << "❌ INVARIANT VIOLATION at count=" << count << "\n";
            std::cerr << "   Pollard: " << hashToHex(pollard_commitment) << "\n";
            std::cerr << "   Stump:   " << hashToHex(stump_commitment) << "\n";
            assert(false && "Pollard/Stump commitment invariant violated");
        }

        std::cout << "✅ " << count << " leaves: Pollard == Stump\n";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 4: fromForest() Extraction Invariant
// ═══════════════════════════════════════════════════════════════════════════
void test_from_forest_invariant() {
    std::cout << "\n[Test 4] fromForest() Extraction Invariant\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest pollard;

    // Add various leaves
    for (int i = 0; i < 42; i++) {
        pollard.add(createDummyLeaf(i));
    }

    // Extract stump from pollard
    UtreexoStump extracted = UtreexoStump::fromForest(pollard);

    auto pollard_commitment = pollard.getCommitment();
    auto extracted_commitment = extracted.getCommitment();

    std::cout << "Pollard commitment:   " << hashToHex(pollard_commitment) << "\n";
    std::cout << "Extracted commitment: " << hashToHex(extracted_commitment) << "\n";
    std::cout << "Pollard leaves:   " << pollard.getNumLeaves() << "\n";
    std::cout << "Extracted leaves: " << extracted.getNumLeaves() << "\n";

    assert(pollard_commitment == extracted_commitment);
    assert(pollard.getNumLeaves() == extracted.getNumLeaves());
    std::cout << "✅ fromForest(): Extracted Stump == Original Pollard\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 5: Parallel Add Sequence Invariant
// ═══════════════════════════════════════════════════════════════════════════
void test_parallel_add_invariant() {
    std::cout << "\n[Test 5] Parallel Add Sequence Invariant\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest pollard;
    UtreexoStump stump;

    // Add leaves one at a time, checking invariant after each
    for (int i = 0; i < 50; i++) {
        auto leaf = createDummyLeaf(i);
        pollard.add(leaf);
        stump.addSingle(leaf);

        auto pollard_commitment = pollard.getCommitment();
        auto stump_commitment = stump.getCommitment();

        if (pollard_commitment != stump_commitment) {
            std::cerr << "❌ INVARIANT VIOLATION after adding leaf " << i << "\n";
            std::cerr << "   Pollard: " << hashToHex(pollard_commitment) << "\n";
            std::cerr << "   Stump:   " << hashToHex(stump_commitment) << "\n";
            assert(false && "Pollard/Stump commitment invariant violated");
        }
    }

    std::cout << "✅ 50 sequential adds: Invariant held after each\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 6: Roots Consistency
// ═══════════════════════════════════════════════════════════════════════════
void test_roots_consistency() {
    std::cout << "\n[Test 6] Roots Consistency\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest pollard;

    // 13 = 8 + 4 + 1 = three trees
    for (int i = 0; i < 13; i++) {
        pollard.add(createDummyLeaf(i));
    }

    UtreexoStump stump = UtreexoStump::fromForest(pollard);

    auto pollard_roots = pollard.getRoots();
    auto stump_roots = stump.getRoots();

    std::cout << "Pollard roots: " << pollard_roots.size() << "\n";
    std::cout << "Stump roots:   " << stump_roots.size() << "\n";

    assert(pollard_roots.size() == stump_roots.size());

    for (size_t i = 0; i < pollard_roots.size(); i++) {
        if (pollard_roots[i] != stump_roots[i]) {
            std::cerr << "❌ Root mismatch at index " << i << "\n";
            assert(false && "Root mismatch");
        }
    }

    std::cout << "✅ All roots match\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 7: fromCommitment() Must Not Fabricate Roots
// ═══════════════════════════════════════════════════════════════════════════
void test_from_commitment_is_opaque() {
    std::cout << "\n[Test 7] fromCommitment() Opaque Construction\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    // One-leaf forest
    UtreexoForest one_leaf_forest;
    one_leaf_forest.add(createDummyLeaf(100));
    UtreexoStump one_leaf_stump = UtreexoStump::fromCommitment(
        one_leaf_forest.getCommitment(), one_leaf_forest.getNumLeaves());

    assert(one_leaf_stump.getNumLeaves() == one_leaf_forest.getNumLeaves());
    assert(one_leaf_stump.getNumRoots() == 0);
    std::cout << "✅ numLeaves=1: no synthetic roots were created\n";

    // Two-leaf forest
    UtreexoForest two_leaf_forest;
    two_leaf_forest.add(createDummyLeaf(200));
    two_leaf_forest.add(createDummyLeaf(201));
    UtreexoStump two_leaf_stump = UtreexoStump::fromCommitment(
        two_leaf_forest.getCommitment(), two_leaf_forest.getNumLeaves());

    assert(two_leaf_stump.getNumLeaves() == two_leaf_forest.getNumLeaves());
    assert(two_leaf_stump.getNumRoots() == 0);
    std::cout << "✅ numLeaves=2: remains opaque without roots\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════
int main() {
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "    UTREEXO POLLARD/STUMP COMMITMENT INVARIANT TEST\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "\n";
    std::cout << "INVARIANT: Pollard.getCommitment() == Stump.getCommitment()\n";
    std::cout << "\n";
    std::cout << "If any test fails, there is a CONSENSUS BUG.\n";
    std::cout << "\n";

    test_empty_invariant();
    test_single_leaf_invariant();
    test_multi_leaf_invariant();
    test_from_forest_invariant();
    test_parallel_add_invariant();
    test_roots_consistency();
    test_from_commitment_is_opaque();

    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "    ✅ ALL INVARIANT TESTS PASSED\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "\n";
    std::cout << "The commitment is frozen. Pollard and Stump are equivalent.\n";
    std::cout << "\n";

    return 0;
}
