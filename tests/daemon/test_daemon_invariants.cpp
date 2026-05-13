// Daemon Invariants Tests - Priority 5
// Following methodology: define invariants, write tests first, then fix code
//
// Invariants tested:
//   D1: Tip height must be non-negative
//   D2: Fork point must be valid (not null, height >= 0)
//   D3: Prev block hash must match expected parent
//   D4: Block timestamp must not be > 2 hours in future
//   D5: UTXO count must be non-negative
//   D6: Tip height monotonically increases (except during reorg)
//
// Build: cmake --build build --target test_daemon_invariants
// Run:   ./build/tests/daemon/test_daemon_invariants

#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <cstdint>
#include <ctime>
#include <sstream>
#include <optional>
#include <functional>

// Test framework
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;
static int tests_skipped = 0;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    tests_run++; \
    std::cout << "  Running " << #name << "... "; \
    try { \
        test_##name(); \
        tests_passed++; \
        std::cout << "PASSED" << std::endl; \
    } catch (const std::exception& e) { \
        tests_failed++; \
        std::cout << "FAILED: " << e.what() << std::endl; \
    } \
} while(0)

#define SKIP_TEST(name, reason) do { \
    tests_run++; \
    tests_skipped++; \
    std::cout << "  Skipping " << #name << ": " << reason << std::endl; \
} while(0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond); \
} while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::ostringstream oss; \
        oss << "Expected " << (a) << " == " << (b); \
        throw std::runtime_error(oss.str()); \
    } \
} while(0)

// ============================================================================
// Mock Types for Testing Daemon Invariants
// ============================================================================

struct MockBlockHash {
    std::string hex;

    MockBlockHash() : hex("0000000000000000000000000000000000000000000000000000000000000000") {}
    MockBlockHash(const std::string& h) : hex(h) {}

    bool operator==(const MockBlockHash& other) const { return hex == other.hex; }
    bool operator!=(const MockBlockHash& other) const { return hex != other.hex; }
    bool IsNull() const { return hex == "0000000000000000000000000000000000000000000000000000000000000000"; }
};

struct MockBlockIndex {
    int32_t height;
    MockBlockHash hash;
    MockBlockIndex* pprev;
    uint64_t chainwork;

    MockBlockIndex() : height(0), pprev(nullptr), chainwork(0) {}
    MockBlockIndex(int32_t h, const std::string& hash_hex, MockBlockIndex* prev = nullptr)
        : height(h), hash(hash_hex), pprev(prev), chainwork(h * 1000) {}
};

struct MockBlockHeader {
    MockBlockHash prev_block_hash;
    MockBlockHash merkle_root;
    uint32_t timestamp;
    uint32_t bits;  // Difficulty target
    uint32_t nonce;

    MockBlockHeader() : timestamp(0), bits(0), nonce(0) {}
};

struct MockBlock {
    MockBlockHeader header;
    std::vector<std::string> vtx;  // Simplified transaction list
};

// ============================================================================
// Invariant Validation Functions
// ============================================================================

struct InvariantResult {
    bool valid;
    std::string error;

    InvariantResult() : valid(true) {}
    InvariantResult(bool v, const std::string& e = "") : valid(v), error(e) {}

    static InvariantResult Ok() { return InvariantResult(true); }
    static InvariantResult Fail(const std::string& e) { return InvariantResult(false, e); }
};

// D1: Tip height must be non-negative
InvariantResult ValidateTipHeightNonNegative(const MockBlockIndex* tip) {
    if (!tip) {
        return InvariantResult::Fail("D1 VIOLATION: tip is null");
    }
    if (tip->height < 0) {
        return InvariantResult::Fail("D1 VIOLATION: tip height is negative (" +
                                     std::to_string(tip->height) + ")");
    }
    return InvariantResult::Ok();
}

// D2: Fork point must be valid
InvariantResult ValidateForkPoint(const MockBlockIndex* fork_point) {
    if (!fork_point) {
        return InvariantResult::Fail("D2 VIOLATION: fork point is null");
    }
    if (fork_point->height < 0) {
        return InvariantResult::Fail("D2 VIOLATION: fork point height is negative (" +
                                     std::to_string(fork_point->height) + ")");
    }
    return InvariantResult::Ok();
}

// D3: Prev block hash must match expected parent
InvariantResult ValidatePrevBlockHash(const MockBlock& block, const MockBlockIndex* expected_parent) {
    if (!expected_parent) {
        // Genesis block case - prev_hash should be null
        if (!block.header.prev_block_hash.IsNull()) {
            return InvariantResult::Fail("D3 VIOLATION: Genesis block has non-null prev_hash");
        }
        return InvariantResult::Ok();
    }

    if (block.header.prev_block_hash != expected_parent->hash) {
        return InvariantResult::Fail("D3 VIOLATION: prev_block_hash mismatch (expected " +
                                     expected_parent->hash.hex.substr(0, 16) + "..., got " +
                                     block.header.prev_block_hash.hex.substr(0, 16) + "...)");
    }
    return InvariantResult::Ok();
}

// D4: Block timestamp must not be > 2 hours in future
InvariantResult ValidateTimestamp(uint32_t block_timestamp, uint32_t network_time) {
    constexpr uint32_t MAX_FUTURE_SECONDS = 2 * 60 * 60;  // 2 hours

    if (block_timestamp > network_time + MAX_FUTURE_SECONDS) {
        return InvariantResult::Fail("D4 VIOLATION: block timestamp too far in future (" +
                                     std::to_string(block_timestamp) + " > " +
                                     std::to_string(network_time + MAX_FUTURE_SECONDS) + ")");
    }
    return InvariantResult::Ok();
}

// D5: UTXO count must be non-negative
InvariantResult ValidateUTXOCount(int64_t utxo_count) {
    if (utxo_count < 0) {
        return InvariantResult::Fail("D5 VIOLATION: UTXO count is negative (" +
                                     std::to_string(utxo_count) + ")");
    }
    return InvariantResult::Ok();
}

// D6: Tip height monotonically increases (except during reorg)
InvariantResult ValidateTipHeightMonotonic(int32_t old_height, int32_t new_height, bool is_reorg) {
    if (!is_reorg && new_height < old_height) {
        return InvariantResult::Fail("D6 VIOLATION: tip height decreased without reorg (" +
                                     std::to_string(old_height) + " -> " +
                                     std::to_string(new_height) + ")");
    }
    return InvariantResult::Ok();
}

// ============================================================================
// Mock FindFork Implementation
// ============================================================================

MockBlockIndex* FindFork(MockBlockIndex* a, MockBlockIndex* b) {
    if (!a || !b) return nullptr;

    // Walk both chains to same height
    while (a->height > b->height && a->pprev) a = a->pprev;
    while (b->height > a->height && b->pprev) b = b->pprev;

    // Walk both chains to common ancestor
    while (a != b && a && b) {
        a = a->pprev;
        b = b->pprev;
    }

    return a;  // Common ancestor (or nullptr if chains don't share ancestor)
}

// ============================================================================
// D1: Tip Height Non-Negative Tests
// ============================================================================

TEST(D1_ValidTipHeight) {
    MockBlockIndex tip(100, "abc123");
    auto result = ValidateTipHeightNonNegative(&tip);
    ASSERT_TRUE(result.valid);
}

TEST(D1_ZeroTipHeight) {
    MockBlockIndex genesis(0, "genesis");
    auto result = ValidateTipHeightNonNegative(&genesis);
    ASSERT_TRUE(result.valid);  // Height 0 is valid (genesis)
}

TEST(D1_NegativeTipHeightViolation) {
    MockBlockIndex bad_tip(-1, "invalid");
    auto result = ValidateTipHeightNonNegative(&bad_tip);
    ASSERT_FALSE(result.valid);
    ASSERT_TRUE(result.error.find("D1 VIOLATION") != std::string::npos);
    std::cout << "\n    " << result.error << std::endl;
}

TEST(D1_NullTipViolation) {
    auto result = ValidateTipHeightNonNegative(nullptr);
    ASSERT_FALSE(result.valid);
    ASSERT_TRUE(result.error.find("D1 VIOLATION") != std::string::npos);
}

// ============================================================================
// D2: Fork Point Validity Tests
// ============================================================================

TEST(D2_ValidForkPoint) {
    MockBlockIndex fork(50, "fork_hash");
    auto result = ValidateForkPoint(&fork);
    ASSERT_TRUE(result.valid);
}

TEST(D2_NullForkPointViolation) {
    auto result = ValidateForkPoint(nullptr);
    ASSERT_FALSE(result.valid);
    ASSERT_TRUE(result.error.find("D2 VIOLATION") != std::string::npos);
    std::cout << "\n    " << result.error << std::endl;
}

TEST(D2_NegativeForkHeightViolation) {
    MockBlockIndex bad_fork(-5, "bad_fork");
    auto result = ValidateForkPoint(&bad_fork);
    ASSERT_FALSE(result.valid);
    ASSERT_TRUE(result.error.find("D2 VIOLATION") != std::string::npos);
}

TEST(D2_FindForkReturnsValidResult) {
    // Build two chains that share genesis
    MockBlockIndex genesis(0, "genesis_hash");
    MockBlockIndex block1(1, "block1", &genesis);
    MockBlockIndex block2(2, "block2", &block1);
    MockBlockIndex block2b(2, "block2b", &block1);  // Fork at height 1
    MockBlockIndex block3b(3, "block3b", &block2b);

    MockBlockIndex* fork = FindFork(&block2, &block3b);

    ASSERT_TRUE(fork != nullptr);
    auto result = ValidateForkPoint(fork);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(fork->height, 1);  // Fork point is block1
}

// ============================================================================
// D3: Prev Block Hash Tests
// ============================================================================

TEST(D3_ValidPrevHash) {
    MockBlockIndex parent(99, "parent_hash_abc");
    MockBlock block;
    block.header.prev_block_hash = MockBlockHash("parent_hash_abc");

    auto result = ValidatePrevBlockHash(block, &parent);
    ASSERT_TRUE(result.valid);
}

TEST(D3_GenesisPrevHashNull) {
    MockBlock genesis_block;
    genesis_block.header.prev_block_hash = MockBlockHash();  // Null hash

    auto result = ValidatePrevBlockHash(genesis_block, nullptr);
    ASSERT_TRUE(result.valid);  // Genesis should have null parent
}

TEST(D3_PrevHashMismatchViolation) {
    MockBlockIndex parent(99, "correct_hash");
    MockBlock block;
    block.header.prev_block_hash = MockBlockHash("wrong_hash");

    auto result = ValidatePrevBlockHash(block, &parent);
    ASSERT_FALSE(result.valid);
    ASSERT_TRUE(result.error.find("D3 VIOLATION") != std::string::npos);
    std::cout << "\n    " << result.error << std::endl;
}

TEST(D3_GenesisNonNullPrevHashViolation) {
    MockBlock bad_genesis;
    bad_genesis.header.prev_block_hash = MockBlockHash("non_null_hash");

    auto result = ValidatePrevBlockHash(bad_genesis, nullptr);
    ASSERT_FALSE(result.valid);
    ASSERT_TRUE(result.error.find("D3 VIOLATION") != std::string::npos);
}

// ============================================================================
// D4: Timestamp Validation Tests
// ============================================================================

TEST(D4_ValidTimestamp) {
    uint32_t network_time = 1700000000;  // Some recent timestamp
    uint32_t block_time = network_time + 60;  // 1 minute in future - OK

    auto result = ValidateTimestamp(block_time, network_time);
    ASSERT_TRUE(result.valid);
}

TEST(D4_TimestampAtMaxFuture) {
    uint32_t network_time = 1700000000;
    uint32_t block_time = network_time + (2 * 60 * 60);  // Exactly 2 hours - OK

    auto result = ValidateTimestamp(block_time, network_time);
    ASSERT_TRUE(result.valid);
}

TEST(D4_TimestampTooFarFutureViolation) {
    uint32_t network_time = 1700000000;
    uint32_t block_time = network_time + (2 * 60 * 60) + 1;  // 2 hours + 1 second

    auto result = ValidateTimestamp(block_time, network_time);
    ASSERT_FALSE(result.valid);
    ASSERT_TRUE(result.error.find("D4 VIOLATION") != std::string::npos);
    std::cout << "\n    " << result.error << std::endl;
}

TEST(D4_TimestampInPast) {
    uint32_t network_time = 1700000000;
    uint32_t block_time = network_time - 3600;  // 1 hour in past - OK

    auto result = ValidateTimestamp(block_time, network_time);
    ASSERT_TRUE(result.valid);  // Past timestamps are allowed (MTP handles this)
}

// ============================================================================
// D5: UTXO Count Tests
// ============================================================================

TEST(D5_ValidUTXOCount) {
    auto result = ValidateUTXOCount(1000000);
    ASSERT_TRUE(result.valid);
}

TEST(D5_ZeroUTXOCount) {
    auto result = ValidateUTXOCount(0);
    ASSERT_TRUE(result.valid);  // Zero is valid (empty UTXO set)
}

TEST(D5_NegativeUTXOCountViolation) {
    auto result = ValidateUTXOCount(-1);
    ASSERT_FALSE(result.valid);
    ASSERT_TRUE(result.error.find("D5 VIOLATION") != std::string::npos);
    std::cout << "\n    " << result.error << std::endl;
}

TEST(D5_LargeNegativeViolation) {
    auto result = ValidateUTXOCount(-9223372036854775807LL);  // Near min int64
    ASSERT_FALSE(result.valid);
}

// ============================================================================
// D6: Tip Height Monotonicity Tests
// ============================================================================

TEST(D6_HeightIncreases) {
    auto result = ValidateTipHeightMonotonic(100, 101, false);
    ASSERT_TRUE(result.valid);
}

TEST(D6_HeightSameNoReorg) {
    // Height staying same without reorg is technically valid (orphan replacement)
    auto result = ValidateTipHeightMonotonic(100, 100, false);
    ASSERT_TRUE(result.valid);
}

TEST(D6_HeightDecreasesWithReorg) {
    // Height can decrease during reorg
    auto result = ValidateTipHeightMonotonic(100, 95, true);
    ASSERT_TRUE(result.valid);
}

TEST(D6_HeightDecreasesWithoutReorgViolation) {
    auto result = ValidateTipHeightMonotonic(100, 99, false);
    ASSERT_FALSE(result.valid);
    ASSERT_TRUE(result.error.find("D6 VIOLATION") != std::string::npos);
    std::cout << "\n    " << result.error << std::endl;
}

// ============================================================================
// Combined Invariant Check (Simulated Block Connect)
// ============================================================================

struct MockChainstate {
    MockBlockIndex* active_tip = nullptr;
    int64_t utxo_count = 0;

    InvariantResult ConnectBlockWithInvariants(MockBlock& block, MockBlockIndex* new_tip) {
        // D1: Check tip height non-negative
        auto d1 = ValidateTipHeightNonNegative(new_tip);
        if (!d1.valid) return d1;

        // D3: Check prev_hash matches
        auto d3 = ValidatePrevBlockHash(block, active_tip);
        if (!d3.valid) return d3;

        // D4: Check timestamp
        uint32_t network_time = static_cast<uint32_t>(std::time(nullptr));
        auto d4 = ValidateTimestamp(block.header.timestamp, network_time);
        if (!d4.valid) return d4;

        // D6: Check monotonicity
        int32_t old_height = active_tip ? active_tip->height : -1;
        auto d6 = ValidateTipHeightMonotonic(old_height, new_tip->height, false);
        if (!d6.valid) return d6;

        // D5: Check UTXO count after changes
        auto d5 = ValidateUTXOCount(utxo_count);
        if (!d5.valid) return d5;

        // All invariants pass - update state
        active_tip = new_tip;
        return InvariantResult::Ok();
    }
};

TEST(Combined_ValidBlockConnect) {
    MockChainstate chainstate;

    // Genesis
    MockBlockIndex genesis(0, "genesis");
    MockBlock genesis_block;
    genesis_block.header.timestamp = static_cast<uint32_t>(std::time(nullptr));

    auto result = chainstate.ConnectBlockWithInvariants(genesis_block, &genesis);
    ASSERT_TRUE(result.valid);

    // Block 1
    MockBlockIndex block1(1, "block1", &genesis);
    MockBlock block1_data;
    block1_data.header.prev_block_hash = genesis.hash;
    block1_data.header.timestamp = static_cast<uint32_t>(std::time(nullptr));

    result = chainstate.ConnectBlockWithInvariants(block1_data, &block1);
    ASSERT_TRUE(result.valid);
}

TEST(Combined_InvalidBlockRejected) {
    MockChainstate chainstate;

    // Set up genesis
    MockBlockIndex genesis(0, "genesis");
    chainstate.active_tip = &genesis;

    // Try to connect block with wrong prev_hash
    MockBlockIndex block1(1, "block1", &genesis);
    MockBlock bad_block;
    bad_block.header.prev_block_hash = MockBlockHash("wrong_parent");
    bad_block.header.timestamp = static_cast<uint32_t>(std::time(nullptr));

    auto result = chainstate.ConnectBlockWithInvariants(bad_block, &block1);
    ASSERT_FALSE(result.valid);
    ASSERT_TRUE(result.error.find("D3 VIOLATION") != std::string::npos);
}

// ============================================================================
// Integration Test Stubs
// ============================================================================

TEST(Integration_RealBlockValidation) {
    throw std::runtime_error("Requires real BlockValidator - SKIP");
}

TEST(Integration_RealChainstate) {
    throw std::runtime_error("Requires real ChainstateService - SKIP");
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "\n=== Daemon Invariants Tests (Priority 5) ===" << std::endl;
    std::cout << "Testing invariants D1-D6\n" << std::endl;

    std::cout << "D1: Tip Height Non-Negative Tests" << std::endl;
    RUN_TEST(D1_ValidTipHeight);
    RUN_TEST(D1_ZeroTipHeight);
    RUN_TEST(D1_NegativeTipHeightViolation);
    RUN_TEST(D1_NullTipViolation);

    std::cout << "\nD2: Fork Point Validity Tests" << std::endl;
    RUN_TEST(D2_ValidForkPoint);
    RUN_TEST(D2_NullForkPointViolation);
    RUN_TEST(D2_NegativeForkHeightViolation);
    RUN_TEST(D2_FindForkReturnsValidResult);

    std::cout << "\nD3: Prev Block Hash Tests" << std::endl;
    RUN_TEST(D3_ValidPrevHash);
    RUN_TEST(D3_GenesisPrevHashNull);
    RUN_TEST(D3_PrevHashMismatchViolation);
    RUN_TEST(D3_GenesisNonNullPrevHashViolation);

    std::cout << "\nD4: Timestamp Validation Tests" << std::endl;
    RUN_TEST(D4_ValidTimestamp);
    RUN_TEST(D4_TimestampAtMaxFuture);
    RUN_TEST(D4_TimestampTooFarFutureViolation);
    RUN_TEST(D4_TimestampInPast);

    std::cout << "\nD5: UTXO Count Tests" << std::endl;
    RUN_TEST(D5_ValidUTXOCount);
    RUN_TEST(D5_ZeroUTXOCount);
    RUN_TEST(D5_NegativeUTXOCountViolation);
    RUN_TEST(D5_LargeNegativeViolation);

    std::cout << "\nD6: Tip Height Monotonicity Tests" << std::endl;
    RUN_TEST(D6_HeightIncreases);
    RUN_TEST(D6_HeightSameNoReorg);
    RUN_TEST(D6_HeightDecreasesWithReorg);
    RUN_TEST(D6_HeightDecreasesWithoutReorgViolation);

    std::cout << "\nCombined Invariant Tests" << std::endl;
    RUN_TEST(Combined_ValidBlockConnect);
    RUN_TEST(Combined_InvalidBlockRejected);

    std::cout << "\nIntegration Tests (require real classes)" << std::endl;
    SKIP_TEST(Integration_RealBlockValidation, "requires real BlockValidator");
    SKIP_TEST(Integration_RealChainstate, "requires real ChainstateService");

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Total:   " << tests_run << std::endl;
    std::cout << "Passed:  " << tests_passed << std::endl;
    std::cout << "Failed:  " << tests_failed << std::endl;
    std::cout << "Skipped: " << tests_skipped << std::endl;

    return tests_failed > 0 ? 1 : 0;
}
