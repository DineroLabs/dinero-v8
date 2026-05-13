// ============================================================================
// CONSENSUS INVARIANTS TEST SUITE
// ============================================================================
//
// Tests for forensic-grade consensus assertions.
//
// ============================================================================

#include "consensus/consensus_invariants.h"
#include "consensus/consensus_utxo_set.h"
#include "primitives/transaction.h"
#include "primitives/block.h"
#include "primitives/amount.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace dinero;
using namespace dinero::consensus;

// ============================================================================
// Test Utilities
// ============================================================================

static uint256 MakeHash(uint64_t seed) {
    uint256 hash;
    for (int i = 0; i < 4; i++) {
        reinterpret_cast<uint64_t*>(hash.data)[i] = seed + i * 0x123456789ABCDEFULL;
    }
    return hash;
}

static TxId MakeTxId(uint64_t seed) {
    uint256 hash = MakeHash(seed);
    return TxId(hash);
}

// Test counters
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, name) \
    do { \
        if (!(condition)) { \
            printf("  [FAIL] %s: %s\n", name, #condition); \
            tests_failed++; \
        } else { \
            printf("  [PASS] %s\n", name); \
            tests_passed++; \
        } \
    } while (0)

// ============================================================================
// Test 1: MaxSupplyAtHeight Correctness
// ============================================================================

void TestMaxSupplyAtHeight() {
    printf("\n[Test 1] MaxSupplyAtHeight Correctness\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    // Fair-launch schedule (v5/v7): no premine, PoW starts at height 1,
    // each PoW block emits INITIAL_SUBSIDY until the first halving.
    constexpr uint64_t UNA_PER_DIN = 100'000'000ULL;
    constexpr uint64_t INITIAL_SUBSIDY = 100ULL * UNA_PER_DIN;

    // Height 0: genesis (no coins)
    TEST_ASSERT(MaxSupplyAtHeight(0) == 0, "Height 0 = 0");

    // Height 1: one block at INITIAL_SUBSIDY
    TEST_ASSERT(MaxSupplyAtHeight(1) == INITIAL_SUBSIDY, "Height 1 = 1 * INITIAL_SUBSIDY");

    // Height 2: two blocks
    TEST_ASSERT(MaxSupplyAtHeight(2) == 2 * INITIAL_SUBSIDY, "Height 2 = 2 * INITIAL_SUBSIDY");

    // Height 100: 100 blocks (still in epoch 0, well below 1.314M halving)
    TEST_ASSERT(MaxSupplyAtHeight(100) == 100 * INITIAL_SUBSIDY, "Height 100 = 100 * INITIAL_SUBSIDY");

    // Supply is monotonically increasing
    uint64_t prev = 0;
    bool monotonic = true;
    for (uint32_t h = 0; h < 10000; h += 100) {
        uint64_t current = MaxSupplyAtHeight(h);
        if (current < prev) {
            monotonic = false;
            break;
        }
        prev = current;
    }
    TEST_ASSERT(monotonic, "Supply monotonically increasing");
}

// ============================================================================
// Test 2: ConsensusInvariantContext
// ============================================================================

void TestInvariantContext() {
    printf("\n[Test 2] ConsensusInvariantContext\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    auto& ctx = ConsensusInvariantContext::get();

    // Set values
    uint256 block_hash = MakeHash(12345);
    ctx.setBlock(block_hash, 1000);
    ctx.setOperation("TestOperation");

    TEST_ASSERT(ctx.current_block_hash == block_hash, "Block hash set");
    TEST_ASSERT(ctx.current_height == 1000, "Height set");
    TEST_ASSERT(strcmp(ctx.current_operation, "TestOperation") == 0, "Operation set");

    // Clear
    ctx.clear();
    TEST_ASSERT(ctx.current_height == 0, "Height cleared");
    TEST_ASSERT(ctx.current_operation == nullptr, "Operation cleared");
}

// ============================================================================
// Test 3: ConsensusOperationScope RAII
// ============================================================================

void TestOperationScope() {
    printf("\n[Test 3] ConsensusOperationScope RAII\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    auto& ctx = ConsensusInvariantContext::get();
    ctx.clear();

    TEST_ASSERT(ctx.current_operation == nullptr, "Initially null");

    {
        ConsensusOperationScope scope("OuterOperation");
        TEST_ASSERT(strcmp(ctx.current_operation, "OuterOperation") == 0, "Outer scope set");

        {
            ConsensusOperationScope inner("InnerOperation");
            TEST_ASSERT(strcmp(ctx.current_operation, "InnerOperation") == 0, "Inner scope set");
        }

        TEST_ASSERT(strcmp(ctx.current_operation, "OuterOperation") == 0, "Restored to outer");
    }

    TEST_ASSERT(ctx.current_operation == nullptr, "Restored to null after scopes");
}

// ============================================================================
// Test 4: BuildCurrentSnapshot
// ============================================================================

void TestBuildCurrentSnapshot() {
    printf("\n[Test 4] BuildCurrentSnapshot\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    auto& ctx = ConsensusInvariantContext::get();
    ctx.clear();

    // Set up context
    uint256 block_hash = MakeHash(99999);
    ctx.setBlock(block_hash, 500);
    ctx.setOperation("ApplyBlock");

    // Build snapshot
    auto snapshot = BuildCurrentSnapshot(__FILE__, __LINE__);

    TEST_ASSERT(snapshot.block_hash == block_hash, "Block hash captured");
    TEST_ASSERT(snapshot.height == 500, "Height captured");
    TEST_ASSERT(strcmp(snapshot.operation, "ApplyBlock") == 0, "Operation captured");
    TEST_ASSERT(snapshot.file != nullptr, "File captured");
    TEST_ASSERT(snapshot.line > 0, "Line captured");

    // Expected supply at height 500
    TEST_ASSERT(snapshot.expected_supply == MaxSupplyAtHeight(500), "Expected supply computed");

    ctx.clear();
}

// ============================================================================
// Test 5: VerifyAllInvariants - Valid State
// ============================================================================

void TestVerifyAllInvariantsValid() {
    printf("\n[Test 5] VerifyAllInvariants - Valid State\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    ConsensusUTXOSet utxo_set;

    // Add some valid UTXOs
    for (int i = 0; i < 10; i++) {
        OutPoint outpoint(MakeTxId(i), 0);
        UTXOEntry entry;
        entry.value = AmountUna::Una(100'000'000);  // 1 DIN each
        entry.height = 50 + i;
        entry.isCoinbase = false;
        utxo_set.AddCoin(outpoint, entry);
    }

    std::string error;
    bool valid = VerifyAllInvariants(utxo_set, 100, error);

    TEST_ASSERT(valid, "Valid state passes invariants");
    TEST_ASSERT(error.empty(), "No error message");
}

// ============================================================================
// Test 6: VerifyAllInvariants - Future Height
// ============================================================================

void TestVerifyAllInvariantsFutureHeight() {
    printf("\n[Test 6] VerifyAllInvariants - Future Height UTXO\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    ConsensusUTXOSet utxo_set;

    // Add UTXO from "future"
    OutPoint outpoint(MakeTxId(1), 0);
    UTXOEntry entry;
    entry.value = AmountUna::Una(100'000'000);
    entry.height = 200;  // Future height
    entry.isCoinbase = false;
    utxo_set.AddCoin(outpoint, entry);

    std::string error;
    bool valid = VerifyAllInvariants(utxo_set, 100, error);

    TEST_ASSERT(!valid, "Future height UTXO fails invariant");
    TEST_ASSERT(error.find("exceeds current height") != std::string::npos, "Correct error message");
}

// ============================================================================
// Test 7: VerifyQuickSanity
// ============================================================================

void TestVerifyQuickSanity() {
    printf("\n[Test 7] VerifyQuickSanity\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    ConsensusUTXOSet utxo_set;

    // Add some UTXOs
    for (int i = 0; i < 5; i++) {
        OutPoint outpoint(MakeTxId(i), 0);
        UTXOEntry entry;
        entry.value = AmountUna::Una(100'000'000);
        entry.height = 50;
        entry.isCoinbase = false;
        utxo_set.AddCoin(outpoint, entry);
    }

    TEST_ASSERT(VerifyQuickSanity(utxo_set, 100), "Normal state passes");
    TEST_ASSERT(!VerifyQuickSanity(utxo_set, 200'000'000), "Absurd height fails");
}

// ============================================================================
// Test 8: ConsensusStateSnapshot Dump Format
// ============================================================================

void TestSnapshotDump() {
    printf("\n[Test 8] ConsensusStateSnapshot Dump Format\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    ConsensusStateSnapshot snapshot;
    snapshot.block_hash = MakeHash(111);
    snapshot.height = 12345;
    snapshot.utreexo_root = MakeHash(222);
    snapshot.utxo_count = 999;
    snapshot.total_supply = 1'000'000'000'000ULL;  // 10,000 DIN
    snapshot.expected_supply = 2'000'000'000'000ULL;  // 20,000 DIN
    snapshot.operation = "TestDump";
    snapshot.file = "test.cpp";
    snapshot.line = 42;

    // Dump to /dev/null to verify no crash
    FILE* null_file = fopen("/dev/null", "w");
    if (null_file) {
        snapshot.dump(null_file);
        fclose(null_file);
        TEST_ASSERT(true, "Dump completes without crash");
    } else {
        // On systems without /dev/null, just test we can create the snapshot
        TEST_ASSERT(true, "Snapshot created successfully");
    }
}

// ============================================================================
// Test 9: Paranoid Mode Check
// ============================================================================

void TestParanoidMode() {
    printf("\n[Test 9] Paranoid Mode Runtime Check\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    // Just verify the function doesn't crash
    bool enabled = IsParanoidModeEnabled();
    printf("  Paranoid mode: %s\n", enabled ? "enabled" : "disabled");
    TEST_ASSERT(true, "IsParanoidModeEnabled() callable");
}

// ============================================================================
// Test 10: Supply Overflow Detection
// ============================================================================

void TestSupplyOverflow() {
    printf("\n[Test 10] Supply Overflow Detection\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    ConsensusUTXOSet utxo_set;

    // Add UTXOs that would exceed max supply at height 100. Fair-launch
    // schedule: no premine, height H == H * INITIAL_SUBSIDY in epoch 0.
    constexpr uint64_t UNA_PER_DIN = 100'000'000ULL;
    constexpr uint64_t INITIAL_SUBSIDY = 100ULL * UNA_PER_DIN;
    uint64_t max_at_100 = 100 * INITIAL_SUBSIDY;

    // Add one UTXO that exceeds max supply
    OutPoint outpoint(MakeTxId(1), 0);
    UTXOEntry entry;
    entry.value = AmountUna::Una(max_at_100 + 1);  // 1 una over max
    entry.height = 50;
    entry.isCoinbase = false;
    utxo_set.AddCoin(outpoint, entry);

    std::string error;
    bool valid = VerifyAllInvariants(utxo_set, 100, error);

    TEST_ASSERT(!valid, "Overflow detected");
    TEST_ASSERT(error.find("exceeds maximum") != std::string::npos, "Correct error message");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("════════════════════════════════════════════════════════════════\n");
    printf("Consensus Invariants Test Suite\n");
    printf("════════════════════════════════════════════════════════════════\n");
    printf("Forensic-grade correctness assertions\n");

    TestMaxSupplyAtHeight();
    TestInvariantContext();
    TestOperationScope();
    TestBuildCurrentSnapshot();
    TestVerifyAllInvariantsValid();
    TestVerifyAllInvariantsFutureHeight();
    TestVerifyQuickSanity();
    TestSnapshotDump();
    TestParanoidMode();
    TestSupplyOverflow();

    printf("\n════════════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("════════════════════════════════════════════════════════════════\n");

    if (tests_failed > 0) {
        printf("FAILURE: Some invariant tests failed\n");
        return 1;
    }

    printf("SUCCESS: All invariant tests passed\n");
    return 0;
}
