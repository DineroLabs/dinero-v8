/**
 * Phase 2: ConsensusUTXOSet Fuzz Tests
 *
 * Tests the pure in-memory UTXO set under random operations.
 * No database setup needed - pure consensus testing.
 *
 * Test Properties:
 * 1. Add/Remove consistency - removing what was added
 * 2. Snapshot/Restore invariant - restore always returns to snapshot state
 * 3. Double-spend prevention - can't spend same UTXO twice
 * 4. UTXO count tracking - count matches operations
 *
 * This is the target for Phase 2: pure consensus that can be fuzzed trivially.
 */

#include "consensus/consensus_utxo_set.h"
#include "consensus/utxo_snapshot_state.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include "consensus/chainparams.h"
#include "primitives/uint256.h"
#include "primitives/hash_domains.h"  // For TxId
#include <iostream>
#include <vector>
#include <random>
#include <cassert>
#include <ctime>
#include <set>
#include <unordered_map>

using namespace dinero;
using namespace dinero::consensus;

// Random number generator
static std::mt19937 g_rng(static_cast<unsigned>(std::time(nullptr)));

// Configuration
static constexpr int NUM_FUZZ_ITERATIONS = 1000;
static constexpr int MAX_UTXOS_PER_ITERATION = 100;
static constexpr int SNAPSHOT_RESTORE_ITERATIONS = 50;

// Generate random OutPoint
OutPoint RandomOutPoint() {
    std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);

    uint256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(dist(g_rng) & 0xFF);
    }
    TxId txid(hash);

    uint32_t vout = static_cast<uint32_t>(dist(g_rng) % 256);
    return OutPoint(txid, vout);
}

// Generate random UTXOEntry
UTXOEntry RandomUTXOEntry(uint32_t height) {
    std::uniform_int_distribution<uint64_t> value_dist(1, 1000000000ULL);
    std::uniform_int_distribution<int> coinbase_dist(0, 10);

    UTXOEntry entry;
    entry.value = AmountUna::Una(value_dist(g_rng));
    entry.height = height;
    entry.isCoinbase = (coinbase_dist(g_rng) == 0);  // 10% chance of coinbase
    entry.scriptPubKey = {0x76, 0xa9, 0x14};  // P2PKH prefix

    // Add random pubkey hash
    for (int i = 0; i < 20; i++) {
        entry.scriptPubKey.push_back(static_cast<uint8_t>(g_rng() & 0xFF));
    }
    entry.scriptPubKey.push_back(0x88);  // OP_EQUALVERIFY
    entry.scriptPubKey.push_back(0xac);  // OP_CHECKSIG

    return entry;
}

// ============================================================================
// Test 1: Add/Remove Consistency
// ============================================================================
bool TestAddRemoveConsistency() {
    std::cout << "\n[Test 1] Add/Remove Consistency" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    ConsensusUTXOSet utxo_set;

    // Track what we've added
    std::vector<std::pair<OutPoint, UTXOEntry>> added_utxos;

    // Add random UTXOs
    std::uniform_int_distribution<int> count_dist(10, MAX_UTXOS_PER_ITERATION);
    int num_utxos = count_dist(g_rng);

    for (int i = 0; i < num_utxos; i++) {
        OutPoint outpoint = RandomOutPoint();
        UTXOEntry entry = RandomUTXOEntry(i + 1);

        if (utxo_set.AddCoin(outpoint, entry)) {
            added_utxos.push_back({outpoint, entry});
        }
    }

    std::cout << "  Added " << added_utxos.size() << " UTXOs" << std::endl;

    // Verify count
    if (utxo_set.GetSetSize() != added_utxos.size()) {
        std::cout << "  [FAIL] Count mismatch: expected " << added_utxos.size()
                  << ", got " << utxo_set.GetSetSize() << std::endl;
        return false;
    }

    // Verify all added UTXOs exist
    for (const auto& [outpoint, entry] : added_utxos) {
        if (!utxo_set.HaveCoin(outpoint)) {
            std::cout << "  [FAIL] Added UTXO not found" << std::endl;
            return false;
        }
    }

    // Spend all UTXOs
    for (const auto& [outpoint, entry] : added_utxos) {
        auto spent = utxo_set.SpendCoin(outpoint);
        if (!spent) {
            std::cout << "  [FAIL] Failed to spend added UTXO" << std::endl;
            return false;
        }
    }

    // Verify all spent
    if (utxo_set.GetSetSize() != 0) {
        std::cout << "  [FAIL] UTXOs remain after spending all" << std::endl;
        return false;
    }

    std::cout << "  [PASS] Add/Remove consistency verified" << std::endl;
    return true;
}

// ============================================================================
// Test 2: Snapshot/Restore Invariant
// ============================================================================
bool TestSnapshotRestoreInvariant() {
    std::cout << "\n[Test 2] Snapshot/Restore Invariant" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    ConsensusUTXOSet utxo_set;
    std::unordered_map<OutPoint, UTXOEntry> initial_utxos;

    // Build initial state through BulkLoad so snapshot/restore exercises the
    // backing forest, not just the UTXO map.
    while (initial_utxos.size() < 50) {
        initial_utxos.emplace(RandomOutPoint(), RandomUTXOEntry(static_cast<uint32_t>(initial_utxos.size() + 1)));
    }

    uint256 initial_best_block;
    initial_best_block.data[0] = 0x21;
    initial_best_block.data[31] = 0x12;
    if (!utxo_set.BulkLoad(initial_utxos, 50, initial_best_block)) {
        std::cout << "  [FAIL] BulkLoad failed while creating initial state" << std::endl;
        return false;
    }

    std::vector<OutPoint> initial_outpoints;
    initial_outpoints.reserve(initial_utxos.size());
    for (const auto& item : initial_utxos) {
        initial_outpoints.push_back(item.first);
    }

    size_t initial_count = utxo_set.GetSetSize();
    UtreexoHash initial_root = utxo_set.GetUtreexoRoot();
    uint64_t initial_leaves = utxo_set.GetForest().getNumLeaves();
    std::cout << "  Initial state: " << initial_count << " UTXOs" << std::endl;

    // Take snapshot
    UTXOSnapshot snapshot = utxo_set.Snapshot();

    // Verify snapshot captures forest state too.
    if (snapshot.GetUTXOCount() != initial_count) {
        std::cout << "  [FAIL] Snapshot count mismatch" << std::endl;
        return false;
    }
    if (snapshot.utreexo_root != initial_root ||
        snapshot.utreexo_num_leaves != initial_leaves ||
        snapshot.utreexo_forest_state.empty()) {
        std::cout << "  [FAIL] Snapshot did not capture expected Utreexo state" << std::endl;
        return false;
    }

    // Mutate state through BulkLoad so the in-memory forest definitely changes
    // before Restore() runs.
    std::cout << "  Mutating state..." << std::endl;
    std::unordered_map<OutPoint, UTXOEntry> mutated_utxos;
    while (mutated_utxos.size() < 30) {
        mutated_utxos.emplace(RandomOutPoint(), RandomUTXOEntry(static_cast<uint32_t>(100 + mutated_utxos.size())));
    }
    uint256 mutated_best_block;
    mutated_best_block.data[0] = 0x34;
    mutated_best_block.data[31] = 0x43;
    if (!utxo_set.BulkLoad(mutated_utxos, 999, mutated_best_block)) {
        std::cout << "  [FAIL] BulkLoad failed while mutating state" << std::endl;
        return false;
    }

    size_t mutated_count = utxo_set.GetSetSize();
    std::cout << "  After mutations: " << mutated_count << " UTXOs" << std::endl;

    // Restore snapshot
    utxo_set.Restore(snapshot);

    size_t restored_count = utxo_set.GetSetSize();
    std::cout << "  After restore: " << restored_count << " UTXOs" << std::endl;

    // Verify restoration
    if (restored_count != initial_count) {
        std::cout << "  [FAIL] Restore count mismatch: expected " << initial_count
                  << ", got " << restored_count << std::endl;
        return false;
    }
    if (utxo_set.GetUtreexoRoot() != initial_root) {
        std::cout << "  [FAIL] Restore root mismatch" << std::endl;
        return false;
    }
    if (utxo_set.GetForest().getNumLeaves() != initial_leaves) {
        std::cout << "  [FAIL] Restore leaf-count mismatch" << std::endl;
        return false;
    }

    // Verify all initial UTXOs are back
    for (const auto& outpoint : initial_outpoints) {
        if (!utxo_set.HaveCoin(outpoint)) {
            std::cout << "  [FAIL] Initial UTXO missing after restore" << std::endl;
            return false;
        }
    }

    std::cout << "  [PASS] Snapshot/Restore invariant verified" << std::endl;
    return true;
}

// ============================================================================
// Test 3: Double-Spend Prevention
// ============================================================================
bool TestDoubleSpendPrevention() {
    std::cout << "\n[Test 3] Double-Spend Prevention" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    ConsensusUTXOSet utxo_set;

    // Add a UTXO
    OutPoint outpoint = RandomOutPoint();
    UTXOEntry entry = RandomUTXOEntry(1);

    bool added = utxo_set.AddCoin(outpoint, entry);
    if (!added) {
        std::cout << "  [FAIL] Failed to add initial UTXO" << std::endl;
        return false;
    }

    // First spend should succeed
    auto first_spend = utxo_set.SpendCoin(outpoint);
    if (!first_spend) {
        std::cout << "  [FAIL] First spend failed" << std::endl;
        return false;
    }

    // Second spend should fail (UTXO already spent)
    auto second_spend = utxo_set.SpendCoin(outpoint);
    if (second_spend) {
        std::cout << "  [FAIL] Double spend succeeded!" << std::endl;
        return false;
    }

    std::cout << "  [PASS] Double-spend correctly prevented" << std::endl;
    return true;
}

// ============================================================================
// Test 4: Random Fuzz Operations
// ============================================================================
bool TestRandomFuzzOperations() {
    std::cout << "\n[Test 4] Random Fuzz Operations (" << NUM_FUZZ_ITERATIONS << " iterations)" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    ConsensusUTXOSet utxo_set;
    std::set<OutPoint> known_utxos;

    std::uniform_int_distribution<int> op_dist(0, 3);
    int add_count = 0, spend_count = 0, snapshot_count = 0, restore_count = 0;

    UTXOSnapshot latest_snapshot;
    bool has_snapshot = false;

    for (int i = 0; i < NUM_FUZZ_ITERATIONS; i++) {
        int op = op_dist(g_rng);

        switch (op) {
            case 0: {
                // Add UTXO
                OutPoint outpoint = RandomOutPoint();
                UTXOEntry entry = RandomUTXOEntry(i + 1);
                if (utxo_set.AddCoin(outpoint, entry)) {
                    known_utxos.insert(outpoint);
                    add_count++;
                }
                break;
            }
            case 1: {
                // Spend random known UTXO
                if (!known_utxos.empty()) {
                    auto it = known_utxos.begin();
                    std::uniform_int_distribution<size_t> idx_dist(0, known_utxos.size() - 1);
                    std::advance(it, idx_dist(g_rng));

                    if (utxo_set.SpendCoin(*it)) {
                        known_utxos.erase(it);
                        spend_count++;
                    }
                }
                break;
            }
            case 2: {
                // Take snapshot
                latest_snapshot = utxo_set.Snapshot();
                has_snapshot = true;
                snapshot_count++;
                break;
            }
            case 3: {
                // Restore snapshot (occasionally)
                if (has_snapshot && (i % 20 == 0)) {
                    utxo_set.Restore(latest_snapshot);
                    // Reset known_utxos based on snapshot
                    known_utxos.clear();
                    for (const auto& [outpoint, entry] : latest_snapshot.utxos) {
                        known_utxos.insert(outpoint);
                    }
                    restore_count++;
                }
                break;
            }
        }

        // Invariant check: known set matches UTXO count
        if (known_utxos.size() != utxo_set.GetSetSize()) {
            std::cout << "  [FAIL] Invariant violation at iteration " << i << std::endl;
            std::cout << "         Known: " << known_utxos.size()
                      << ", Actual: " << utxo_set.GetSetSize() << std::endl;
            return false;
        }
    }

    std::cout << "  Operations: add=" << add_count << ", spend=" << spend_count
              << ", snapshot=" << snapshot_count << ", restore=" << restore_count << std::endl;
    std::cout << "  Final state: " << utxo_set.GetSetSize() << " UTXOs" << std::endl;
    std::cout << "  [PASS] Random fuzz operations completed without invariant violations" << std::endl;

    return true;
}

// ============================================================================
// Test 5: Stress Test - Rapid Snapshot/Restore Cycles
// ============================================================================
bool TestRapidSnapshotRestoreCycles() {
    std::cout << "\n[Test 5] Rapid Snapshot/Restore Cycles (" << SNAPSHOT_RESTORE_ITERATIONS << " cycles)" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    ConsensusUTXOSet utxo_set;

    // Build initial state
    for (int i = 0; i < 100; i++) {
        OutPoint outpoint = RandomOutPoint();
        UTXOEntry entry = RandomUTXOEntry(i + 1);
        utxo_set.AddCoin(outpoint, entry);
    }

    size_t initial_count = utxo_set.GetSetSize();
    std::cout << "  Initial state: " << initial_count << " UTXOs" << std::endl;

    for (int cycle = 0; cycle < SNAPSHOT_RESTORE_ITERATIONS; cycle++) {
        // Take snapshot
        UTXOSnapshot snapshot = utxo_set.Snapshot();

        // Random mutations
        for (int m = 0; m < 10; m++) {
            OutPoint outpoint = RandomOutPoint();
            UTXOEntry entry = RandomUTXOEntry(1000 + cycle * 10 + m);
            utxo_set.AddCoin(outpoint, entry);
        }

        // Restore
        utxo_set.Restore(snapshot);

        // Verify
        if (utxo_set.GetSetSize() != snapshot.GetUTXOCount()) {
            std::cout << "  [FAIL] Cycle " << cycle << " restore mismatch" << std::endl;
            return false;
        }
    }

    std::cout << "  Final state: " << utxo_set.GetSetSize() << " UTXOs" << std::endl;
    std::cout << "  [PASS] Rapid snapshot/restore cycles completed" << std::endl;

    return true;
}

// ============================================================================
// Main
// ============================================================================
int main() {
    // ConsensusUTXOSet::Restore() queries the canonical-roots activation
    // flag, which calls dinero::GetActiveChain() — that throws if
    // SelectParams() has never been called. The throw propagates out of
    // main(), std::terminate() runs, and on MSVC abort() raises
    // __fastfail(FAST_FAIL_FATAL_APP_EXIT) (exit code 0xC0000409). Same
    // pattern as test_formal_invariants (commit a0a71ab9).
    dinero::SelectParams(dinero::Chain::MAINNET);

    std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Phase 2: ConsensusUTXOSet Fuzz Tests" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Pure in-memory UTXO set - no database required" << std::endl;
    std::cout << "Seed: " << static_cast<unsigned>(std::time(nullptr)) << std::endl;

    int passed = 0;
    int failed = 0;

    if (TestAddRemoveConsistency()) passed++; else failed++;
    if (TestSnapshotRestoreInvariant()) passed++; else failed++;
    if (TestDoubleSpendPrevention()) passed++; else failed++;
    if (TestRandomFuzzOperations()) passed++; else failed++;
    if (TestRapidSnapshotRestoreCycles()) passed++; else failed++;

    std::cout << "\n═══════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;

    if (failed > 0) {
        std::cout << "FAILED: Some tests did not pass" << std::endl;
        return 1;
    }

    std::cout << "SUCCESS: All ConsensusUTXOSet fuzz tests passed" << std::endl;
    return 0;
}
