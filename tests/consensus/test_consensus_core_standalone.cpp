// ============================================================================
// TEST: Consensus Core Library Standalone
// ============================================================================
//
// This test verifies that libdinero-consensus works independently
// without any database, networking, or threading dependencies.
//
// If this test compiles and runs, the consensus library is truly pure.
//
// ----------------------------------------------------------------------------
// WHY THERE IS NO assert() IN THIS FILE
// ----------------------------------------------------------------------------
// This test used to be written entirely in assert(). CI builds Release, and
// CMAKE_CXX_FLAGS_RELEASE is "-O3 -DNDEBUG", so assert(x) expanded to
// ((void)0) and the expression was never compiled. The test ran in CI,
// printed PASSED, and verified nothing.
//
// That was not theoretical. Under NDEBUG this file referenced
// ConsensusSubsidy::PREMINE_UNA -- a constant deleted when Dinero moved to a
// fair launch -- and still compiled clean, because the preprocessor discarded
// the reference before the compiler saw it. The test asserted a premine that
// the consensus rules had not paid for a long time. See issue #497.
//
// CHECK/CHECK_EQ/REQUIRE below are ordinary if-statements. They gate in every
// build configuration. Do not reintroduce assert() here.
// ============================================================================

#include "dinero_consensus.h"
#include <cstdint>
#include <cstring>
#include <iostream>

using namespace dinero;
using namespace dinero::consensus;

namespace {

int g_failures = 0;

// Non-fatal: records the failure and keeps going, so one run reports every
// broken invariant rather than only the first (mirrors gtest's EXPECT_*).
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::cerr << "\n  CHECK FAILED: " #cond "\n    at " << __FILE__  \
                      << ":" << __LINE__ << "\n";                            \
        }                                                                    \
    } while (false)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        const auto a_ = (actual);                                            \
        const auto e_ = (expected);                                          \
        if (!(a_ == e_)) {                                                   \
            ++g_failures;                                                    \
            std::cerr << "\n  CHECK_EQ FAILED: " #actual " == " #expected    \
                      << "\n    actual:   " << a_                            \
                      << "\n    expected: " << e_                            \
                      << "\n    at " << __FILE__ << ":" << __LINE__ << "\n"; \
        }                                                                    \
    } while (false)

// Fatal: for preconditions whose failure would make the following lines
// undefined (null dereference, etc). Mirrors gtest's ASSERT_*.
#define REQUIRE(cond)                                                        \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "\n  REQUIRE FAILED: " #cond "\n    at " << __FILE__\
                      << ":" << __LINE__ << "\n  aborting: later checks "    \
                         "would be undefined\n";                             \
            std::exit(1);                                                    \
        }                                                                    \
    } while (false)

// ============================================================================
// Test 1: Subsidy Calculation (pure math)
// ============================================================================
void test_subsidy() {
    std::cout << "Test: Subsidy calculation... ";

    // Genesis block (height 0) has no spendable subsidy: it is an OP_RETURN
    // burn, not a payable coinbase.
    auto subsidy0 = GetBlockSubsidy(0);
    CHECK(subsidy0.IsZero());

    // FAIR LAUNCH -- there is NO premine. Height 1 is the first ordinary PoW
    // block and pays the full initial subsidy (100 DIN), exactly like height 2.
    // GetBlockSubsidy computes halvings from (height - 1), so heights 1 and 2
    // are both in halving epoch 0.
    auto subsidy1 = GetBlockSubsidy(1);
    CHECK_EQ(subsidy1.GetUna(), ConsensusSubsidy::INITIAL_SUBSIDY);

    // Height 2+ is normal PoW emission (100 DIN = 10,000,000,000 una)
    auto subsidy2 = GetBlockSubsidy(2);
    CHECK_EQ(subsidy2.GetUna(), ConsensusSubsidy::INITIAL_SUBSIDY);

    // Heights 1 and 2 must agree: a premine would show up precisely as a
    // difference here.
    CHECK_EQ(subsidy1.GetUna(), subsidy2.GetUna());

    // The last block of epoch 0 still pays the full subsidy...
    auto subsidy_last_full = GetBlockSubsidy(ConsensusSubsidy::HALVING_INTERVAL);
    CHECK_EQ(subsidy_last_full.GetUna(), ConsensusSubsidy::INITIAL_SUBSIDY);

    // ...and the very next block is halved. Pinning both sides makes this an
    // exact boundary rather than a spot check.
    auto subsidy_first_halved =
        GetBlockSubsidy(ConsensusSubsidy::HALVING_INTERVAL + 1);
    CHECK_EQ(subsidy_first_halved.GetUna(), ConsensusSubsidy::INITIAL_SUBSIDY / 2);

    // After first halving, subsidy is 50 DIN
    auto subsidyHalf = GetBlockSubsidy(2 + ConsensusSubsidy::HALVING_INTERVAL);
    CHECK_EQ(subsidyHalf.GetUna(), ConsensusSubsidy::INITIAL_SUBSIDY / 2);

    // Emission never drops below the tail floor, however many halvings pass.
    auto subsidy_far = GetBlockSubsidy(ConsensusSubsidy::HALVING_INTERVAL * 40);
    CHECK(subsidy_far.GetUna() >= ConsensusSubsidy::TAIL_EMISSION_UNA);

    std::cout << (g_failures == 0 ? "PASSED\n" : "FAILED\n");
}

// ============================================================================
// Test 2: ConsensusUTXOSet (pure in-memory)
// ============================================================================
void test_utxo_set() {
    std::cout << "Test: ConsensusUTXOSet operations... ";

    ConsensusUTXOSet utxo_set;

    // Initial state
    CHECK_EQ(utxo_set.GetSetSize(), static_cast<size_t>(0));
    CHECK_EQ(utxo_set.GetHeight(), static_cast<uint32_t>(0));

    // Create a test outpoint
    uint256 hash;
    std::memset(hash.data, 0x42, 32);
    TxId txid(hash);
    OutPoint outpoint(txid, 0);

    // Add a UTXO
    UTXOEntry entry;
    entry.value = AmountUna::Una(1000000);
    entry.height = 1;
    entry.isCoinbase = true;

    bool added = utxo_set.AddCoin(outpoint, entry);
    CHECK(added);
    CHECK_EQ(utxo_set.GetSetSize(), static_cast<size_t>(1));
    CHECK(utxo_set.HaveCoin(outpoint));

    // Get the UTXO
    const UTXOEntry* got = utxo_set.GetCoin(outpoint);
    REQUIRE(got != nullptr);
    CHECK_EQ(got->value.GetUna(), static_cast<uint64_t>(1000000));
    CHECK(got->isCoinbase);

    // Spend the UTXO
    auto spent = utxo_set.SpendCoin(outpoint);
    REQUIRE(spent != nullptr);
    CHECK_EQ(spent->value.GetUna(), static_cast<uint64_t>(1000000));
    CHECK_EQ(utxo_set.GetSetSize(), static_cast<size_t>(0));
    CHECK(!utxo_set.HaveCoin(outpoint));

    std::cout << (g_failures == 0 ? "PASSED\n" : "FAILED\n");
}

// ============================================================================
// Test 3: Snapshot/Restore (trivial reorg)
// ============================================================================
void test_snapshot_restore() {
    std::cout << "Test: Snapshot/Restore... ";

    ConsensusUTXOSet utxo_set;

    // Add some UTXOs
    for (int i = 0; i < 10; i++) {
        uint256 hash;
        std::memset(hash.data, static_cast<uint8_t>(i), 32);
        TxId txid(hash);
        OutPoint outpoint(txid, 0);

        UTXOEntry entry;
        entry.value = AmountUna::Una(1000 * (i + 1));
        entry.height = static_cast<uint32_t>(i);
        entry.isCoinbase = (i == 0);

        // The return value was previously discarded, so a silent AddCoin
        // failure would have shown up only as a confusing size mismatch.
        CHECK(utxo_set.AddCoin(outpoint, entry));
    }

    CHECK_EQ(utxo_set.GetSetSize(), static_cast<size_t>(10));

    // Take snapshot
    UTXOSnapshot snapshot = CreateSnapshot(utxo_set);
    CHECK_EQ(snapshot.utxos.size(), static_cast<size_t>(10));

    // Modify state (simulate applying a block)
    uint256 new_hash;
    std::memset(new_hash.data, 0xFF, 32);
    TxId new_txid(new_hash);
    OutPoint new_outpoint(new_txid, 0);
    UTXOEntry new_entry;
    new_entry.value = AmountUna::Una(999999);
    CHECK(utxo_set.AddCoin(new_outpoint, new_entry));

    CHECK_EQ(utxo_set.GetSetSize(), static_cast<size_t>(11));

    // Restore from snapshot (simulate reorg rollback)
    RestoreSnapshot(utxo_set, snapshot);

    CHECK_EQ(utxo_set.GetSetSize(), static_cast<size_t>(10));
    CHECK(!utxo_set.HaveCoin(new_outpoint));

    std::cout << (g_failures == 0 ? "PASSED\n" : "FAILED\n");
}

// ============================================================================
// Test 4: Consensus Limits (pure constants)
// ============================================================================
void test_limits() {
    std::cout << "Test: Consensus limits... ";

    // Block limits
    CHECK_EQ(MAX_BLOCK_SIZE, 1000000);
    CHECK_EQ(MAX_BLOCK_WEIGHT, 4000000);
    CHECK_EQ(MAX_BLOCK_SIGOPS_COST, 80000);

    // Transaction limits
    CHECK_EQ(MAX_TX_SIZE, 100000);
    CHECK_EQ(MAX_TX_WEIGHT, 400000);

    // Script limits
    CHECK_EQ(MAX_SCRIPT_SIZE, 10000);
    CHECK_EQ(MAX_SCRIPT_OPCODES, 201);
    CHECK_EQ(MAX_SCRIPT_ELEMENT_SIZE, 520);
    CHECK_EQ(MAX_STACK_SIZE, 1000);

    // Validation functions
    CHECK(IsValidBlockSize(500000));
    CHECK(!IsValidBlockSize(2000000));
    CHECK(IsValidTxSize(50000));
    CHECK(!IsValidTxSize(200000));

    std::cout << (g_failures == 0 ? "PASSED\n" : "FAILED\n");
}

// ============================================================================
// Test 5: Max Supply Calculation
// ============================================================================
void test_max_supply() {
    std::cout << "Test: Max supply calculation... ";

    // At genesis, only the unspendable OP_RETURN burn exists.
    uint64_t supply0 = GetMaxSupplyAtHeight(0);
    CHECK_EQ(supply0, ConsensusSubsidy::GENESIS_UNSPENDABLE_UNA);

    // At height 1 the first PoW block is issued -- NOT a premine. Total supply
    // is the genesis burn plus one full 100 DIN subsidy.
    uint64_t supply1 = GetMaxSupplyAtHeight(1);
    CHECK_EQ(supply1, ConsensusSubsidy::GENESIS_UNSPENDABLE_UNA +
                          ConsensusSubsidy::INITIAL_SUBSIDY);

    // Each subsequent early block adds exactly one full subsidy. A reinstated
    // premine at height 1 would break this stride immediately.
    uint64_t supply2 = GetMaxSupplyAtHeight(2);
    CHECK_EQ(supply2 - supply1, ConsensusSubsidy::INITIAL_SUBSIDY);

    // Supply should be monotonically increasing
    uint64_t prev = 0;
    for (uint32_t h = 0; h < 100; h++) {
        uint64_t current = GetMaxSupplyAtHeight(h);
        CHECK(current >= prev);
        prev = current;
    }

    std::cout << (g_failures == 0 ? "PASSED\n" : "FAILED\n");
}

// ============================================================================
// Test 6: Library Version
// ============================================================================
void test_version() {
    std::cout << "Test: Library version... ";

    CHECK(CONSENSUS_LIB_VERSION != nullptr);
    CHECK(CONSENSUS_PROTOCOL_VERSION >= 1);

    std::cout << (g_failures == 0 ? "PASSED" : "FAILED")
              << " (v" << CONSENSUS_LIB_VERSION << ")\n";
}

}  // namespace

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════════════\n";
    std::cout << "CONSENSUS CORE LIBRARY - STANDALONE TEST\n";
    std::cout << "════════════════════════════════════════════════════════════════\n";
    std::cout << "\n";
    std::cout << "This test uses ONLY libdinero-consensus (pure library).\n";
    std::cout << "No database. No networking. No threading.\n";
    std::cout << "\n";

    test_subsidy();
    test_utxo_set();
    test_snapshot_restore();
    test_limits();
    test_max_supply();
    test_version();

    std::cout << "\n";
    std::cout << "════════════════════════════════════════════════════════════════\n";
    if (g_failures != 0) {
        std::cout << "FAILED - " << g_failures << " check(s) did not hold\n";
        std::cout << "════════════════════════════════════════════════════════════════\n\n";
        return 1;
    }
    std::cout << "ALL TESTS PASSED - Consensus library is standalone-capable\n";
    std::cout << "════════════════════════════════════════════════════════════════\n";
    std::cout << "\n";
    std::cout << "This library can now be used in:\n";
    std::cout << "  - Mobile wallets (iOS/Android)\n";
    std::cout << "  - Hardware wallets\n";
    std::cout << "  - Browsers (WASM)\n";
    std::cout << "  - Light clients\n";
    std::cout << "\n";

    return 0;
}
