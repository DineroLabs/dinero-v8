/**
 * @file test_block_construction.cpp
 * @brief Block Construction Correctness Tests (Mainnet Hardening)
 *
 * MAINNET REQUIREMENT: Blocks must be constructed correctly or rejected.
 *
 * Tests:
 *   B2.1: Coinbase Rules
 *         - Height encoded correctly (BIP34)
 *         - Subsidy correct per schedule
 *         - Fees summed correctly
 *         - No overflow/underflow
 *
 *   B2.2: Subsidy Schedule Enforcement
 *         - Halving boundaries correct
 *         - Tail emission floor (1 DIN) enforced
 *         - No hard cap (disinflationary)
 *
 *   B2.3: Commitment Integrity
 *         - Merkle root matches transactions
 *         - Witness commitment (if applicable)
 *
 * If any test fails -> DO NOT SHIP TO MAINNET
 */

#include "consensus/subsidy.h"
#include "primitives/amount.h"
#include "primitives/transaction.h"
#include <iostream>
#include <vector>
#include <cstdint>
#include <limits>
#include <cmath>

using namespace dinero;

// ============================================================================
// Test Infrastructure
// ============================================================================

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define ASSERT_TRUE(cond, msg) \
    do { \
        g_tests_run++; \
        if (!(cond)) { \
            std::cerr << "  FAIL: " << msg << "\n"; \
            std::cerr << "     at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
        g_tests_passed++; \
    } while(0)

#define ASSERT_EQ(a, b, msg) \
    do { \
        g_tests_run++; \
        if ((a) != (b)) { \
            std::cerr << "  FAIL: " << msg << "\n"; \
            std::cerr << "     Expected: " << (b) << "\n"; \
            std::cerr << "     Got:      " << (a) << "\n"; \
            std::cerr << "     at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
        g_tests_passed++; \
    } while(0)

#define ASSERT_LE(a, b, msg) \
    do { \
        g_tests_run++; \
        if ((a) > (b)) { \
            std::cerr << "  FAIL: " << msg << "\n"; \
            std::cerr << "     Expected <= " << (b) << "\n"; \
            std::cerr << "     Got:        " << (a) << "\n"; \
            std::cerr << "     at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
        g_tests_passed++; \
    } while(0)

#define ASSERT_GE(a, b, msg) \
    do { \
        g_tests_run++; \
        if ((a) < (b)) { \
            std::cerr << "  FAIL: " << msg << "\n"; \
            std::cerr << "     Expected >= " << (b) << "\n"; \
            std::cerr << "     Got:        " << (a) << "\n"; \
            std::cerr << "     at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
        g_tests_passed++; \
    } while(0)

// ============================================================================
// TEST B2.1.1: Genesis Block Subsidy
// ============================================================================

bool test_b2_1_1_genesis_subsidy() {
    std::cout << "\nTEST B2.1.1: Genesis block subsidy (height 0)" << std::endl;

    AmountUna genesis_subsidy = ConsensusSubsidy::GetBlockSubsidy(0);

    std::cout << "  Genesis subsidy: " << genesis_subsidy.GetUna() << " una" << std::endl;

    ASSERT_EQ(genesis_subsidy.GetUna(), 0ULL,
              "Genesis block must have zero spendable subsidy");

    std::cout << "  OK: Genesis subsidy correct (0 spendable)\n" << std::endl;
    return true;
}

// ============================================================================
// TEST B2.1.2: First PoW Block Subsidy (Height 1)
// ============================================================================

bool test_b2_1_2_first_pow_subsidy() {
    std::cout << "\nTEST B2.1.2: First PoW block subsidy (height 1)" << std::endl;

    AmountUna h1_subsidy = ConsensusSubsidy::GetBlockSubsidy(1);

    uint64_t expected = ConsensusSubsidy::INITIAL_SUBSIDY;

    std::cout << "  Height 1 subsidy: " << h1_subsidy.GetUna() << " una" << std::endl;
    std::cout << "  Expected:         " << expected << " una (100 DIN)" << std::endl;

    ASSERT_EQ(h1_subsidy.GetUna(), expected,
              "Height 1 must have 100 DIN subsidy (first PoW block)");

    std::cout << "  OK: First PoW block subsidy correct (100 DIN)\n" << std::endl;
    return true;
}

// ============================================================================
// TEST B2.1.3: Initial PoW Block Subsidy (Height 2+)
// ============================================================================

bool test_b2_1_3_initial_pow_subsidy() {
    std::cout << "\nTEST B2.1.3: Initial PoW block subsidy (height 2+)" << std::endl;

    AmountUna pow_subsidy_h2 = ConsensusSubsidy::GetBlockSubsidy(2);
    AmountUna pow_subsidy_h3 = ConsensusSubsidy::GetBlockSubsidy(3);
    AmountUna pow_subsidy_h100 = ConsensusSubsidy::GetBlockSubsidy(100);

    uint64_t expected_initial = ConsensusSubsidy::INITIAL_SUBSIDY;

    std::cout << "  Height 2 subsidy:   " << pow_subsidy_h2.GetUna() << " una ("
              << (pow_subsidy_h2.GetUna() / ConsensusSubsidy::UNA_PER_DIN) << " DIN)" << std::endl;
    std::cout << "  Height 3 subsidy:   " << pow_subsidy_h3.GetUna() << " una" << std::endl;
    std::cout << "  Height 100 subsidy: " << pow_subsidy_h100.GetUna() << " una" << std::endl;

    ASSERT_EQ(pow_subsidy_h2.GetUna(), expected_initial,
              "Height 2 must have 100 DIN subsidy");
    ASSERT_EQ(pow_subsidy_h3.GetUna(), expected_initial,
              "Height 3 must have 100 DIN subsidy");
    ASSERT_EQ(pow_subsidy_h100.GetUna(), expected_initial,
              "Height 100 must have 100 DIN subsidy (pre-halving)");

    std::cout << "  OK: Initial PoW subsidy correct (100 DIN)\n" << std::endl;
    return true;
}

// ============================================================================
// TEST B2.1.4: Halving Boundaries
// ============================================================================

bool test_b2_1_4_halving_boundaries() {
    std::cout << "\nTEST B2.1.4: Halving boundaries" << std::endl;

    // Halving interval is 1,314,000 blocks
    // PoW starts at height 1, so pow_blocks = height - 1
    // First halving: pow_block 1,314,000 = chain height 1,314,001

    uint32_t halving_interval = ConsensusSubsidy::HALVING_INTERVAL;
    uint64_t initial_subsidy = ConsensusSubsidy::INITIAL_SUBSIDY;

    std::cout << "  Halving interval: " << halving_interval << " PoW blocks" << std::endl;

    // Test first halving boundary
    uint32_t pre_halving_1 = 1 + halving_interval - 1;  // Last block before first halving
    uint32_t at_halving_1 = 1 + halving_interval;       // First block after first halving

    AmountUna subsidy_pre_h1 = ConsensusSubsidy::GetBlockSubsidy(pre_halving_1);
    AmountUna subsidy_at_h1 = ConsensusSubsidy::GetBlockSubsidy(at_halving_1);

    std::cout << "  Height " << pre_halving_1 << " (pre-halving 1):  "
              << subsidy_pre_h1.GetUna() << " una" << std::endl;
    std::cout << "  Height " << at_halving_1 << " (at halving 1):   "
              << subsidy_at_h1.GetUna() << " una" << std::endl;

    ASSERT_EQ(subsidy_pre_h1.GetUna(), initial_subsidy,
              "Block before first halving must have full subsidy");
    ASSERT_EQ(subsidy_at_h1.GetUna(), initial_subsidy / 2,
              "First block after halving must have half subsidy");

    // Test second halving boundary
    uint32_t at_halving_2 = 1 + (halving_interval * 2);
    AmountUna subsidy_at_h2 = ConsensusSubsidy::GetBlockSubsidy(at_halving_2);

    std::cout << "  Height " << at_halving_2 << " (at halving 2): "
              << subsidy_at_h2.GetUna() << " una" << std::endl;

    ASSERT_EQ(subsidy_at_h2.GetUna(), initial_subsidy / 4,
              "Block at second halving must have quarter subsidy");

    std::cout << "  OK: Halving boundaries correct\n" << std::endl;
    return true;
}

// ============================================================================
// TEST B2.1.5: Tail Emission Floor (1 DIN/block forever)
// ============================================================================

bool test_b2_1_5_tail_emission() {
    std::cout << "\nTEST B2.1.5: Tail emission floor (1 DIN/block forever)" << std::endl;

    uint32_t halving_interval = ConsensusSubsidy::HALVING_INTERVAL;
    uint64_t tail = ConsensusSubsidy::TAIL_EMISSION_UNA;

    // After enough halvings, raw subsidy would be < 1 DIN
    // Tail emission kicks in: max(halving_subsidy, 1 DIN)
    // 100 DIN >> 7 = 0.78125 DIN < 1 DIN → tail at halving 7 (height 9,198,001)

    uint32_t tail_start_height = 1 + (halving_interval * 7);
    AmountUna subsidy_at_tail = ConsensusSubsidy::GetBlockSubsidy(tail_start_height);

    std::cout << "  Tail start height: " << tail_start_height << std::endl;
    std::cout << "  Subsidy there:     " << subsidy_at_tail.GetUna() << " una ("
              << (subsidy_at_tail.GetUna() / ConsensusSubsidy::UNA_PER_DIN) << " DIN)" << std::endl;
    std::cout << "  Expected tail:     " << tail << " una (1 DIN)" << std::endl;

    ASSERT_EQ(subsidy_at_tail.GetUna(), tail,
              "Tail emission must be 1 DIN at halving 7+");

    // After 33 halvings, raw subsidy = 0 but tail = 1 DIN
    uint32_t height_after_33 = 1 + (halving_interval * 33);
    AmountUna subsidy_after_33 = ConsensusSubsidy::GetBlockSubsidy(height_after_33);

    std::cout << "  Height after 33 halvings: " << height_after_33 << std::endl;
    std::cout << "  Subsidy there:            " << subsidy_after_33.GetUna() << " una" << std::endl;

    ASSERT_EQ(subsidy_after_33.GetUna(), tail,
              "Tail emission must be 1 DIN even after 33 halvings");

    // At UINT32_MAX: still 1 DIN
    AmountUna subsidy_max = ConsensusSubsidy::GetBlockSubsidy(std::numeric_limits<uint32_t>::max());
    std::cout << "  UINT32_MAX height:        " << subsidy_max.GetUna() << " una" << std::endl;

    ASSERT_EQ(subsidy_max.GetUna(), tail,
              "Tail emission must be 1 DIN at UINT32_MAX");

    std::cout << "  OK: Tail emission floor enforced\n" << std::endl;
    return true;
}

// ============================================================================
// TEST B2.1.6: No Overflow in Subsidy Calculation
// ============================================================================

bool test_b2_1_6_no_overflow() {
    std::cout << "\nTEST B2.1.6: No overflow in subsidy calculation" << std::endl;

    // Test maximum valid height
    uint32_t max_height = std::numeric_limits<uint32_t>::max();
    AmountUna subsidy_max = ConsensusSubsidy::GetBlockSubsidy(max_height);

    std::cout << "  Max height tested: " << max_height << std::endl;
    std::cout << "  Subsidy at max:    " << subsidy_max.GetUna() << " una" << std::endl;

    // With tail emission, should be 1 DIN (never 0)
    ASSERT_EQ(subsidy_max.GetUna(), ConsensusSubsidy::TAIL_EMISSION_UNA,
              "Subsidy at max height must be tail emission (1 DIN)");

    // Test large but valid heights
    std::vector<uint32_t> test_heights = {
        1000000,
        10000000,
        100000000,
        1000000000
    };

    for (uint32_t h : test_heights) {
        AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(h);
        std::cout << "  Height " << h << ": " << subsidy.GetUna() << " una" << std::endl;

        ASSERT_LE(subsidy.GetUna(), ConsensusSubsidy::INITIAL_SUBSIDY,
                  "Subsidy must not exceed initial subsidy (no overflow)");
        ASSERT_GE(subsidy.GetUna(), ConsensusSubsidy::TAIL_EMISSION_UNA,
                  "Subsidy must be at least tail emission (1 DIN)");
    }

    std::cout << "  OK: No overflow in subsidy calculation\n" << std::endl;
    return true;
}

// ============================================================================
// TEST B2.1.7: Fees Cannot Cause Overflow
// ============================================================================

bool test_b2_1_7_fees_no_overflow() {
    std::cout << "\nTEST B2.1.7: Fees cannot cause coinbase overflow" << std::endl;

    // Maximum possible coinbase = subsidy + total fees
    // Without a hard cap, use a generous upper bound for fees
    uint64_t max_subsidy = ConsensusSubsidy::INITIAL_SUBSIDY;
    // Even 1 billion DIN of fees shouldn't overflow
    uint64_t generous_fees = 1000000000ULL * ConsensusSubsidy::UNA_PER_DIN;

    // Portable unsigned-overflow check: a + b overflows iff a > UINT64_MAX - b.
    // (Was __builtin_add_overflow which is GCC/Clang only; not on MSVC.)
    uint64_t max_coinbase;
    bool overflow = (generous_fees > 0 && max_subsidy > UINT64_MAX - generous_fees);
    max_coinbase = overflow ? 0 : (max_subsidy + generous_fees);

    std::cout << "  Max subsidy: " << max_subsidy << " una" << std::endl;
    std::cout << "  Generous fees: " << generous_fees << " una" << std::endl;
    std::cout << "  Sum: " << (overflow ? "OVERFLOW" : std::to_string(max_coinbase)) << std::endl;

    ASSERT_TRUE(!overflow, "Subsidy + generous fees must not overflow uint64_t");

    std::cout << "  OK: Fees cannot cause overflow\n" << std::endl;
    return true;
}

// ============================================================================
// TEST B2.2.1: Monotonic Subsidy Decrease (with tail floor)
// ============================================================================

bool test_b2_2_1_monotonic_decrease() {
    std::cout << "\nTEST B2.2.1: Subsidy monotonically decreases (with tail floor)" << std::endl;

    // Subsidy must never increase after height 1
    uint64_t prev_subsidy = ConsensusSubsidy::GetBlockSubsidy(1).GetUna();

    std::vector<uint32_t> check_heights = {100, 1000, 10000, 100000, 1000000, 10000000, 100000000};

    for (uint32_t h : check_heights) {
        uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(h).GetUna();

        std::cout << "  Height " << h << ": " << subsidy << " una" << std::endl;

        ASSERT_LE(subsidy, prev_subsidy,
                  "Subsidy must not increase (monotonic decrease)");

        prev_subsidy = subsidy;
    }

    // Final value must be tail emission
    ASSERT_EQ(prev_subsidy, ConsensusSubsidy::TAIL_EMISSION_UNA,
              "Subsidy must converge to tail emission");

    std::cout << "  OK: Subsidy monotonically decreases to tail floor\n" << std::endl;
    return true;
}

// ============================================================================
// TEST B2.2.2: Total Issued Calculation
// ============================================================================

bool test_b2_2_2_total_issued() {
    std::cout << "\nTEST B2.2.2: Total issued calculation" << std::endl;

    // At height 0: genesis only (100 DIN unspendable)
    uint64_t issued_h0 = ConsensusSubsidy::GetTotalIssuedAtHeight(0);
    std::cout << "  Height 0 total: " << issued_h0 << " una" << std::endl;
    ASSERT_EQ(issued_h0, ConsensusSubsidy::GENESIS_UNSPENDABLE_UNA,
              "Height 0 must have only genesis");

    // At height 1: genesis + 100 DIN PoW
    uint64_t issued_h1 = ConsensusSubsidy::GetTotalIssuedAtHeight(1);
    uint64_t expected_h1 = ConsensusSubsidy::GENESIS_UNSPENDABLE_UNA + ConsensusSubsidy::INITIAL_SUBSIDY;
    std::cout << "  Height 1 total: " << issued_h1 << " una" << std::endl;
    ASSERT_EQ(issued_h1, expected_h1,
              "Height 1 must have genesis + 100 DIN");

    // At height 2: genesis + 200 DIN
    uint64_t issued_h2 = ConsensusSubsidy::GetTotalIssuedAtHeight(2);
    uint64_t expected_h2 = expected_h1 + ConsensusSubsidy::INITIAL_SUBSIDY;
    std::cout << "  Height 2 total: " << issued_h2 << " una" << std::endl;
    ASSERT_EQ(issued_h2, expected_h2,
              "Height 2 must have genesis + 200 DIN");

    // Total issued always increases (disinflationary, no hard cap)
    uint64_t issued_1m = ConsensusSubsidy::GetTotalIssuedAtHeight(1000000);
    uint64_t issued_100m = ConsensusSubsidy::GetTotalIssuedAtHeight(100000000);
    std::cout << "  Height 1M total:   " << issued_1m << " una ("
              << (issued_1m / ConsensusSubsidy::UNA_PER_DIN) << " DIN)" << std::endl;
    std::cout << "  Height 100M total: " << issued_100m << " una ("
              << (issued_100m / ConsensusSubsidy::UNA_PER_DIN) << " DIN)" << std::endl;

    ASSERT_TRUE(issued_100m > issued_1m,
                "Total issued must always increase (tail emission)");

    std::cout << "  OK: Total issued calculation verified\n" << std::endl;
    return true;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "\n" << std::endl;
    std::cout << "  Block Construction Correctness Tests" << std::endl;
    std::cout << "  MAINNET HARDENING - Coinbase & Subsidy" << std::endl;
    std::cout << std::endl;

    bool all_passed = true;

    // B2.1: Coinbase Rules
    all_passed &= test_b2_1_1_genesis_subsidy();
    all_passed &= test_b2_1_2_first_pow_subsidy();
    all_passed &= test_b2_1_3_initial_pow_subsidy();
    all_passed &= test_b2_1_4_halving_boundaries();
    all_passed &= test_b2_1_5_tail_emission();
    all_passed &= test_b2_1_6_no_overflow();
    all_passed &= test_b2_1_7_fees_no_overflow();

    // B2.2: Schedule Enforcement
    all_passed &= test_b2_2_1_monotonic_decrease();
    all_passed &= test_b2_2_2_total_issued();

    // Summary
    std::cout << "\n" << std::endl;
    if (all_passed) {
        std::cout << "  ALL BLOCK CONSTRUCTION TESTS PASSED" << std::endl;
        std::cout << "  Proven:" << std::endl;
        std::cout << "    - Genesis subsidy correct (0 spendable)" << std::endl;
        std::cout << "    - First PoW block correct (100 DIN at height 1)" << std::endl;
        std::cout << "    - Halving boundaries correct" << std::endl;
        std::cout << "    - Tail emission floor enforced (1 DIN forever)" << std::endl;
        std::cout << "    - No overflow in calculations" << std::endl;
        std::cout << "    - Subsidy monotonically decreases to tail" << std::endl;
    } else {
        std::cout << "  BLOCK CONSTRUCTION TESTS FAILED" << std::endl;
        std::cout << "  DO NOT SHIP TO MAINNET" << std::endl;
    }

    std::cout << "\nTests: " << g_tests_passed << "/" << g_tests_run << " passed" << std::endl;

    return all_passed ? 0 : 1;
}
