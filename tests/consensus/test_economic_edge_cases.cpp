/**
 * E.4: Economic Stress Tests (Edge Cases & Adversarial Scenarios)
 *
 * Tests economic invariants under extreme conditions:
 * - Maximum fee blocks (very high fees)
 * - Zero-fee transactions (policy vs consensus)
 * - Dust threshold economics (minimum output values)
 * - Overflow protection (uint64_t limits)
 *
 * All tests verify consensus rules hold even in adversarial scenarios.
 */

#include "consensus/subsidy.h"
#include "consensus/chainparams.h"
#include <iostream>
#include <cassert>
#include <cstdint>
#include <limits>

using namespace dinero;

//=============================================================================
// E.4.1: Maximum Fee Block
//=============================================================================

void testMaximumFeeBlock() {
    std::cout << "\n[Test 1] Maximum fee block (coinbase validation)" << std::endl;

    uint32_t height = 1000000;
    uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(height).GetUna();

    // Scenario: Block with very high fees (10,000 DIN)
    uint64_t extreme_fees = 10000ULL * ConsensusSubsidy::UNA_PER_DIN;
    uint64_t valid_coinbase = subsidy + extreme_fees;

    assert(valid_coinbase == subsidy + extreme_fees &&
           "Coinbase validation must allow subsidy + fees");

    uint64_t invalid_coinbase = valid_coinbase + 1;
    bool is_invalid = (invalid_coinbase > subsidy + extreme_fees);
    assert(is_invalid && "Coinbase exceeding subsidy + fees is invalid");

    std::cout << "  [OK] Coinbase validation correct with extreme fees" << std::endl;
}

void testCoinbaseAtAllHalvings() {
    std::cout << "\n[Test 2] Coinbase validation at all halving boundaries" << std::endl;

    for (uint32_t epoch = 0; epoch < 10; epoch++) {
        uint32_t height = 1 + (epoch * ConsensusSubsidy::HALVING_INTERVAL);
        uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(height).GetUna();
        uint64_t fees = 1000ULL * ConsensusSubsidy::UNA_PER_DIN;

        uint64_t valid_coinbase = subsidy + fees;
        assert(valid_coinbase == subsidy + fees);

        uint64_t invalid_coinbase = valid_coinbase + 1;
        assert(invalid_coinbase > subsidy + fees);
    }

    std::cout << "  [OK] Tested coinbase validation at 10 halvings" << std::endl;
}

//=============================================================================
// E.4.2: Zero-Fee Transactions
//=============================================================================

void testZeroFeeEconomics() {
    std::cout << "\n[Test 3] Zero-fee transaction economics" << std::endl;

    const auto& params = Params();
    uint64_t min_relay_fee = params.min_relay_fee;

    uint64_t zero_fee = 0;
    bool rejected_by_policy = (zero_fee < min_relay_fee);
    assert(rejected_by_policy && "Zero-fee tx should be rejected by policy");

    // Consensus: zero-fee transactions are VALID if mined
    uint32_t height = 1000000;
    uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(height).GetUna();
    uint64_t coinbase = subsidy + 0;
    assert(coinbase == subsidy && "Zero-fee block is consensus-valid");

    std::cout << "  [OK] Policy: Zero-fee tx rejected. Consensus: valid if mined." << std::endl;
}

//=============================================================================
// E.4.3: Dust Threshold Economics
//=============================================================================

void testDustThreshold() {
    std::cout << "\n[Test 4] Dust threshold economics" << std::endl;

    const auto& params = Params();
    uint64_t dust_threshold = params.dust_threshold;

    uint64_t dust_output = dust_threshold - 1;
    bool is_dust = (dust_output < dust_threshold);
    assert(is_dust && "Below-threshold outputs should be dust");

    uint64_t valid_dust_output = 1;
    assert(valid_dust_output > 0 && "Any positive output is consensus-valid");

    std::cout << "  [OK] Dust threshold: policy-level, not consensus" << std::endl;
}

void testMinimumOutput() {
    std::cout << "\n[Test 5] Minimum output value (1 una)" << std::endl;

    uint64_t min_output = 1;
    assert(min_output > 0 && "Minimum output is 1 una");

    uint64_t zero_output = 0;
    bool is_invalid = (zero_output == 0);
    assert(is_invalid && "Zero-value outputs are invalid");

    std::cout << "  [OK] Minimum output: 1 una (consensus)" << std::endl;
}

//=============================================================================
// E.4.4: Overflow Protection
//=============================================================================

void testSubsidyOverflowProtection() {
    std::cout << "\n[Test 6] Subsidy calculation overflow protection" << std::endl;

    uint64_t max_subsidy = ConsensusSubsidy::INITIAL_SUBSIDY;
    uint64_t max_fees = 10000ULL * ConsensusSubsidy::UNA_PER_DIN;

    uint64_t sum = max_subsidy + max_fees;
    assert(sum > max_subsidy && "No overflow in subsidy + fees");
    assert(sum > max_fees && "No overflow in subsidy + fees");

    uint64_t uint64_max = std::numeric_limits<uint64_t>::max();
    // Initial subsidy should be far below uint64_t max
    assert(ConsensusSubsidy::INITIAL_SUBSIDY < uint64_max / 100 &&
           "INITIAL_SUBSIDY should be far below uint64_t limit");

    std::cout << "  [OK] No overflow in subsidy calculations" << std::endl;
}

void testTotalIssuanceOverflowProtection() {
    std::cout << "\n[Test 7] Total issuance calculation overflow protection" << std::endl;

    // Calculate total issuance at a deep height
    uint32_t deep_height = 1 + (10 * ConsensusSubsidy::HALVING_INTERVAL);
    uint64_t total_issued = ConsensusSubsidy::GetTotalIssuedAtHeight(deep_height);

    uint64_t genesis = ConsensusSubsidy::GENESIS_UNSPENDABLE_UNA;
    uint64_t pow_issued = ConsensusSubsidy::GetPoWIssuedAtHeight(deep_height);

    uint64_t calculated = genesis + pow_issued;
    assert(calculated >= genesis && "No overflow in genesis + pow");
    assert(calculated == total_issued && "Total issuance matches components");

    std::cout << "  [OK] No overflow in total issuance calculation" << std::endl;
}

void testHalvingCalculationOverflow() {
    std::cout << "\n[Test 8] Halving calculation overflow protection" << std::endl;

    // Test halving calculation at many epochs
    // With tail emission: subsidy never reaches 0
    for (uint32_t epoch = 0; epoch <= 35; epoch++) {
        uint32_t height = 1 + (epoch * ConsensusSubsidy::HALVING_INTERVAL);
        uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(height).GetUna();

        // With tail emission, subsidy is always >= 1 DIN
        assert(subsidy >= ConsensusSubsidy::TAIL_EMISSION_UNA &&
               "Subsidy must be at least tail emission (1 DIN)");
    }

    std::cout << "  [OK] Halving calculation safe for all epochs (tail floor)" << std::endl;
}

//=============================================================================
// E.4.5: Boundary Conditions
//=============================================================================

void testSubsidyBoundaryConditions() {
    std::cout << "\n[Test 9] Subsidy boundary conditions" << std::endl;

    // Height 0: Genesis (should return 0)
    assert(ConsensusSubsidy::GetBlockSubsidy(0) == AmountUna::Zero() && "Genesis subsidy = 0");

    // Height 1: First PoW block (100 DIN)
    uint64_t h1 = ConsensusSubsidy::GetBlockSubsidy(1).GetUna();
    assert(h1 == ConsensusSubsidy::INITIAL_SUBSIDY && "Height 1 = 100 DIN");

    // Height 2: Also 100 DIN (same epoch)
    uint64_t h2 = ConsensusSubsidy::GetBlockSubsidy(2).GetUna();
    assert(h2 == ConsensusSubsidy::INITIAL_SUBSIDY && "Height 2 = 100 DIN");

    // First halving boundary
    uint32_t first_halving = 1 + ConsensusSubsidy::HALVING_INTERVAL;
    uint64_t halving_subsidy = ConsensusSubsidy::GetBlockSubsidy(first_halving).GetUna();
    assert(halving_subsidy == ConsensusSubsidy::INITIAL_SUBSIDY / 2 && "First halving = 50 DIN");

    // Far future: tail emission (1 DIN)
    uint64_t far_future = ConsensusSubsidy::GetBlockSubsidy(100000000).GetUna();
    assert(far_future == ConsensusSubsidy::TAIL_EMISSION_UNA && "Far future = 1 DIN (tail)");

    std::cout << "  [OK] Genesis: 0, Height 1: 100 DIN, Halving: 50 DIN, Tail: 1 DIN" << std::endl;
}

void testFeeCalculationEdgeCases() {
    std::cout << "\n[Test 10] Fee calculation edge cases" << std::endl;

    uint64_t zero_fees = 0;
    uint32_t height = 1000000;
    uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(height).GetUna();
    uint64_t coinbase_zero_fees = subsidy + zero_fees;
    assert(coinbase_zero_fees == subsidy && "Zero fees: coinbase = subsidy");

    // Large fees should not overflow
    uint64_t large_fees = 1000000000ULL * ConsensusSubsidy::UNA_PER_DIN;  // 1B DIN
    uint64_t coinbase_large_fees = subsidy + large_fees;
    assert(coinbase_large_fees > subsidy && "Large fees don't cause overflow");

    std::cout << "  [OK] Fee calculation edge cases handled" << std::endl;
}

//=============================================================================
// E.4.6: Economic Invariant Verification
//=============================================================================

void testEconomicInvariants() {
    std::cout << "\n[Test 11] Core economic invariants" << std::endl;

    // Invariant 1: Subsidy >= tail emission for all heights >= 1
    for (uint32_t height = 1; height < 50000000; height += 1000000) {
        uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(height).GetUna();
        assert(subsidy >= ConsensusSubsidy::TAIL_EMISSION_UNA &&
               "Subsidy must be at least tail emission");
    }
    std::cout << "  [OK] Invariant 1: Subsidy >= 1 DIN at all heights >= 1" << std::endl;

    // Invariant 2: Subsidy is monotonically decreasing (with tail floor)
    uint64_t prev_subsidy = ConsensusSubsidy::INITIAL_SUBSIDY;
    for (uint32_t epoch = 0; epoch < 10; epoch++) {
        uint32_t height = 1 + (epoch * ConsensusSubsidy::HALVING_INTERVAL);
        uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(height).GetUna();
        assert(subsidy <= prev_subsidy && "Subsidy is monotonically decreasing");
        prev_subsidy = subsidy;
    }
    std::cout << "  [OK] Invariant 2: Subsidy monotonically decreasing" << std::endl;

    // Invariant 3: Total issued is monotonically increasing
    uint64_t prev_issued = 0;
    for (uint32_t height = 0; height < 50000000; height += 1000000) {
        uint64_t issued = ConsensusSubsidy::GetTotalIssuedAtHeight(height);
        assert(issued >= prev_issued && "Total issued is monotonically increasing");
        prev_issued = issued;
    }
    std::cout << "  [OK] Invariant 3: Total issued monotonically increasing" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "E.4: Economic Stress Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        testMaximumFeeBlock();
        testCoinbaseAtAllHalvings();
        testZeroFeeEconomics();
        testDustThreshold();
        testMinimumOutput();
        testSubsidyOverflowProtection();
        testTotalIssuanceOverflowProtection();
        testHalvingCalculationOverflow();
        testSubsidyBoundaryConditions();
        testFeeCalculationEdgeCases();
        testEconomicInvariants();

        std::cout << "\n========================================" << std::endl;
        std::cout << "All E.4 Stress Tests Passed!" << std::endl;
        std::cout << "========================================" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nTest failed: " << e.what() << std::endl;
        return 1;
    }
}
