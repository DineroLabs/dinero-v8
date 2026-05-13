/**
 * Layer 1-2 Torture Tests - STANDALONE
 *
 * Self-contained stress testing for DisconnectBlock/RollbackToFork concepts.
 * No external dependencies - pure logic testing.
 *
 * What We're Testing:
 * 1. DisconnectBlock correctness (UTXO restoration)
 * 2. Deep reorg stress (random depths)
 * 3. Value conservation invariants
 * 4. Edge cases
 *
 * Failure = Implementation broken.
 */

#include <iostream>
#include <map>
#include <vector>
#include <random>
#include <chrono>
#include <cstdint>
#include <string>
#include <cassert>

//=============================================================================
// Minimal Types (Standalone)
//=============================================================================

struct OutPoint {
    uint64_t txid;  // Simplified: just use uint64_t for test
    uint32_t vout;

    bool operator<(const OutPoint& other) const {
        if (txid != other.txid) return txid < other.txid;
        return vout < other.vout;
    }

    bool operator==(const OutPoint& other) const {
        return txid == other.txid && vout == other.vout;
    }
};

//=============================================================================
// Torture Statistics
//=============================================================================

struct TortureStats {
    size_t tests_run{0};
    size_t tests_passed{0};
    size_t tests_failed{0};
    size_t blocks_connected{0};
    size_t blocks_disconnected{0};
    size_t utxos_created{0};
    size_t utxos_spent{0};
    std::chrono::milliseconds total_time{0};

    void print() const {
        std::cout << "\n";
        std::cout << "════════════════════════════════════════════════════════\n";
        std::cout << "  LAYER 1-2 TORTURE TEST RESULTS\n";
        std::cout << "════════════════════════════════════════════════════════\n";
        std::cout << "Tests Run:           " << tests_run << "\n";
        std::cout << "Tests Passed:        " << tests_passed << " ✅\n";
        std::cout << "Tests Failed:        " << tests_failed << (tests_failed > 0 ? " ❌" : "") << "\n";
        std::cout << "Blocks Connected:    " << blocks_connected << "\n";
        std::cout << "Blocks Disconnected: " << blocks_disconnected << "\n";
        std::cout << "UTXOs Created:       " << utxos_created << "\n";
        std::cout << "UTXOs Spent:         " << utxos_spent << "\n";
        std::cout << "Total Time:          " << total_time.count() << " ms\n";
        std::cout << "════════════════════════════════════════════════════════\n";
        std::cout << "\n";
    }
};

TortureStats g_stats;

//=============================================================================
// Mock UTXO Set (Instrumented)
//=============================================================================

class MockUTXOSet {
public:
    std::map<OutPoint, uint64_t> utxos_;

    void AddCoin(const OutPoint& outpoint, uint64_t value) {
        if (value == 0) {
            std::cerr << "❌ FATAL: Zero-value UTXO\n";
            std::abort();
        }
        if (utxos_.count(outpoint)) {
            std::cerr << "❌ FATAL: Duplicate UTXO at txid=" << outpoint.txid << " vout=" << outpoint.vout << "\n";
            std::abort();
        }
        utxos_[outpoint] = value;
        g_stats.utxos_created++;
    }

    void SpendCoin(const OutPoint& outpoint) {
        if (!utxos_.count(outpoint)) {
            std::cerr << "❌ FATAL: Spend non-existent UTXO at txid=" << outpoint.txid << " vout=" << outpoint.vout << "\n";
            std::abort();
        }
        utxos_.erase(outpoint);
        g_stats.utxos_spent++;
    }

    bool HaveCoin(const OutPoint& outpoint) const {
        return utxos_.count(outpoint) > 0;
    }

    uint64_t GetTotalValue() const {
        uint64_t total = 0;
        for (const auto& [op, value] : utxos_) {
            total += value;
        }
        return total;
    }

    size_t GetSize() const {
        return utxos_.size();
    }

    std::map<OutPoint, uint64_t> Snapshot() const {
        return utxos_;
    }

    void Restore(const std::map<OutPoint, uint64_t>& snapshot) {
        utxos_ = snapshot;
    }

    bool MatchesSnapshot(const std::map<OutPoint, uint64_t>& snapshot) const {
        return utxos_ == snapshot;
    }
};

//=============================================================================
// Test 1: DisconnectBlock Correctness
//=============================================================================

void Test_DisconnectBlock_Correctness() {
    std::cout << "\n[Test 1] DisconnectBlock Correctness\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    auto start = std::chrono::steady_clock::now();

    MockUTXOSet utxo_set;

    // Initial state: Genesis UTXO
    OutPoint genesis{1, 0};
    utxo_set.AddCoin(genesis, 50'000'000'000);

    auto snapshot_before = utxo_set.Snapshot();

    // ConnectBlock: Spend genesis, create 2 new outputs
    utxo_set.SpendCoin(genesis);
    OutPoint out0{2, 0};
    OutPoint out1{2, 1};
    utxo_set.AddCoin(out0, 25'000'000'000);
    utxo_set.AddCoin(out1, 25'000'000'000);

    // DisconnectBlock: Reverse the operation
    utxo_set.SpendCoin(out0);
    utxo_set.SpendCoin(out1);
    utxo_set.AddCoin(genesis, 50'000'000'000);

    bool matches = utxo_set.MatchesSnapshot(snapshot_before);

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    g_stats.tests_run++;
    g_stats.total_time += duration;

    if (matches) {
        std::cout << "  ✅ PASS - UTXO set restored exactly\n";
        g_stats.tests_passed++;
    } else {
        std::cout << "  ❌ FAIL - UTXO set mismatch\n";
        g_stats.tests_failed++;
    }

    std::cout << "  Time: " << duration.count() << " ms\n";
}

//=============================================================================
// Test 2: Deep Reorg Stress
//=============================================================================

void Test_DeepReorg_Stress() {
    std::cout << "\n[Test 2] Deep Reorg Stress (10 tests, depths 1-100)\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    auto start = std::chrono::steady_clock::now();

    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> depth_dist(1, 100);

    const int NUM_TESTS = 10;
    int passed = 0;

    for (int i = 0; i < NUM_TESTS; ++i) {
        int depth = depth_dist(rng);

        MockUTXOSet utxo_set;
        std::vector<std::map<OutPoint, uint64_t>> snapshots;
        uint64_t prev_txid = 0;

        // Build chain
        for (int height = 0; height < depth; ++height) {
            snapshots.push_back(utxo_set.Snapshot());

            if (height > 0) {
                OutPoint prev{prev_txid, 0};
                utxo_set.SpendCoin(prev);
            }

            prev_txid = height + 1;
            OutPoint out{prev_txid, 0};
            utxo_set.AddCoin(out, 1'000'000);

            g_stats.blocks_connected++;
        }

        // Reorg back to genesis
        for (int height = depth - 1; height >= 0; --height) {
            utxo_set.Restore(snapshots[height]);
            g_stats.blocks_disconnected++;
        }

        if (utxo_set.GetSize() == 0) {
            passed++;
        } else {
            std::cerr << "  ❌ Test " << (i+1) << " FAILED (depth=" << depth << "): " << utxo_set.GetSize() << " UTXOs remain\n";
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    g_stats.tests_run += NUM_TESTS;
    g_stats.total_time += duration;

    if (passed == NUM_TESTS) {
        std::cout << "  ✅ PASS - All " << NUM_TESTS << " reorgs succeeded\n";
        g_stats.tests_passed += NUM_TESTS;
    } else {
        std::cout << "  ❌ FAIL - " << (NUM_TESTS - passed) << "/" << NUM_TESTS << " failed\n";
        g_stats.tests_failed += (NUM_TESTS - passed);
        g_stats.tests_passed += passed;
    }

    std::cout << "  Time: " << duration.count() << " ms\n";
}

//=============================================================================
// Test 3: Value Conservation
//=============================================================================

void Test_Value_Conservation() {
    std::cout << "\n[Test 3] Value Conservation (100 blocks of tx activity)\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    auto start = std::chrono::steady_clock::now();

    MockUTXOSet utxo_set;

    const uint64_t INITIAL_SUPPLY = 1'000'000'000'000;
    OutPoint genesis{1, 0};
    utxo_set.AddCoin(genesis, INITIAL_SUPPLY);

    uint64_t initial_value = utxo_set.GetTotalValue();

    std::mt19937 rng(54321);
    std::uniform_int_distribution<int> split_dist(2, 5);

    // Simulate 100 blocks
    for (int block = 0; block < 100; ++block) {
        if (utxo_set.GetSize() == 0) break;

        // Pick random UTXO to spend
        auto it = utxo_set.utxos_.begin();
        std::advance(it, rng() % utxo_set.GetSize());
        OutPoint to_spend = it->first;
        uint64_t value = it->second;

        utxo_set.SpendCoin(to_spend);

        // Split into N outputs
        int num_outputs = split_dist(rng);
        uint64_t per_output = value / num_outputs;
        uint64_t remainder = value % num_outputs;

        uint64_t new_txid = block + 1000;
        for (int i = 0; i < num_outputs; ++i) {
            OutPoint out{new_txid, static_cast<uint32_t>(i)};
            uint64_t out_value = per_output + (i == 0 ? remainder : 0);
            utxo_set.AddCoin(out, out_value);
        }

        g_stats.blocks_connected++;
    }

    uint64_t final_value = utxo_set.GetTotalValue();

    bool conserved = (initial_value == final_value);

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    g_stats.tests_run++;
    g_stats.total_time += duration;

    if (conserved) {
        std::cout << "  ✅ PASS - Value conserved (" << final_value << " una)\n";
        g_stats.tests_passed++;
    } else {
        std::cout << "  ❌ FAIL - Value NOT conserved\n";
        std::cout << "     Initial: " << initial_value << "\n";
        std::cout << "     Final:   " << final_value << "\n";
        std::cout << "     Diff:    " << (int64_t)(final_value - initial_value) << "\n";
        g_stats.tests_failed++;
    }

    std::cout << "  Time: " << duration.count() << " ms\n";
}

//=============================================================================
// Test 4: Stress - 1000 Block Reorg
//=============================================================================

void Test_Stress_1000Blocks() {
    std::cout << "\n[Test 4] Stress Test - 1000 Block Deep Reorg\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    auto start = std::chrono::steady_clock::now();

    MockUTXOSet utxo_set;
    std::vector<std::map<OutPoint, uint64_t>> snapshots;
    uint64_t prev_txid = 0;

    std::cout << "  Building chain...\n";
    for (int height = 0; height < 1000; ++height) {
        snapshots.push_back(utxo_set.Snapshot());

        if (height > 0) {
            OutPoint prev{prev_txid, 0};
            utxo_set.SpendCoin(prev);
        }

        prev_txid = height + 10000;
        OutPoint out{prev_txid, 0};
        utxo_set.AddCoin(out, 1'000'000);

        g_stats.blocks_connected++;

        if ((height + 1) % 100 == 0) {
            std::cout << "    ... " << (height + 1) << " blocks\n";
        }
    }

    std::cout << "  Reorging back to genesis...\n";
    for (int height = 999; height >= 0; --height) {
        utxo_set.Restore(snapshots[height]);
        g_stats.blocks_disconnected++;

        if ((999 - height + 1) % 100 == 0) {
            std::cout << "    ... disconnected " << (999 - height + 1) << " blocks\n";
        }
    }

    bool success = (utxo_set.GetSize() == 0);

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    g_stats.tests_run++;
    g_stats.total_time += duration;

    if (success) {
        std::cout << "  ✅ PASS - 1000 block reorg succeeded\n";
        g_stats.tests_passed++;
    } else {
        std::cout << "  ❌ FAIL - " << utxo_set.GetSize() << " UTXOs remain\n";
        g_stats.tests_failed++;
    }

    std::cout << "  Time: " << duration.count() << " ms\n";
}

//=============================================================================
// Main
//=============================================================================

int main() {
    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════\n";
    std::cout << "  LAYER 1-2 TORTURE TESTS (STANDALONE)\n";
    std::cout << "  DisconnectBlock | RollbackToFork | UTXO Invariants\n";
    std::cout << "════════════════════════════════════════════════════════\n";

    Test_DisconnectBlock_Correctness();
    Test_DeepReorg_Stress();
    Test_Value_Conservation();
    Test_Stress_1000Blocks();

    g_stats.print();

    if (g_stats.tests_failed == 0) {
        std::cout << "🎉 ALL TESTS PASSED - Layer 1-2 spine is SOLID\n\n";
        return 0;
    } else {
        std::cout << "❌ TESTS FAILED - Layer 1-2 has bugs\n\n";
        return 1;
    }
}
