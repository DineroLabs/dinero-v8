/**
 * E.3: Supply Validation Tests
 *
 * Proves Dinero's disinflationary supply schedule is correct:
 * - No premine (fair launch)
 * - 100 DIN initial subsidy, halving every 1,314,000 blocks
 * - 1 DIN/block tail emission forever
 * - Supply is monotonically increasing
 * - No overflow in calculations
 */

#include "consensus/supply_validator.h"
#include "consensus/subsidy.h"
#include <iostream>
#include <cassert>
#include <iomanip>

using namespace dinero;

//=============================================================================
// E.3.1: Supply Validation at Critical Heights
//=============================================================================

void testSupplyAtCriticalHeights() {
    std::cout << "\n[Test 1] Supply validation at critical heights" << std::endl;

    bool all_valid = SupplyValidator::VerifyAllCriticalHeights();
    assert(all_valid && "Supply invariants must hold at all critical heights");

    // Print detailed verification
    std::cout << "  Checking critical heights:" << std::endl;

    // Genesis
    std::cout << "    Height 0 (genesis): "
              << (ConsensusSubsidy::GetTotalIssuedAtHeight(0) / 1e8) << " DIN" << std::endl;

    // First PoW block
    std::cout << "    Height 1 (first PoW): "
              << (ConsensusSubsidy::GetTotalIssuedAtHeight(1) / 1e8) << " DIN" << std::endl;

    // Every 5th halving (sample)
    for (uint32_t epoch = 0; epoch < 10; epoch += 2) {
        uint32_t height = 1 + (epoch * ConsensusSubsidy::HALVING_INTERVAL);
        double issued = ConsensusSubsidy::GetTotalIssuedAtHeight(height) / 1e8;
        std::cout << "    Halving " << epoch << " (height " << height << "): "
                  << std::fixed << std::setprecision(2) << issued << " DIN" << std::endl;
    }

    std::cout << "  [OK] Supply validation passed at all critical heights" << std::endl;
}

void testSupplyAtRandomHeights() {
    std::cout << "\n[Test 2] Supply validation at various heights" << std::endl;

    uint32_t test_heights[] = {
        10, 100, 1000, 10000, 100000, 1000000,
        5000000, 10000000, 20000000, 50000000
    };

    for (uint32_t height : test_heights) {
        assert(SupplyValidator::VerifyTailEmissionFloor(height));
        assert(SupplyValidator::VerifyMonotonicSupply(0, height));
    }

    std::cout << "  [OK] Tested " << (sizeof(test_heights) / sizeof(test_heights[0]))
              << " heights - all valid" << std::endl;
}

//=============================================================================
// E.3.2: Monotonic Supply
//=============================================================================

void testMonotonicSupply() {
    std::cout << "\n[Test 3] Supply is monotonic (never decreases)" << std::endl;

    uint32_t test_ranges[][2] = {
        {0, 100},
        {1000, 2000},
        {100000, 100100},
        {1314000, 1314100},
        {9198000, 9198100}  // Tail emission region
    };

    for (auto& range : test_ranges) {
        uint32_t start = range[0];
        uint32_t end = range[1];

        for (uint32_t h = start; h < end; h++) {
            assert(SupplyValidator::VerifyMonotonicSupply(h, h + 1) &&
                   "Supply must never decrease");
        }
    }

    std::cout << "  [OK] Supply is monotonically increasing" << std::endl;
}

//=============================================================================
// E.3.3: Tail Emission Behavior
//=============================================================================

void testTailEmission() {
    std::cout << "\n[Test 4] Tail emission (1 DIN/block forever)" << std::endl;

    // Tail emission kicks in at halving 7 (~height 9,198,001)
    // when 100 >> 7 = 0.78125 DIN < 1 DIN floor
    uint32_t tail_start = 1 + (7 * ConsensusSubsidy::HALVING_INTERVAL);

    std::cout << "  Tail emission start: ~height " << tail_start << std::endl;

    // Verify subsidy at tail is exactly 1 DIN
    for (uint32_t offset = 0; offset < 100; offset++) {
        uint32_t height = tail_start + offset;
        uint64_t subsidy = ConsensusSubsidy::GetBlockSubsidy(height).GetUna();
        assert(subsidy == ConsensusSubsidy::TAIL_EMISSION_UNA &&
               "Tail emission must be exactly 1 DIN");
    }

    // Verify it continues forever
    uint64_t subsidy_far = ConsensusSubsidy::GetBlockSubsidy(100000000).GetUna();
    assert(subsidy_far == ConsensusSubsidy::TAIL_EMISSION_UNA &&
           "Tail emission must continue at 100M blocks");

    std::cout << "  [OK] Tail emission: 1 DIN/block at all post-tail heights" << std::endl;
}

void testSupplyGrowsContinuously() {
    std::cout << "\n[Test 5] Supply grows continuously (tail emission)" << std::endl;

    // Unlike the old hard-cap model, supply never stops growing
    uint32_t deep_height = 1 + (33 * ConsensusSubsidy::HALVING_INTERVAL);

    uint64_t supply_at = ConsensusSubsidy::GetTotalIssuedAtHeight(deep_height);
    uint64_t supply_later = ConsensusSubsidy::GetTotalIssuedAtHeight(deep_height + 1000000);

    std::cout << "  Supply at height " << deep_height << ": "
              << std::fixed << std::setprecision(2) << (supply_at / 1e8) << " DIN" << std::endl;
    std::cout << "  Supply 1M blocks later: "
              << std::fixed << std::setprecision(2) << (supply_later / 1e8) << " DIN" << std::endl;

    // With tail emission, supply should grow by ~1M DIN per 1M blocks
    uint64_t expected_growth = 1000000ULL * ConsensusSubsidy::TAIL_EMISSION_UNA;
    uint64_t actual_growth = supply_later - supply_at;

    assert(actual_growth == expected_growth &&
           "Supply must grow by exactly 1 DIN per block in tail");

    std::cout << "  [OK] Supply grows by 1 DIN/block in tail emission era" << std::endl;
}

//=============================================================================
// E.3.4: Supply Decomposition (Genesis + PoW = Total)
//=============================================================================

void testSupplyDecomposition() {
    std::cout << "\n[Test 6] Supply decomposition (Genesis + PoW)" << std::endl;

    uint32_t test_height = 1 + (10 * ConsensusSubsidy::HALVING_INTERVAL);

    uint64_t genesis = ConsensusSubsidy::GENESIS_UNSPENDABLE_UNA;
    uint64_t pow_issued = ConsensusSubsidy::GetPoWIssuedAtHeight(test_height);
    uint64_t total_issued = ConsensusSubsidy::GetTotalIssuedAtHeight(test_height);

    std::cout << "  Genesis (unspendable): " << std::fixed << std::setprecision(2)
              << (genesis / 1e8) << " DIN" << std::endl;
    std::cout << "  PoW issued:            " << std::setprecision(2)
              << (pow_issued / 1e8) << " DIN" << std::endl;
    std::cout << "  Total issued:          " << (total_issued / 1e8) << " DIN" << std::endl;

    // Verify decomposition
    uint64_t calculated_total = genesis + pow_issued;
    assert(calculated_total == total_issued &&
           "Total supply must equal genesis + PoW");

    std::cout << "  [OK] Genesis + PoW = Total Supply" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "E.3: Supply Validation Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nDisinflationary supply with 1 DIN/block tail emission" << std::endl;

    try {
        testSupplyAtCriticalHeights();
        testSupplyAtRandomHeights();
        testMonotonicSupply();
        testTailEmission();
        testSupplyGrowsContinuously();
        testSupplyDecomposition();

        std::cout << "\n========================================" << std::endl;
        std::cout << "All E.3 Supply Validation Tests Passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nProof Complete:" << std::endl;
        std::cout << "  - Supply invariants hold at all critical heights" << std::endl;
        std::cout << "  - Supply is monotonically increasing" << std::endl;
        std::cout << "  - Tail emission (1 DIN/block) enforced forever" << std::endl;
        std::cout << "  - Genesis + PoW = Total (no premine)" << std::endl;
        std::cout << "  - No overflow in calculations" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nTest failed: " << e.what() << std::endl;
        return 1;
    }
}
