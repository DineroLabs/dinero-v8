/**
 * Ring 4 Phase 4c: Subsidy Calculator Tests
 *
 * Purpose: Verify subsidy calculation matches Ring 1 frozen spec
 * Tests: Genesis, initial subsidy, halving, boundary conditions
 */

#include "subsidy_calculator.h"
#include "consensus_params.h"
#include <cassert>
#include <iostream>
#include <string>

using namespace mining_test;

// ============================================================================
// Test Utilities
// ============================================================================

void assert_true(bool condition, const std::string& msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::exit(1);
    }
}

void assert_eq(uint64_t a, uint64_t b, const std::string& msg) {
    if (a != b) {
        std::cerr << "FAIL: " << msg << " (expected " << b << ", got " << a << ")" << std::endl;
        std::exit(1);
    }
}

void assert_eq(uint32_t a, uint32_t b, const std::string& msg) {
    if (a != b) {
        std::cerr << "FAIL: " << msg << " (expected " << b << ", got " << a << ")" << std::endl;
        std::exit(1);
    }
}

// ============================================================================
// Test 1: Genesis Block Subsidy
// ============================================================================

void test_genesis_subsidy() {
    std::cout << "Running test_genesis_subsidy..." << std::endl;

    ConsensusParams params = ConsensusParams::mainnet();
    ConsensusSubsidyCalculator calc(params);

    // Genesis block (height 0) should return genesis_subsidy
    uint64_t genesis = calc.getBlockSubsidy(0);
    assert_eq(genesis, params.genesis_subsidy, "Genesis subsidy should match params");
    assert_eq(genesis, uint64_t(0), "Mainnet has no premine");

    std::cout << "  ✅ Genesis subsidy test passed" << std::endl;
}

// ============================================================================
// Test 2: Initial Block Subsidy
// ============================================================================

void test_initial_subsidy() {
    std::cout << "Running test_initial_subsidy..." << std::endl;

    ConsensusParams params = ConsensusParams::mainnet();
    ConsensusSubsidyCalculator calc(params);

    // Block 1 should have initial subsidy (100 DIN = 10,000,000,000 una)
    uint64_t block1 = calc.getBlockSubsidy(1);
    assert_eq(block1, 100ULL * 100000000ULL, "Block 1 should have 100 DIN subsidy");

    // All blocks before first halving should have same subsidy
    uint64_t block100 = calc.getBlockSubsidy(100);
    assert_eq(block100, 100ULL * 100000000ULL, "Block 100 should have 100 DIN");

    uint64_t block209999 = calc.getBlockSubsidy(209999);
    assert_eq(block209999, 100ULL * 100000000ULL, "Block 209999 should have 100 DIN");

    std::cout << "  ✅ Initial subsidy test passed" << std::endl;
}

// ============================================================================
// Test 3: First Halving
// ============================================================================

void test_first_halving() {
    std::cout << "Running test_first_halving..." << std::endl;

    ConsensusParams params = ConsensusParams::mainnet();
    ConsensusSubsidyCalculator calc(params);

    // Block at first halving (210000) should have half subsidy
    uint64_t halving1 = calc.getBlockSubsidy(210000);
    assert_eq(halving1, 50ULL * 100000000ULL, "First halving should be 50 DIN");

    // Check halving detection
    assert_true(calc.isHalvingHeight(210000), "210000 should be halving height");
    assert_true(!calc.isHalvingHeight(210001), "210001 should not be halving height");

    // Check halvings count
    assert_eq(calc.getHalvingsAt(210000), uint32_t(1), "Should have 1 halving at 210000");

    std::cout << "  ✅ First halving test passed" << std::endl;
}

// ============================================================================
// Test 4: Second Halving
// ============================================================================

void test_second_halving() {
    std::cout << "Running test_second_halving..." << std::endl;

    ConsensusParams params = ConsensusParams::mainnet();
    ConsensusSubsidyCalculator calc(params);

    // Block at second halving (420000) should be 25 DIN
    uint64_t halving2 = calc.getBlockSubsidy(420000);
    assert_eq(halving2, 25ULL * 100000000ULL, "Second halving should be 25 DIN");

    // Check halvings count
    assert_eq(calc.getHalvingsAt(420000), uint32_t(2), "Should have 2 halvings at 420000");

    std::cout << "  ✅ Second halving test passed" << std::endl;
}

// ============================================================================
// Test 5: Multiple Halvings
// ============================================================================

void test_multiple_halvings() {
    std::cout << "Running test_multiple_halvings..." << std::endl;

    ConsensusParams params = ConsensusParams::mainnet();
    ConsensusSubsidyCalculator calc(params);

    // Test several halvings, starting from block 1 (not genesis)
    for (uint32_t i = 0; i < 10; i++) {
        uint32_t height = i * params.halving_interval;

        // Skip genesis (special case)
        if (height == 0) {
            height = 1;
        }

        // Calculate expected subsidy for this halving period
        uint64_t expected = params.initial_subsidy >> i;

        uint64_t subsidy = calc.getBlockSubsidy(height);
        assert_eq(subsidy, expected, "Halving " + std::to_string(i) + " subsidy mismatch");
    }

    std::cout << "  ✅ Multiple halvings test passed" << std::endl;
}

// ============================================================================
// Test 6: Subsidy Becomes Zero After 64 Halvings
// ============================================================================

void test_zero_subsidy() {
    std::cout << "Running test_zero_subsidy..." << std::endl;

    ConsensusParams params = ConsensusParams::mainnet();
    ConsensusSubsidyCalculator calc(params);

    // After 64 halvings, subsidy should be 0
    uint32_t height_64_halvings = 64 * params.halving_interval;
    uint64_t subsidy_64 = calc.getBlockSubsidy(height_64_halvings);
    assert_eq(subsidy_64, uint64_t(0), "Subsidy should be 0 after 64 halvings");

    // Check even higher heights
    uint64_t subsidy_100 = calc.getBlockSubsidy(height_64_halvings + 1000000);
    assert_eq(subsidy_100, uint64_t(0), "Subsidy should remain 0 after 64 halvings");

    std::cout << "  ✅ Zero subsidy test passed" << std::endl;
}

// ============================================================================
// Test 7: Regtest Parameters
// ============================================================================

void test_regtest_params() {
    std::cout << "Running test_regtest_params..." << std::endl;

    ConsensusParams params = ConsensusParams::regtest();
    ConsensusSubsidyCalculator calc(params);

    // Regtest has faster halving (150 blocks)
    assert_eq(calc.getHalvingInterval(), uint32_t(150), "Regtest halving interval should be 150");

    // First halving at 150
    uint64_t pre_halving = calc.getBlockSubsidy(149);
    assert_eq(pre_halving, 100ULL * 100000000ULL, "Pre-halving should be 100 DIN");

    uint64_t post_halving = calc.getBlockSubsidy(150);
    assert_eq(post_halving, 50ULL * 100000000ULL, "Post-halving should be 50 DIN");

    std::cout << "  ✅ Regtest params test passed" << std::endl;
}

// ============================================================================
// Test 8: Halving Boundary Detection
// ============================================================================

void test_halving_boundaries() {
    std::cout << "Running test_halving_boundaries..." << std::endl;

    ConsensusParams params = ConsensusParams::mainnet();
    ConsensusSubsidyCalculator calc(params);

    // Test boundary detection
    assert_true(!calc.isHalvingHeight(0), "Genesis is not a halving");
    assert_true(!calc.isHalvingHeight(1), "Block 1 is not a halving");
    assert_true(!calc.isHalvingHeight(209999), "Block before halving is not halving");
    assert_true(calc.isHalvingHeight(210000), "Block 210000 is halving");
    assert_true(!calc.isHalvingHeight(210001), "Block after halving is not halving");
    assert_true(calc.isHalvingHeight(420000), "Block 420000 is halving");
    assert_true(calc.isHalvingHeight(630000), "Block 630000 is halving");

    std::cout << "  ✅ Halving boundary detection test passed" << std::endl;
}

// ============================================================================
// Test 9: ConsensusParams Equality
// ============================================================================

void test_consensus_params_equality() {
    std::cout << "Running test_consensus_params_equality..." << std::endl;

    ConsensusParams mainnet1 = ConsensusParams::mainnet();
    ConsensusParams mainnet2 = ConsensusParams::mainnet();
    ConsensusParams testnet = ConsensusParams::testnet();
    ConsensusParams regtest = ConsensusParams::regtest();

    assert_true(mainnet1 == mainnet2, "Same network params should be equal");
    assert_true(mainnet1 == testnet, "Mainnet and testnet currently have same params");
    assert_true(!(mainnet1 == regtest), "Mainnet and regtest should differ");

    std::cout << "  ✅ ConsensusParams equality test passed" << std::endl;
}

// ============================================================================
// Test 10: Edge Cases
// ============================================================================

void test_edge_cases() {
    std::cout << "Running test_edge_cases..." << std::endl;

    ConsensusParams params = ConsensusParams::mainnet();
    ConsensusSubsidyCalculator calc(params);

    // Test height = 1 (first real block)
    uint64_t block1 = calc.getBlockSubsidy(1);
    assert_eq(block1, 100ULL * 100000000ULL, "Block 1 should have full subsidy");

    // Test just before first halving
    uint64_t block_before = calc.getBlockSubsidy(params.halving_interval - 1);
    assert_eq(block_before, 100ULL * 100000000ULL, "Block before halving should have full subsidy");

    // Test at halving
    uint64_t block_at = calc.getBlockSubsidy(params.halving_interval);
    assert_eq(block_at, 50ULL * 100000000ULL, "Block at halving should have half subsidy");

    // Test just after halving
    uint64_t block_after = calc.getBlockSubsidy(params.halving_interval + 1);
    assert_eq(block_after, 50ULL * 100000000ULL, "Block after halving should have half subsidy");

    std::cout << "  ✅ Edge cases test passed" << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
    std::cout << "Ring 4 Phase 4c: Subsidy Calculator Tests" << std::endl;
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n" << std::endl;

    try {
        test_genesis_subsidy();
        test_initial_subsidy();
        test_first_halving();
        test_second_halving();
        test_multiple_halvings();
        test_zero_subsidy();
        test_regtest_params();
        test_halving_boundaries();
        test_consensus_params_equality();
        test_edge_cases();

        std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━" << std::endl;
        std::cout << "✅ ALL TESTS PASSED (10/10)" << std::endl;
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
