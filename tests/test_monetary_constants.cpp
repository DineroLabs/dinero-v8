// Phase D.2: Invariant Tests - Monetary Constants
//
// Verifies monetary constants (MAX_MONEY, supply, subsidy, tail emission)
// are correctly defined.

#include "consensus/tx_validation.h"
#include "consensus/subsidy.h"
#include <cassert>
#include <iostream>
#include <string>
#include <cstdint>

using namespace dinero;
using namespace dinero::consensus;

int main() {
    std::cout << "Phase D.2: Monetary Constants Invariant Tests\n";
    std::cout << "=============================================\n\n";

    int passed = 0;
    int failed = 0;

    // ========================================================================
    // Test 1: MAX_MONEY verification
    // ========================================================================
    {
        std::cout << "Test 1: MAX_MONEY verification\n";

        bool test_ok = true;

        // MAX_MONEY should be large enough to accommodate tail emission
        // With tail emission there's no hard cap, but MAX_MONEY bounds
        // individual transaction amounts for anti-DoS
        if (MAX_MONEY == 0) {
            std::cout << "  FAIL: MAX_MONEY must not be zero\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  PASS: MAX_MONEY = " << MAX_MONEY << " una\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 2: IsValidAmount() validation
    // ========================================================================
    {
        std::cout << "\nTest 2: IsValidAmount() validation\n";

        bool test_ok = true;

        if (!IsValidAmount(1)) {
            std::cout << "  FAIL: 1 una should be valid\n";
            test_ok = false;
        }

        if (!IsValidAmount(MAX_MONEY)) {
            std::cout << "  FAIL: MAX_MONEY should be valid\n";
            test_ok = false;
        }

        if (IsValidAmount(0)) {
            std::cout << "  FAIL: 0 should be invalid (no zero amounts)\n";
            test_ok = false;
        }

        if (IsValidAmount(MAX_MONEY + 1)) {
            std::cout << "  FAIL: MAX_MONEY + 1 should be invalid\n";
            test_ok = false;
        }

        if (IsValidAmount(UINT64_MAX)) {
            std::cout << "  FAIL: UINT64_MAX should be invalid\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  PASS: IsValidAmount() correct\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 3: IsValidAmountOrZero() validation
    // ========================================================================
    {
        std::cout << "\nTest 3: IsValidAmountOrZero() validation\n";

        bool test_ok = true;

        if (!IsValidAmountOrZero(0)) {
            std::cout << "  FAIL: 0 should be valid (zero allowed)\n";
            test_ok = false;
        }

        if (!IsValidAmountOrZero(1)) {
            std::cout << "  FAIL: 1 una should be valid\n";
            test_ok = false;
        }

        if (!IsValidAmountOrZero(MAX_MONEY)) {
            std::cout << "  FAIL: MAX_MONEY should be valid\n";
            test_ok = false;
        }

        if (IsValidAmountOrZero(MAX_MONEY + 1)) {
            std::cout << "  FAIL: MAX_MONEY + 1 should be invalid\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  PASS: IsValidAmountOrZero() correct\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 4: Initial subsidy validation
    // ========================================================================
    {
        std::cout << "\nTest 4: Initial subsidy validation\n";

        bool test_ok = true;

        constexpr uint64_t EXPECTED_INITIAL_SUBSIDY = 10000000000ULL;  // 100 DIN

        if (ConsensusSubsidy::INITIAL_SUBSIDY != EXPECTED_INITIAL_SUBSIDY) {
            std::cout << "  FAIL: INITIAL_SUBSIDY != 100 DIN\n";
            test_ok = false;
        }

        if (ConsensusSubsidy::INITIAL_SUBSIDY != 100 * ConsensusSubsidy::UNA_PER_DIN) {
            std::cout << "  FAIL: INITIAL_SUBSIDY != 100 * UNA_PER_DIN\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  PASS: Initial subsidy = 100 DIN\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 5: Halving interval validation
    // ========================================================================
    {
        std::cout << "\nTest 5: Halving interval validation\n";

        bool test_ok = true;

        constexpr uint32_t EXPECTED_HALVING = 1314000;

        if (ConsensusSubsidy::HALVING_INTERVAL != EXPECTED_HALVING) {
            std::cout << "  FAIL: HALVING_INTERVAL != 1,314,000\n";
            test_ok = false;
        }

        // ~5 years @ 2 min blocks
        constexpr uint32_t FIVE_YEARS_BLOCKS = (365 * 5 * 24 * 60) / 2;

        if (ConsensusSubsidy::HALVING_INTERVAL < FIVE_YEARS_BLOCKS - 10000 ||
            ConsensusSubsidy::HALVING_INTERVAL > FIVE_YEARS_BLOCKS + 10000) {
            std::cout << "  FAIL: HALVING_INTERVAL not close to 5 years\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  PASS: Halving interval = 1,314,000 blocks (~5 years)\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 6: Subsidy schedule with tail emission
    // ========================================================================
    {
        std::cout << "\nTest 6: Subsidy schedule with tail emission\n";

        bool test_ok = true;

        // Height 0: Genesis (0 DIN spendable)
        if (ConsensusSubsidy::GetBlockSubsidy(0) != AmountUna::Zero()) {
            std::cout << "  FAIL: Genesis subsidy should be 0\n";
            test_ok = false;
        }

        // Height 1: First PoW block (100 DIN)
        if (ConsensusSubsidy::GetBlockSubsidy(1) != AmountUna::Una(ConsensusSubsidy::INITIAL_SUBSIDY)) {
            std::cout << "  FAIL: Height 1 subsidy should be 100 DIN\n";
            test_ok = false;
        }

        // First halving: height 1 + 1,314,000 = 1,314,001 (50 DIN)
        uint32_t first_halving_height = 1 + ConsensusSubsidy::HALVING_INTERVAL;
        uint64_t halved_subsidy = ConsensusSubsidy::GetBlockSubsidy(first_halving_height).GetUna();

        if (halved_subsidy != ConsensusSubsidy::INITIAL_SUBSIDY / 2) {
            std::cout << "  FAIL: First halving should be 50 DIN\n";
            std::cout << "     Expected: " << (ConsensusSubsidy::INITIAL_SUBSIDY / 2) << "\n";
            std::cout << "     Got:      " << halved_subsidy << "\n";
            test_ok = false;
        }

        // After many halvings: tail emission (1 DIN forever)
        uint32_t far_future = 1 + (33 * ConsensusSubsidy::HALVING_INTERVAL);
        uint64_t far_subsidy = ConsensusSubsidy::GetBlockSubsidy(far_future).GetUna();
        if (far_subsidy != ConsensusSubsidy::TAIL_EMISSION_UNA) {
            std::cout << "  FAIL: After 33 halvings, subsidy should be tail emission (1 DIN)\n";
            std::cout << "     Expected: " << ConsensusSubsidy::TAIL_EMISSION_UNA << "\n";
            std::cout << "     Got:      " << far_subsidy << "\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  PASS: Subsidy schedule correct\n";
            std::cout << "          Genesis: 0 DIN\n";
            std::cout << "          Height 1+: 100 DIN -> halvings every 1,314,000 blocks\n";
            std::cout << "          Tail: 1 DIN/block forever\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 7: COINBASE_MATURITY validation
    // ========================================================================
    {
        std::cout << "\nTest 7: COINBASE_MATURITY validation\n";

        bool test_ok = true;

        if (COINBASE_MATURITY != 100) {
            std::cout << "  FAIL: COINBASE_MATURITY != 100\n";
            test_ok = false;
        }

        if (test_ok) {
            std::cout << "  PASS: COINBASE_MATURITY = 100 blocks\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Test 8: Supply calculation (no overflow with tail emission)
    // ========================================================================
    {
        std::cout << "\nTest 8: Supply calculation integrity\n";

        bool test_ok = true;

        // Sum halving-era PoW subsidies
        uint64_t pow_supply = 0;
        uint64_t subsidy = ConsensusSubsidy::INITIAL_SUBSIDY;

        for (uint32_t halving = 0; halving < 64 && subsidy > 0; halving++) {
            uint64_t blocks_this_halving = ConsensusSubsidy::HALVING_INTERVAL;
            uint64_t supply_this_halving = blocks_this_halving * subsidy;

            if (pow_supply + supply_this_halving < pow_supply) {
                std::cout << "  FAIL: PoW supply calculation overflowed!\n";
                test_ok = false;
                break;
            }

            pow_supply += supply_this_halving;
            subsidy >>= 1;
        }

        if (test_ok) {
            std::cout << "  PASS: Supply calculation does not overflow\n";
            std::cout << "          Halving-era supply: " << pow_supply << " una ("
                      << (pow_supply / ConsensusSubsidy::UNA_PER_DIN) << " DIN)\n";
            std::cout << "          + tail emission of 1 DIN/block forever (disinflationary)\n";
            passed++;
        } else {
            failed++;
        }
    }

    // ========================================================================
    // Summary
    // ========================================================================
    std::cout << "\n=============================================\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed\n";

    if (failed > 0) {
        std::cout << "CRITICAL: Monetary constants tests FAILED!\n";
        return 1;
    }

    std::cout << "All monetary constants tests PASSED!\n";
    std::cout << "   Dinero supply schedule:\n";
    std::cout << "   - Initial subsidy: 100 DIN/block\n";
    std::cout << "   - Halving: Every 1,314,000 blocks (~5 years)\n";
    std::cout << "   - Tail emission: 1 DIN/block forever\n";
    std::cout << "   - No premine. No hard cap. Fair launch.\n";
    return 0;
}
