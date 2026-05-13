/**
 * Phase 2.1: Formal Invariants Test Harness
 *
 * Runs all formal invariants against ConsensusUTXOSet.
 * These tests can be run in CI to verify consensus correctness.
 *
 * This is the first step toward formal verification.
 */

#include "consensus/formal_invariants.h"
#include "consensus/consensus_utxo_set.h"
#include "consensus/utxo_snapshot_state.h"
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include "consensus/chainparams.h"
#include "primitives/uint256.h"
#include "primitives/hash_domains.h"
#include "primitives/amount.h"
#include <iostream>
#include <random>
#include <ctime>
#include <unordered_map>

using namespace dinero;
using namespace dinero::consensus;

// Random number generator
static std::mt19937 g_rng(static_cast<unsigned>(std::time(nullptr)));

// Generate random OutPoint
OutPoint RandomOutPoint() {
    std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
    uint256 hash;
    for (int i = 0; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>(dist(g_rng) & 0xFF);
    }
    return OutPoint(TxId(hash), static_cast<uint32_t>(dist(g_rng) % 256));
}

// Generate random UTXOEntry
UTXOEntry RandomUTXOEntry(uint32_t height, bool is_coinbase = false) {
    std::uniform_int_distribution<uint64_t> value_dist(1, 1000000000ULL);

    UTXOEntry entry;
    entry.value = AmountUna::Una(value_dist(g_rng));
    entry.height = height;
    entry.isCoinbase = is_coinbase;
    entry.scriptPubKey = {0x00, 0x14};  // P2WPKH prefix
    for (int i = 0; i < 20; i++) {
        entry.scriptPubKey.push_back(static_cast<uint8_t>(g_rng() & 0xFF));
    }
    return entry;
}

// =============================================================================
// Test: I1 - No Negative Balances
// =============================================================================
bool TestI1_NoNegativeBalances() {
    std::cout << "\n[I1] No Negative Balances" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    ConsensusUTXOSet utxo_set;

    // Add some UTXOs
    for (int i = 0; i < 100; i++) {
        utxo_set.AddCoin(RandomOutPoint(), RandomUTXOEntry(i + 1));
    }

    auto result = FormalInvariants::I1_NoNegativeBalances(utxo_set);

    if (result.passed) {
        std::cout << "  [PASS] " << result.invariant_name << std::endl;
        return true;
    } else {
        std::cout << "  [FAIL] " << result.message << std::endl;
        return false;
    }
}

// =============================================================================
// Test: I2 - Total Supply Bounded
// =============================================================================
bool TestI2_TotalSupplyBounded() {
    std::cout << "\n[I2] Total Supply Bounded" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    ConsensusUTXOSet utxo_set;

    // Add UTXOs with reasonable values
    for (int i = 0; i < 100; i++) {
        UTXOEntry entry;
        entry.value = AmountUna::Una(100000000ULL);  // 1 DIN each
        entry.height = i + 1;
        entry.scriptPubKey = {0x00, 0x14};
        utxo_set.AddCoin(RandomOutPoint(), entry);
    }

    auto result = FormalInvariants::I2_TotalSupplyBounded(utxo_set);

    if (result.passed) {
        std::cout << "  [PASS] " << result.invariant_name << std::endl;
        return true;
    } else {
        std::cout << "  [FAIL] " << result.message << std::endl;
        return false;
    }
}

// =============================================================================
// Test: I3 - No Double Spend
// =============================================================================
bool TestI3_NoDoubleSpend() {
    std::cout << "\n[I3] No Double Spend" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    ConsensusUTXOSet utxo_set;

    // Add a UTXO
    OutPoint outpoint = RandomOutPoint();
    utxo_set.AddCoin(outpoint, RandomUTXOEntry(1));

    // Check before spending
    auto result1 = FormalInvariants::I3_NoDoubleSpend(utxo_set, outpoint);
    if (!result1.passed) {
        std::cout << "  [FAIL] Pre-spend check failed" << std::endl;
        return false;
    }

    // Spend it
    utxo_set.SpendCoin(outpoint);

    // Check after spending (should fail now)
    auto result2 = FormalInvariants::I3_NoDoubleSpend(utxo_set, outpoint);
    if (result2.passed) {
        std::cout << "  [FAIL] Post-spend check should have failed" << std::endl;
        return false;
    }

    std::cout << "  [PASS] I3_NoDoubleSpend (correctly prevents double-spend)" << std::endl;
    return true;
}

// =============================================================================
// Test: I4 - Snapshot/Restore Identity
// =============================================================================
bool TestI4_SnapshotRestoreIdentity() {
    std::cout << "\n[I4] Snapshot/Restore Identity" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    ConsensusUTXOSet utxo_set;
    std::unordered_map<OutPoint, UTXOEntry> bulk_utxos;

    // Build state through BulkLoad so the Utreexo forest is populated too.
    while (bulk_utxos.size() < 50) {
        bulk_utxos.emplace(RandomOutPoint(), RandomUTXOEntry(static_cast<uint32_t>(bulk_utxos.size() + 1)));
    }

    uint256 best_block;
    best_block.data[0] = 0x11;
    best_block.data[31] = 0x99;
    if (!utxo_set.BulkLoad(bulk_utxos, 50, best_block)) {
        std::cout << "  [FAIL] BulkLoad failed while building forest-backed state" << std::endl;
        return false;
    }

    if (utxo_set.GetForest().getNumLeaves() != bulk_utxos.size()) {
        std::cout << "  [FAIL] Forest leaf count mismatch before invariant check" << std::endl;
        return false;
    }

    auto result = FormalInvariants::I4_SnapshotRestoreIdentity(utxo_set);

    if (result.passed) {
        std::cout << "  [PASS] " << result.invariant_name << std::endl;
        return true;
    } else {
        std::cout << "  [FAIL] " << result.message << std::endl;
        return false;
    }
}

// =============================================================================
// Test: I7 - Coinbase Maturity
// =============================================================================
bool TestI7_CoinbaseMaturity() {
    std::cout << "\n[I7] Coinbase Maturity" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    // Create coinbase UTXO at height 10
    UTXOEntry coinbase = RandomUTXOEntry(10, true);

    // Should NOT be spendable at height 50 (need 110)
    auto result1 = FormalInvariants::I7_CoinbaseMaturity(coinbase, 50);
    if (result1.passed) {
        std::cout << "  [FAIL] Coinbase should not be mature at height 50" << std::endl;
        return false;
    }

    // Should be spendable at height 110
    auto result2 = FormalInvariants::I7_CoinbaseMaturity(coinbase, 110);
    if (!result2.passed) {
        std::cout << "  [FAIL] Coinbase should be mature at height 110" << std::endl;
        return false;
    }

    std::cout << "  [PASS] I7_CoinbaseMaturity (enforces 100 block maturity)" << std::endl;
    return true;
}

// =============================================================================
// Test: I8 - UTXO Height Monotonicity
// =============================================================================
bool TestI8_UTXOHeightMonotonicity() {
    std::cout << "\n[I8] UTXO Height Monotonicity" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    ConsensusUTXOSet utxo_set;

    // Add UTXOs at various heights
    for (int i = 0; i < 50; i++) {
        utxo_set.AddCoin(RandomOutPoint(), RandomUTXOEntry(i + 1));
    }

    // Set chain height to 50
    utxo_set.SetBestBlock(uint256(), 50);

    auto result = FormalInvariants::I8_UTXOHeightMonotonicity(utxo_set);

    if (result.passed) {
        std::cout << "  [PASS] " << result.invariant_name << std::endl;
        return true;
    } else {
        std::cout << "  [FAIL] " << result.message << std::endl;
        return false;
    }
}

// =============================================================================
// Test: Run All Invariants
// =============================================================================
bool TestCheckAll() {
    std::cout << "\n[CheckAll] Run All Invariants" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;

    ConsensusUTXOSet utxo_set;

    // Build valid state
    for (int i = 0; i < 100; i++) {
        utxo_set.AddCoin(RandomOutPoint(), RandomUTXOEntry(i + 1));
    }
    utxo_set.SetBestBlock(uint256(), 100);

    auto results = FormalInvariants::CheckAll(utxo_set);

    if (FormalInvariants::AllPassed(results)) {
        std::cout << "  [PASS] All " << results.size() << " invariants passed" << std::endl;
        return true;
    } else {
        std::cout << "  [FAIL] " << FormalInvariants::GetFailureMessages(results) << std::endl;
        return false;
    }
}

// =============================================================================
// Main
// =============================================================================
int main() {
    // ConsensusUTXOSet::Restore() queries the canonical-roots activation
    // flag, which calls dinero::GetActiveChain() — that throws if
    // SelectParams() has never been called. The throw propagates out of
    // main(), std::terminate() runs, and on MSVC abort() raises
    // __fastfail(FAST_FAIL_FATAL_APP_EXIT) (exit code 0xC0000409).
    // I4_SnapshotRestoreIdentity is the first test to exercise Restore,
    // so the crash always lands there. Wire up chain selection here so
    // the activation hook has a consistent answer to give.
    dinero::SelectParams(dinero::Chain::MAINNET);

    std::cout << "════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Phase 2.1: Formal Invariants Test Harness" << std::endl;
    std::cout << "════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Verifying consensus invariants - no database required" << std::endl;
    std::cout << "Seed: " << static_cast<unsigned>(std::time(nullptr)) << std::endl;

    int passed = 0;
    int failed = 0;

    if (TestI1_NoNegativeBalances()) passed++; else failed++;
    if (TestI2_TotalSupplyBounded()) passed++; else failed++;
    if (TestI3_NoDoubleSpend()) passed++; else failed++;
    if (TestI4_SnapshotRestoreIdentity()) passed++; else failed++;
    if (TestI7_CoinbaseMaturity()) passed++; else failed++;
    if (TestI8_UTXOHeightMonotonicity()) passed++; else failed++;
    if (TestCheckAll()) passed++; else failed++;

    std::cout << "\n════════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
    std::cout << "════════════════════════════════════════════════════════════════" << std::endl;

    if (failed > 0) {
        std::cout << "FAILED: Some invariants did not pass" << std::endl;
        return 1;
    }

    std::cout << "SUCCESS: All formal invariants verified" << std::endl;
    return 0;
}
