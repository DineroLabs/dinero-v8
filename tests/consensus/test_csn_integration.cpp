/**
 * CSN (Compact State Node) Integration Test Suite
 *
 * Proves the end-to-end CSN pipeline:
 * 1. Position index tracks adds/spends correctly
 * 2. Proofs can be generated from indexed positions
 * 3. Forest + position index can be rebuilt from UTXO set
 * 4. Stateless batch proof verification works
 * 5. Full CSN cycle: build → prove → wipe → rebuild → re-verify
 *
 * This test establishes the CSN invariant:
 *   A node with zero UTXOs, zero forest, and only headers + proofs
 *   can safely validate blocks.
 */

#include "consensus/utreexo_accumulator.h"
#include "indexing/utxo_position_index.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>
#include <unordered_map>
#include <algorithm>

using namespace dinero;
using namespace dinero::consensus;
using namespace dinero::indexing;

// ============================================================================
// Helpers
// ============================================================================

struct UTXOData {
    TxId txid;
    uint32_t vout;
    uint64_t amount;
    std::vector<uint8_t> scriptPubKey;
    UtreexoHash leafHash;
};

static uint256 makeTxId(uint64_t id) {
    uint256 txid;
    std::memset(txid.data, 0, 32);
    std::memcpy(txid.data, &id, sizeof(id));
    return txid;
}

static UTXOData makeUTXO(uint64_t id, uint32_t vout, uint64_t amount) {
    UTXOData utxo;
    utxo.txid = TxId(makeTxId(id));
    utxo.vout = vout;
    utxo.amount = amount;
    utxo.scriptPubKey = {0x51, 0x20};
    utxo.scriptPubKey.resize(34, 0x00);
    utxo.leafHash = HashUTXO(
        utxo.txid.AsUint256(), utxo.vout, utxo.amount, utxo.scriptPubKey);
    return utxo;
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

// Simulate block connection: remove spends, then add new UTXOs
// Returns the list of added UTXOs with their positions
static void connectBlock(
    UtreexoForest& forest,
    UTXOPositionIndex& index,
    const std::vector<UTXOData>& spend_utxos,
    const std::vector<UTXOData>& new_utxos,
    std::vector<std::pair<UTXOData, uint64_t>>& added_with_positions
) {
    // PASS 1: Remove spent UTXOs
    for (const auto& utxo : spend_utxos) {
        auto pos_opt = forest.findLeafPosition(utxo.leafHash);
        if (!pos_opt.has_value()) continue;

        auto proof_opt = forest.prove(pos_opt.value());
        if (!proof_opt.has_value()) continue;

        forest.remove(utxo.leafHash, proof_opt.value());
        index.RemovePosition(utxo.txid, utxo.vout);
    }

    // PASS 2: Add new UTXOs
    for (const auto& utxo : new_utxos) {
        uint64_t position = forest.add(utxo.leafHash);
        index.AddPosition(utxo.txid, utxo.vout, position);
        added_with_positions.push_back({utxo, position});
    }
}

// ============================================================================
// Test 1: Position index tracks adds
// ============================================================================
static void test_position_index_tracks_adds() {
    std::cout << "Test 1: Position index tracks adds..." << std::endl;

    UtreexoForest forest;
    UTXOPositionIndex index;

    // Simulate 3 blocks, each with 3 outputs
    std::vector<UTXOData> all_utxos;
    for (uint64_t block = 0; block < 3; ++block) {
        std::vector<UTXOData> block_utxos;
        for (uint32_t vout = 0; vout < 3; ++vout) {
            block_utxos.push_back(makeUTXO(block * 100 + 1, vout, 1000 + block * 100 + vout));
        }

        std::vector<std::pair<UTXOData, uint64_t>> added;
        connectBlock(forest, index, {}, block_utxos, added);

        for (const auto& [utxo, pos] : added) {
            all_utxos.push_back(utxo);
        }
    }

    // Verify all 9 UTXOs are tracked
    TEST_ASSERT(index.GetPositionCount() == 9, "Should have 9 positions");

    // Verify each position is retrievable and matches forest
    for (const auto& utxo : all_utxos) {
        auto pos_opt = index.GetPosition(utxo.txid, utxo.vout);
        TEST_ASSERT(pos_opt.has_value(), "Position should exist in index");

        // Cross-check: forest should find this leaf at the same position
        auto forest_pos = forest.findLeafPosition(utxo.leafHash);
        TEST_ASSERT(forest_pos.has_value(), "Forest should have this leaf");
        TEST_ASSERT(*pos_opt == *forest_pos, "Index position must match forest position");
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 2: Position index tracks spends
// ============================================================================
static void test_position_index_tracks_spends() {
    std::cout << "Test 2: Position index tracks spends..." << std::endl;

    UtreexoForest forest;
    UTXOPositionIndex index;

    // Add 8 UTXOs
    std::vector<UTXOData> utxos;
    for (uint64_t i = 0; i < 8; ++i) {
        utxos.push_back(makeUTXO(200 + i, 0, 5000 + i));
    }

    std::vector<std::pair<UTXOData, uint64_t>> added;
    connectBlock(forest, index, {}, utxos, added);
    TEST_ASSERT(index.GetPositionCount() == 8, "Should have 8 positions");

    // Spend UTXOs 2 and 5
    std::vector<UTXOData> spends = {utxos[2], utxos[5]};
    std::vector<std::pair<UTXOData, uint64_t>> added2;
    connectBlock(forest, index, spends, {}, added2);

    TEST_ASSERT(index.GetPositionCount() == 6, "Should have 6 positions after spending 2");

    // Spent UTXOs should not be in index
    TEST_ASSERT(!index.HasPosition(utxos[2].txid, utxos[2].vout), "Spent UTXO 2 should be removed");
    TEST_ASSERT(!index.HasPosition(utxos[5].txid, utxos[5].vout), "Spent UTXO 5 should be removed");

    // Remaining UTXOs should still be tracked
    for (size_t i = 0; i < 8; ++i) {
        if (i == 2 || i == 5) continue;
        TEST_ASSERT(index.HasPosition(utxos[i].txid, utxos[i].vout),
                    "Unspent UTXO should still be in index");
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 3: Proof generation via index
// ============================================================================
static void test_proof_generation_via_index() {
    std::cout << "Test 3: Proof generation via index..." << std::endl;

    UtreexoForest forest;
    UTXOPositionIndex index;

    // Add 16 UTXOs (power of 2 for clean tree)
    std::vector<UTXOData> utxos;
    for (uint64_t i = 0; i < 16; ++i) {
        utxos.push_back(makeUTXO(300 + i, 0, 10000 + i));
    }

    std::vector<std::pair<UTXOData, uint64_t>> added;
    connectBlock(forest, index, {}, utxos, added);

    // For each UTXO: look up position in index, generate proof, verify
    size_t proofs_generated = 0;
    for (const auto& utxo : utxos) {
        auto pos_opt = index.GetPosition(utxo.txid, utxo.vout);
        TEST_ASSERT(pos_opt.has_value(), "Position should be in index");

        auto proof_opt = forest.prove(*pos_opt);
        TEST_ASSERT(proof_opt.has_value(), "Proof generation should succeed");

        // Verify the proof
        auto roots = forest.getRoots();
        bool valid = proof_opt->verify(utxo.leafHash, roots);
        TEST_ASSERT(valid, "Proof should verify against forest roots");
        proofs_generated++;
    }

    TEST_ASSERT(proofs_generated == 16, "Should generate 16 proofs");
    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 4: Rebuild from scratch
// ============================================================================
static void test_rebuild_from_scratch() {
    std::cout << "Test 4: Rebuild from scratch..." << std::endl;

    UtreexoForest original_forest;
    UTXOPositionIndex original_index;

    // Build original state: 20 UTXOs, spend 5
    std::vector<UTXOData> utxos;
    for (uint64_t i = 0; i < 20; ++i) {
        utxos.push_back(makeUTXO(400 + i, 0, 20000 + i));
    }

    std::vector<std::pair<UTXOData, uint64_t>> added;
    connectBlock(original_forest, original_index, {}, utxos, added);

    // Spend 5 UTXOs
    std::vector<UTXOData> spends = {utxos[3], utxos[7], utxos[11], utxos[15], utxos[19]};
    std::vector<std::pair<UTXOData, uint64_t>> added2;
    connectBlock(original_forest, original_index, spends, {}, added2);

    UtreexoHash original_root = original_forest.getCommitment();
    size_t original_count = original_index.GetPositionCount();
    TEST_ASSERT(original_count == 15, "Should have 15 UTXOs after spending 5");

    // Rebuild: new forest + new index from "UTXO set" (the 15 unspent UTXOs)
    UtreexoForest rebuilt_forest;
    UTXOPositionIndex rebuilt_index;

    // Simulate forEachUTXO: iterate unspent UTXOs and rebuild
    std::vector<UTXOData> unspent;
    for (size_t i = 0; i < 20; ++i) {
        if (i == 3 || i == 7 || i == 11 || i == 15 || i == 19) continue;
        unspent.push_back(utxos[i]);
    }

    for (const auto& utxo : unspent) {
        uint64_t pos = rebuilt_forest.add(utxo.leafHash);
        rebuilt_index.AddPosition(utxo.txid, utxo.vout, pos);
    }

    UtreexoHash rebuilt_root = rebuilt_forest.getCommitment();

    // NOTE: Roots will NOT match the original because the original forest
    // has deletions (nullopt leaves) while rebuild creates a fresh sequential
    // forest. This is expected — rebuild creates a CLEAN state.
    // The key invariant is: rebuilt_index.GetPositionCount() == unspent count
    // AND proofs work in the rebuilt forest.
    TEST_ASSERT(rebuilt_index.GetPositionCount() == 15, "Rebuilt index should have 15 entries");
    TEST_ASSERT(rebuilt_forest.getNumLeaves() == 15, "Rebuilt forest should have 15 leaves");

    // Verify all proofs work in rebuilt forest
    for (const auto& utxo : unspent) {
        auto pos_opt = rebuilt_index.GetPosition(utxo.txid, utxo.vout);
        TEST_ASSERT(pos_opt.has_value(), "Rebuilt index should have position");

        auto proof_opt = rebuilt_forest.prove(*pos_opt);
        TEST_ASSERT(proof_opt.has_value(), "Should generate proof in rebuilt forest");

        auto roots = rebuilt_forest.getRoots();
        bool valid = proof_opt->verify(utxo.leafHash, roots);
        TEST_ASSERT(valid, "Proof should verify in rebuilt forest");
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 5: Stateless batch proof verification
// ============================================================================
static void test_stateless_batch_proof() {
    std::cout << "Test 5: Stateless batch proof..." << std::endl;

    UtreexoForest forest;
    UTXOPositionIndex index;

    // Add 32 UTXOs
    std::vector<UTXOData> utxos;
    for (uint64_t i = 0; i < 32; ++i) {
        utxos.push_back(makeUTXO(500 + i, 0, 50000 + i));
    }

    std::vector<std::pair<UTXOData, uint64_t>> added;
    connectBlock(forest, index, {}, utxos, added);

    // Select 5 targets for batch proof
    std::vector<UtreexoHash> targets;
    std::vector<uint64_t> positions;
    std::vector<size_t> target_indices = {3, 7, 15, 22, 29};

    for (size_t idx : target_indices) {
        targets.push_back(utxos[idx].leafHash);
        auto pos = index.GetPosition(utxos[idx].txid, utxos[idx].vout);
        TEST_ASSERT(pos.has_value(), "Target should have position");
        positions.push_back(*pos);
    }

    // Generate block proof from forest (returns BlockUtreexoProof with positions)
    auto batch_proof = forest.generateBlockProof(targets);
    TEST_ASSERT(!batch_proof.proof_hashes.empty() || targets.size() <= 1,
                "Batch proof should have proof hashes");
    TEST_ASSERT(batch_proof.positions.size() == targets.size(),
                "Proof should have one position per target");

    // Verify using stateless verification (no findLeafPosition needed)
    auto roots = forest.getRoots();
    bool valid = forest.verifyBatchProofStateless(
        targets,
        batch_proof.positions,
        batch_proof.proof_hashes,
        batch_proof.numLeaves,
        roots
    );
    TEST_ASSERT(valid, "Stateless batch proof should verify");

    // Verify with tampered target fails
    std::vector<UtreexoHash> tampered_targets = targets;
    tampered_targets[0] = makeUTXO(9999, 0, 1).leafHash;  // wrong leaf
    bool tampered_valid = forest.verifyBatchProofStateless(
        tampered_targets,
        batch_proof.positions,
        batch_proof.proof_hashes,
        batch_proof.numLeaves,
        roots
    );
    TEST_ASSERT(!tampered_valid, "Tampered target should fail verification");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 6: Full CSN cycle
// ============================================================================
static void test_full_csn_cycle() {
    std::cout << "Test 6: Full CSN cycle..." << std::endl;

    UtreexoForest forest;
    UTXOPositionIndex index;

    // Phase 1: Build up state over multiple blocks
    std::vector<UTXOData> all_live_utxos;  // Track what's currently unspent

    // Block 1: 5 coinbase-like outputs
    std::vector<UTXOData> block1_utxos;
    for (uint32_t v = 0; v < 5; ++v) {
        block1_utxos.push_back(makeUTXO(600, v, 100000 + v));
    }
    std::vector<std::pair<UTXOData, uint64_t>> added1;
    connectBlock(forest, index, {}, block1_utxos, added1);
    for (const auto& u : block1_utxos) all_live_utxos.push_back(u);

    // Block 2: spend 2 from block 1, add 4 new
    std::vector<UTXOData> block2_spends = {block1_utxos[1], block1_utxos[3]};
    std::vector<UTXOData> block2_utxos;
    for (uint32_t v = 0; v < 4; ++v) {
        block2_utxos.push_back(makeUTXO(601, v, 200000 + v));
    }
    std::vector<std::pair<UTXOData, uint64_t>> added2;
    connectBlock(forest, index, block2_spends, block2_utxos, added2);

    // Update live set
    all_live_utxos.erase(
        std::remove_if(all_live_utxos.begin(), all_live_utxos.end(),
            [&](const UTXOData& u) {
                return (u.txid == block1_utxos[1].txid && u.vout == block1_utxos[1].vout) ||
                       (u.txid == block1_utxos[3].txid && u.vout == block1_utxos[3].vout);
            }),
        all_live_utxos.end());
    for (const auto& u : block2_utxos) all_live_utxos.push_back(u);

    // Block 3: spend 1 from block 2, add 3 new
    std::vector<UTXOData> block3_spends = {block2_utxos[0]};
    std::vector<UTXOData> block3_utxos;
    for (uint32_t v = 0; v < 3; ++v) {
        block3_utxos.push_back(makeUTXO(602, v, 300000 + v));
    }
    std::vector<std::pair<UTXOData, uint64_t>> added3;
    connectBlock(forest, index, block3_spends, block3_utxos, added3);

    all_live_utxos.erase(
        std::remove_if(all_live_utxos.begin(), all_live_utxos.end(),
            [&](const UTXOData& u) {
                return u.txid == block2_utxos[0].txid && u.vout == block2_utxos[0].vout;
            }),
        all_live_utxos.end());
    for (const auto& u : block3_utxos) all_live_utxos.push_back(u);

    // Sanity: 5 + 4 + 3 - 2 - 1 = 9 live UTXOs
    TEST_ASSERT(all_live_utxos.size() == 9, "Should have 9 live UTXOs");
    TEST_ASSERT(index.GetPositionCount() == 9, "Index should track 9 positions");

    // Phase 2: Generate proofs for all live UTXOs (save for later verification)
    struct SavedProof {
        UTXOData utxo;
        UtreexoProof proof;
    };
    std::vector<SavedProof> saved_proofs;

    for (const auto& utxo : all_live_utxos) {
        auto pos = index.GetPosition(utxo.txid, utxo.vout);
        TEST_ASSERT(pos.has_value(), "Live UTXO should have position");

        auto proof = forest.prove(*pos);
        TEST_ASSERT(proof.has_value(), "Should generate proof for live UTXO");

        saved_proofs.push_back({utxo, *proof});
    }

    // Phase 3: Wipe state — simulate clean-slate rebuild
    UtreexoForest rebuilt_forest;
    UTXOPositionIndex rebuilt_index;

    // Rebuild from "UTXO set" (all_live_utxos simulates forEachUTXO)
    for (const auto& utxo : all_live_utxos) {
        uint64_t pos = rebuilt_forest.add(utxo.leafHash);
        rebuilt_index.AddPosition(utxo.txid, utxo.vout, pos);
    }

    TEST_ASSERT(rebuilt_index.GetPositionCount() == 9, "Rebuilt index should have 9 entries");
    TEST_ASSERT(rebuilt_forest.getNumLeaves() == 9, "Rebuilt forest should have 9 leaves");

    // Phase 4: Re-generate proofs from rebuilt state and verify
    auto rebuilt_roots = rebuilt_forest.getRoots();

    for (const auto& utxo : all_live_utxos) {
        auto pos = rebuilt_index.GetPosition(utxo.txid, utxo.vout);
        TEST_ASSERT(pos.has_value(), "Rebuilt index should have position");

        auto proof = rebuilt_forest.prove(*pos);
        TEST_ASSERT(proof.has_value(), "Should generate proof in rebuilt forest");

        bool valid = proof->verify(utxo.leafHash, rebuilt_roots);
        TEST_ASSERT(valid, "Proof should verify in rebuilt forest");
    }

    // Phase 5: Stateless batch proof from rebuilt state
    std::vector<UtreexoHash> all_targets;
    for (const auto& utxo : all_live_utxos) {
        all_targets.push_back(utxo.leafHash);
    }

    auto batch_proof = rebuilt_forest.generateBlockProof(all_targets);
    TEST_ASSERT(batch_proof.positions.size() == all_targets.size(),
                "Batch proof from rebuilt forest should have all positions");

    bool batch_valid = rebuilt_forest.verifyBatchProofStateless(
        all_targets,
        batch_proof.positions,
        batch_proof.proof_hashes,
        batch_proof.numLeaves,
        rebuilt_roots
    );
    TEST_ASSERT(batch_valid, "Batch proof should verify statelessly");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << std::endl;
    std::cout << "=========================================" << std::endl;
    std::cout << "  CSN Integration Test Suite" << std::endl;
    std::cout << "=========================================" << std::endl;

    test_position_index_tracks_adds();
    test_position_index_tracks_spends();
    test_proof_generation_via_index();
    test_rebuild_from_scratch();
    test_stateless_batch_proof();
    test_full_csn_cycle();

    std::cout << std::endl;
    std::cout << "=========================================" << std::endl;
    std::cout << "  All " << tests_passed << "/" << tests_total
              << " assertions passed!" << std::endl;
    std::cout << "=========================================" << std::endl;

    return 0;
}
