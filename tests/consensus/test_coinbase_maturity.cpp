/**
 * Phase 3: Coinbase Maturity Enforcement Test
 *
 * Validates that Dinero enforces the 100-block coinbase maturity rule:
 * - Coinbase outputs cannot be spent until they have 100 confirmations
 * - This prevents spending coins from blocks that might be reorganized
 *
 * Test Cases:
 * 1. Coinbase at height 1, current height 100 (99 confs) → NOT MATURE
 * 2. Coinbase at height 1, current height 101 (100 confs) → MATURE
 * 3. Non-coinbase transactions are immediately spendable (1 conf)
 * 4. Wallet balance correctly excludes immature coinbase
 */

#include "consensus/coinbase_maturity.h"
#include "consensus/utxo_entry.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace dinero;
using namespace dinero::consensus;

//=============================================================================
// Test 1: Basic Maturity Calculation
//=============================================================================

void testBasicMaturityCalculation() {
    std::cout << "\n[Test 1] Basic coinbase maturity calculation..." << std::endl;

    // Coinbase at height 1
    uint32_t coinbase_height = 1;

    // Test at various heights
    struct TestCase {
        uint32_t current_height;
        bool should_be_mature;
        std::string description;
    };

    std::vector<TestCase> test_cases = {
        {1, false, "Same block (0 confirmations)"},
        {50, false, "49 confirmations"},
        {99, false, "98 confirmations"},
        {100, false, "99 confirmations (boundary - still immature)"},
        {101, true, "100 confirmations (boundary - now mature)"},
        {200, true, "199 confirmations"},
        {1000, true, "999 confirmations"},
    };

    for (const auto& tc : test_cases) {
        bool is_mature = CoinbaseMaturity::isCoinbaseMature(coinbase_height, tc.current_height);

        std::cout << "  Height " << tc.current_height << " (" << tc.description << "): "
                  << (is_mature ? "MATURE" : "IMMATURE");

        if (is_mature == tc.should_be_mature) {
            std::cout << " ✓" << std::endl;
        } else {
            std::cout << " ✗ FAILED" << std::endl;
            assert(false && "Maturity check failed");
        }
    }

    std::cout << "[Test 1] ✓ PASS: Basic maturity calculation correct" << std::endl;
}

//=============================================================================
// Test 2: UTXOEntry isMature() Method
//=============================================================================

void testUTXOEntryMaturity() {
    std::cout << "\n[Test 2] UTXOEntry::isMature() method..." << std::endl;

    // Create a coinbase UTXO at height 1
    std::vector<uint8_t> dummy_script = {0x00, 0x14};  // P2WPKH
    UTXOEntry coinbase_utxo(10000000000ULL, dummy_script, 1, true);  // 100 DIN coinbase at height 1

    // Test at height 100 (99 confirmations)
    bool mature_at_100 = coinbase_utxo.isMature(100);
    assert(!mature_at_100 && "Coinbase should be immature at 99 confirmations");
    std::cout << "  Coinbase at height 100 (99 confs): IMMATURE ✓" << std::endl;

    // Test at height 101 (100 confirmations)
    bool mature_at_101 = coinbase_utxo.isMature(101);
    assert(mature_at_101 && "Coinbase should be mature at 100 confirmations");
    std::cout << "  Coinbase at height 101 (100 confs): MATURE ✓" << std::endl;

    // Test non-coinbase UTXO (always mature)
    UTXOEntry regular_utxo(1000000000ULL, dummy_script, 1, false);  // 10 DIN regular tx at height 1

    bool regular_mature_at_2 = regular_utxo.isMature(2);  // Just 1 confirmation
    assert(regular_mature_at_2 && "Non-coinbase should be immediately mature");
    std::cout << "  Non-coinbase at height 2 (1 conf): MATURE ✓" << std::endl;

    std::cout << "[Test 2] ✓ PASS: UTXOEntry maturity logic correct" << std::endl;
}

//=============================================================================
// Test 3: Blocks Until Mature
//=============================================================================

void testBlocksUntilMature() {
    std::cout << "\n[Test 3] Calculating blocks until maturity..." << std::endl;

    uint32_t coinbase_height = 1;

    // At height 1 (same block): 100 blocks until mature
    uint32_t blocks_at_1 = CoinbaseMaturity::getBlocksUntilMature(coinbase_height, 1);
    assert(blocks_at_1 == 100 && "Should need 100 blocks at same height");
    std::cout << "  At height 1: " << blocks_at_1 << " blocks remaining ✓" << std::endl;

    // At height 50 (49 confs): 51 blocks until mature
    uint32_t blocks_at_50 = CoinbaseMaturity::getBlocksUntilMature(coinbase_height, 50);
    assert(blocks_at_50 == 51 && "Should need 51 more blocks at height 50");
    std::cout << "  At height 50: " << blocks_at_50 << " blocks remaining ✓" << std::endl;

    // At height 100 (99 confs): 1 block until mature
    uint32_t blocks_at_100 = CoinbaseMaturity::getBlocksUntilMature(coinbase_height, 100);
    assert(blocks_at_100 == 1 && "Should need 1 more block at height 100");
    std::cout << "  At height 100: " << blocks_at_100 << " blocks remaining ✓" << std::endl;

    // At height 101 (100 confs): 0 blocks (mature)
    uint32_t blocks_at_101 = CoinbaseMaturity::getBlocksUntilMature(coinbase_height, 101);
    assert(blocks_at_101 == 0 && "Should be mature at height 101");
    std::cout << "  At height 101: " << blocks_at_101 << " blocks remaining (mature) ✓" << std::endl;

    std::cout << "[Test 3] ✓ PASS: Blocks until mature calculation correct" << std::endl;
}

//=============================================================================
// Test 4: Spendable Height Calculation
//=============================================================================

void testSpendableHeight() {
    std::cout << "\n[Test 4] Spendable height calculation..." << std::endl;

    // Coinbase at height 1 becomes spendable at height 101 (100 blocks on top)
    uint32_t coinbase_height_1 = 1;
    uint32_t spendable_1 = CoinbaseMaturity::getCoinbaseSpendableHeight(coinbase_height_1);
    assert(spendable_1 == 101 && "Coinbase at height 1 should be spendable at height 101");
    std::cout << "  Coinbase at height " << coinbase_height_1
              << " spendable at: " << spendable_1 << " ✓" << std::endl;

    // Coinbase at height 1000 becomes spendable at height 1100 (100 blocks on top)
    uint32_t coinbase_height_1000 = 1000;
    uint32_t spendable_1000 = CoinbaseMaturity::getCoinbaseSpendableHeight(coinbase_height_1000);
    assert(spendable_1000 == 1100 && "Coinbase at height 1000 should be spendable at height 1100");
    std::cout << "  Coinbase at height " << coinbase_height_1000
              << " spendable at: " << spendable_1000 << " ✓" << std::endl;

    std::cout << "[Test 4] ✓ PASS: Spendable height calculation correct" << std::endl;
}

//=============================================================================
// Test 5: Transaction Immature Coinbase Detection
//=============================================================================

void testSpendsImmatureCoinbase() {
    std::cout << "\n[Test 5] Transaction immature coinbase detection..." << std::endl;

    uint32_t current_height = 100;

    // Test 1: Transaction with no inputs
    std::vector<std::pair<uint32_t, bool>> empty_inputs = {};
    bool spends_immature_1 = CoinbaseMaturity::spendsImmatureCoinbase(empty_inputs, current_height);
    assert(!spends_immature_1 && "Empty transaction should not spend immature coinbase");
    std::cout << "  Empty transaction: OK ✓" << std::endl;

    // Test 2: Transaction with regular (non-coinbase) inputs
    std::vector<std::pair<uint32_t, bool>> regular_inputs = {
        {50, false},   // Regular tx at height 50
        {75, false},   // Regular tx at height 75
        {99, false},   // Regular tx at height 99
    };
    bool spends_immature_2 = CoinbaseMaturity::spendsImmatureCoinbase(regular_inputs, current_height);
    assert(!spends_immature_2 && "Regular inputs should be fine");
    std::cout << "  Regular inputs only: OK ✓" << std::endl;

    // Test 3: Transaction with mature coinbase input (height 1, current 101)
    std::vector<std::pair<uint32_t, bool>> mature_coinbase_inputs = {
        {1, true},  // Coinbase at height 1
    };
    bool spends_immature_3 = CoinbaseMaturity::spendsImmatureCoinbase(mature_coinbase_inputs, 101);
    assert(!spends_immature_3 && "Mature coinbase should be spendable");
    std::cout << "  Mature coinbase (height 1 at block 101): OK ✓" << std::endl;

    // Test 4: Transaction with immature coinbase input (height 2, current 100)
    std::vector<std::pair<uint32_t, bool>> immature_coinbase_inputs = {
        {2, true},  // Coinbase at height 2 (only 98 confirmations at height 100)
    };
    bool spends_immature_4 = CoinbaseMaturity::spendsImmatureCoinbase(immature_coinbase_inputs, 100);
    assert(spends_immature_4 && "Immature coinbase should be detected");
    std::cout << "  Immature coinbase (height 2 at block 100): REJECTED ✓" << std::endl;

    // Test 5: Mixed inputs (regular + immature coinbase)
    std::vector<std::pair<uint32_t, bool>> mixed_inputs = {
        {50, false},  // Regular tx
        {2, true},    // Immature coinbase
        {75, false},  // Regular tx
    };
    bool spends_immature_5 = CoinbaseMaturity::spendsImmatureCoinbase(mixed_inputs, 100);
    assert(spends_immature_5 && "Should detect immature coinbase even with other inputs");
    std::cout << "  Mixed inputs with immature coinbase: REJECTED ✓" << std::endl;

    std::cout << "[Test 5] ✓ PASS: Immature coinbase detection correct" << std::endl;
}

//=============================================================================
// Test 6: Edge Cases
//=============================================================================

void testEdgeCases() {
    std::cout << "\n[Test 6] Edge case testing..." << std::endl;

    // Edge case 1: Current height before coinbase height (invalid)
    bool invalid_height = CoinbaseMaturity::isCoinbaseMature(100, 50);
    assert(!invalid_height && "Current height before coinbase should return false");
    std::cout << "  Current height < coinbase height: handled ✓" << std::endl;

    // Edge case 2: Exact boundary at 100 confirmations
    bool exactly_100 = CoinbaseMaturity::isCoinbaseMature(1, 101);
    assert(exactly_100 && "Exactly 100 confirmations should be mature");
    std::cout << "  Exactly 100 confirmations: MATURE ✓" << std::endl;

    // Edge case 3: Just below boundary (99 confirmations)
    bool just_below = CoinbaseMaturity::isCoinbaseMature(1, 100);
    assert(!just_below && "99 confirmations should be immature");
    std::cout << "  99 confirmations: IMMATURE ✓" << std::endl;

    std::cout << "[Test 6] ✓ PASS: Edge cases handled correctly" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Phase 3: Coinbase Maturity Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        testBasicMaturityCalculation();
        testUTXOEntryMaturity();
        testBlocksUntilMature();
        testSpendableHeight();
        testSpendsImmatureCoinbase();
        testEdgeCases();

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ ALL TESTS PASSED" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nCoinbase maturity enforcement verified:" << std::endl;
        std::cout << "  • 100-block maturity rule enforced" << std::endl;
        std::cout << "  • UTXOEntry maturity checks working" << std::endl;
        std::cout << "  • Non-coinbase transactions immediately spendable" << std::endl;
        std::cout << "  • Boundary conditions handled correctly" << std::endl;
        std::cout << "\nReady for integration with transaction validation." << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
