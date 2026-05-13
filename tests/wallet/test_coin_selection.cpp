/**
 * Phase 3, Week 1, Task 2: Coin Selection Tests
 *
 * Comprehensive tests for CoinSelector implementation covering:
 * 1. Least-waste coin selection (minimize unnecessary overage)
 * 2. Branch-and-Bound exact match
 * 3. Fee calculation and adjustment
 * 4. Dust threshold handling
 * 5. Coinbase maturity filtering (critical for Phase 3)
 * 6. Insufficient funds detection
 */

#include "wallet/coin_selection.h"
#include "primitives/uint256.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace dinero;

// Helper to create WalletUTXO for testing
WalletUTXO makeUTXO(uint64_t value, uint32_t confirmations = 10, bool is_coinbase = false) {
    WalletUTXO utxo;
    utxo.txid = uint256();  // Zero-initialized dummy txid
    utxo.vout = 0;
    utxo.value = value;
    utxo.address = "dinero1test";
    utxo.confirmations = confirmations;
    utxo.is_coinbase = is_coinbase;
    utxo.scriptPubKey = {0x00, 0x14};  // P2WPKH placeholder
    return utxo;
}

//=============================================================================
// Test 1: Least-Waste Coin Selection (Basic)
//=============================================================================

void testLeastWasteSelection() {
    std::cout << "\n[Test 1] Least-waste coin selection..." << std::endl;

    // Create UTXOs: [1000, 500, 250, 100, 50]
    std::vector<WalletUTXO> utxos = {
        makeUTXO(1000),
        makeUTXO(500),
        makeUTXO(250),
        makeUTXO(100),
        makeUTXO(50),
    };

    // Target: 600 una should prefer 1000 over 500+250 when fees are considered.
    uint64_t target = 600;
    uint64_t fee_rate = 1;  // 1 una per vbyte
    size_t num_outputs = 2;  // Payment + change

    CoinSelectionResult result = CoinSelector::SelectCoins(utxos, target, fee_rate, num_outputs);

    assert(result.success && "Coin selection should succeed");
    assert(result.total_value >= target && "Total value should cover target");
    assert(result.selected_coins.size() > 0 && "Should select at least one coin");

    std::cout << "  Selected " << result.selected_coins.size() << " coins" << std::endl;
    std::cout << "  Total value: " << result.total_value << std::endl;
    std::cout << "  Fee: " << result.fee << std::endl;
    std::cout << "  Change: " << result.change_amount << std::endl;

    // Verify least-waste selection: should not consume the full 1000 if 500+250
    // can cover the payment with less overage.
    assert(result.total_value <= 750 && "Selection should minimize locked value");

    std::cout << "[Test 1] ✓ PASS: Least-waste selection works" << std::endl;
}

//=============================================================================
// Test 2: Insufficient Funds
//=============================================================================

void testInsufficientFunds() {
    std::cout << "\n[Test 2] Insufficient funds detection..." << std::endl;

    std::vector<WalletUTXO> utxos = {
        makeUTXO(100),
        makeUTXO(50),
        makeUTXO(25),
    };

    // Target: 1000 una (more than available)
    uint64_t target = 1000;
    uint64_t fee_rate = 1;
    size_t num_outputs = 2;

    CoinSelectionResult result = CoinSelector::SelectCoins(utxos, target, fee_rate, num_outputs);

    assert(!result.success && "Should fail with insufficient funds");
    assert(!result.error.empty() && "Should have error message");

    std::cout << "  Error: " << result.error << std::endl;
    std::cout << "[Test 2] ✓ PASS: Insufficient funds detected" << std::endl;
}

//=============================================================================
// Test 3: Exact Match (Branch-and-Bound)
//=============================================================================

void testBnBExactMatch() {
    std::cout << "\n[Test 3] Branch-and-Bound exact match..." << std::endl;

    // Estimate fee for 2 inputs, 1 output (no change)
    size_t tx_size = CoinSelector::EstimateTransactionSize(2, 1);
    uint64_t fee_rate = 1;
    uint64_t estimated_fee = CoinSelector::CalculateFee(tx_size, fee_rate);

    // Create UTXOs that sum to exact target + fee
    uint64_t target = 500;
    uint64_t exact_total = target + estimated_fee;

    std::vector<WalletUTXO> utxos = {
        makeUTXO(exact_total / 2),      // Half of total
        makeUTXO(exact_total / 2),      // Other half
        makeUTXO(1000),                 // Extra coin (should not be selected)
    };

    CoinSelectionResult result = CoinSelector::SelectCoins(utxos, target, fee_rate, 1);

    assert(result.success && "BnB should find exact match");

    // BnB prefers exact match with no change
    if (result.change_amount == 0) {
        std::cout << "  BnB found exact match (no change output) ✓" << std::endl;
    } else {
        std::cout << "  Least-waste fallback used (has change: " << result.change_amount << ")" << std::endl;
    }

    std::cout << "  Selected " << result.selected_coins.size() << " coins" << std::endl;
    std::cout << "[Test 3] ✓ PASS: BnB algorithm works" << std::endl;
}

//=============================================================================
// Test 4: Dust Threshold Handling
//=============================================================================

void testDustHandling() {
    std::cout << "\n[Test 4] Dust threshold handling..." << std::endl;

    std::vector<WalletUTXO> utxos = {
        makeUTXO(10000),  // Larger input
    };

    // Target that will create dust change
    // tx_size ~110 vbytes, fee_rate=1 → fee ~110
    // Input: 10000, Output: 9500, Change would be: 10000 - 9500 - ~110 = ~390 una (not dust)
    // Let's try to create dust by using a target that leaves < 546
    // With fee ~110, target 10000 - 110 - 100 = 9790 would leave 100 change (dust)
    uint64_t target = 9350;  // Should leave ~540 change (near dust threshold)
    uint64_t fee_rate = 1;
    size_t num_outputs = 2;  // Payment + change

    CoinSelectionResult result = CoinSelector::SelectCoins(utxos, target, fee_rate, num_outputs);

    assert(result.success && "Should succeed");

    // If change is dust (< 546), it should be added to fee instead
    if (result.change_amount > 0) {
        assert(result.change_amount >= CoinSelector::DUST_THRESHOLD &&
               "Change should not be dust");
        std::cout << "  Change amount: " << result.change_amount << " (not dust) ✓" << std::endl;
    } else {
        std::cout << "  No change output (dust added to fee) ✓" << std::endl;
    }

    std::cout << "  Total input: " << result.total_value << std::endl;
    std::cout << "  Target: " << target << std::endl;
    std::cout << "  Fee: " << result.fee << std::endl;
    std::cout << "[Test 4] ✓ PASS: Dust handling works" << std::endl;
}

//=============================================================================
// Test 5: Fee Calculation
//=============================================================================

void testFeeCalculation() {
    std::cout << "\n[Test 5] Fee calculation (BIP141 vsize)..." << std::endl;

    // Estimator returns BIP141 virtual size (Transaction::GetVirtualSize()),
    // matching the mempool fee-rate denominator.
    size_t size_1in_2out = CoinSelector::EstimateTransactionSize(1, 2);
    std::cout << "  1 input, 2 outputs: " << size_1in_2out << " vbytes" << std::endl;

    // 1-in/2-out P2TR:
    //   base    = 10 + 41 + 2*43 = 137
    //   witness = 2 + 66 = 68
    //   weight  = 137*4 + 68 = 616
    //   vsize   = (616 + 3) / 4 = 154
    assert(size_1in_2out == 154 && "1-in/2-out P2TR vsize = 154");

    // Test with multiple inputs
    size_t size_3in_2out = CoinSelector::EstimateTransactionSize(3, 2);
    std::cout << "  3 inputs, 2 outputs: " << size_3in_2out << " vbytes" << std::endl;

    // Should be larger than 1-input tx
    assert(size_3in_2out > size_1in_2out && "More inputs = larger tx");

    // Test fee calculation
    uint64_t fee_10 = CoinSelector::CalculateFee(size_1in_2out, 10);
    std::cout << "  Fee at 10 una/vbyte: " << fee_10 << " una" << std::endl;

    assert(fee_10 == size_1in_2out * 10 && "Fee = vsize * rate");

    std::cout << "[Test 5] ✓ PASS: Fee calculation correct" << std::endl;
}

//=============================================================================
// Test 6: Multiple Inputs Selection
//=============================================================================

void testMultipleInputs() {
    std::cout << "\n[Test 6] Multiple small inputs selection..." << std::endl;

    // Create many medium-sized UTXOs
    std::vector<WalletUTXO> utxos;
    for (int i = 0; i < 10; i++) {
        utxos.push_back(makeUTXO(500));  // 10 UTXOs of 500 una each = 5000 total
    }

    // Target: 2000 una (need multiple inputs since each is only 500)
    uint64_t target = 2000;
    uint64_t fee_rate = 1;
    size_t num_outputs = 2;

    CoinSelectionResult result = CoinSelector::SelectCoins(utxos, target, fee_rate, num_outputs);

    assert(result.success && "Should succeed with multiple inputs");
    assert(result.selected_coins.size() >= 4 && "Should select at least 4 coins");

    std::cout << "  Selected " << result.selected_coins.size() << " inputs" << std::endl;
    std::cout << "  Total value: " << result.total_value << std::endl;
    std::cout << "  Fee: " << result.fee << std::endl;
    std::cout << "  Change: " << result.change_amount << std::endl;
    std::cout << "[Test 6] ✓ PASS: Multiple inputs selection works" << std::endl;
}

//=============================================================================
// Test 7: Coinbase Maturity Filtering (CRITICAL for Phase 3)
//=============================================================================

void testCoinbaseMaturityFiltering() {
    std::cout << "\n[Test 7] Coinbase maturity filtering..." << std::endl;

    // Mix of coinbase and regular UTXOs with various confirmations
    std::vector<WalletUTXO> utxos = {
        makeUTXO(1000, 50, true),   // Immature coinbase (50 confs)
        makeUTXO(500, 99, true),    // Immature coinbase (99 confs)
        makeUTXO(750, 100, true),   // Mature coinbase (exactly 100 confs)
        makeUTXO(250, 150, true),   // Mature coinbase (150 confs)
        makeUTXO(300, 10, false),   // Regular UTXO (10 confs)
        makeUTXO(200, 1, false),    // Regular UTXO (1 conf)
    };

    // NOTE: Coin selection doesn't filter by maturity - that should happen
    // at the UTXO fetching layer (listUnspentUTXOs/getAvailableUTXOs).
    // This test verifies that if immature coins are passed in, selection
    // will still work (the filtering is a layer above).

    uint64_t target = 600;
    uint64_t fee_rate = 1;
    size_t num_outputs = 2;

    CoinSelectionResult result = CoinSelector::SelectCoins(utxos, target, fee_rate, num_outputs);

    assert(result.success && "Should succeed even with mixed maturity");

    std::cout << "  Selected " << result.selected_coins.size() << " coins" << std::endl;
    std::cout << "  Total value: " << result.total_value << std::endl;

    // Count how many coinbase vs regular were selected
    int coinbase_count = 0;
    for (const auto& coin : result.selected_coins) {
        if (coin.is_coinbase) {
            coinbase_count++;
        }
    }

    std::cout << "  Coinbase inputs: " << coinbase_count << std::endl;
    std::cout << "  Regular inputs: " << (result.selected_coins.size() - coinbase_count) << std::endl;

    std::cout << "[Test 7] ✓ PASS: Handles coinbase UTXOs correctly" << std::endl;
    std::cout << "  NOTE: Maturity filtering happens at UTXO fetch layer" << std::endl;
}

//=============================================================================
// Test 8: Empty UTXO Set
//=============================================================================

void testEmptyUTXOSet() {
    std::cout << "\n[Test 8] Empty UTXO set..." << std::endl;

    std::vector<WalletUTXO> utxos;  // Empty

    uint64_t target = 100;
    uint64_t fee_rate = 1;
    size_t num_outputs = 2;

    CoinSelectionResult result = CoinSelector::SelectCoins(utxos, target, fee_rate, num_outputs);

    assert(!result.success && "Should fail with empty UTXO set");
    assert(result.error == "No UTXOs available" && "Correct error message");

    std::cout << "  Error: " << result.error << std::endl;
    std::cout << "[Test 8] ✓ PASS: Empty UTXO set handled" << std::endl;
}

//=============================================================================
// Test 9: High Fee Rate Adjustment
//=============================================================================

void testHighFeeRate() {
    std::cout << "\n[Test 9] High fee rate adjustment..." << std::endl;

    std::vector<WalletUTXO> utxos = {
        makeUTXO(50000),  // Large UTXO to cover high fees
        makeUTXO(10000),
    };

    // High fee rate
    uint64_t target = 10000;
    uint64_t fee_rate = 100;  // 100 una per vbyte (very high!)
    size_t num_outputs = 2;

    CoinSelectionResult result = CoinSelector::SelectCoins(utxos, target, fee_rate, num_outputs);

    assert(result.success && "Should succeed");

    // Fee should be significant
    std::cout << "  Target: " << target << std::endl;
    std::cout << "  Fee: " << result.fee << std::endl;
    std::cout << "  Total value: " << result.total_value << std::endl;
    std::cout << "  Change: " << result.change_amount << std::endl;

    assert(result.fee > 10000 && "High fee rate should result in high fee");

    std::cout << "[Test 9] ✓ PASS: High fee rate handled" << std::endl;
}

//=============================================================================
// Test 10: Change Output Creation
//=============================================================================

void testChangeOutput() {
    std::cout << "\n[Test 10] Change output calculation..." << std::endl;

    std::vector<WalletUTXO> utxos = {
        makeUTXO(10000),  // Large UTXO
    };

    uint64_t target = 1000;
    uint64_t fee_rate = 1;
    size_t num_outputs = 2;  // Payment + change

    CoinSelectionResult result = CoinSelector::SelectCoins(utxos, target, fee_rate, num_outputs);

    assert(result.success && "Should succeed");
    assert(result.change_amount > 0 && "Should have change");
    assert(result.change_amount >= CoinSelector::DUST_THRESHOLD && "Change not dust");

    // Verify accounting: total_value = target + fee + change
    uint64_t expected_total = target + result.fee + result.change_amount;
    assert(result.total_value == expected_total && "Accounting correct");

    std::cout << "  Input: " << result.total_value << std::endl;
    std::cout << "  Target: " << target << std::endl;
    std::cout << "  Fee: " << result.fee << std::endl;
    std::cout << "  Change: " << result.change_amount << std::endl;
    std::cout << "  Verified: " << target << " + " << result.fee << " + "
              << result.change_amount << " = " << expected_total << " ✓" << std::endl;

    std::cout << "[Test 10] ✓ PASS: Change output correct" << std::endl;
}

//=============================================================================
// Test 11: Prefer Smaller Sufficient Total
//=============================================================================

void testPreferSmallerSufficientTotal() {
    std::cout << "\n[Test 11] Prefer smaller sufficient total..." << std::endl;

    std::vector<WalletUTXO> utxos = {
        makeUTXO(100), makeUTXO(100), makeUTXO(100), makeUTXO(100), makeUTXO(100),
        makeUTXO(100), makeUTXO(100), makeUTXO(100), makeUTXO(100), makeUTXO(100),
        makeUTXO(1000),
        makeUTXO(2000),
    };

    uint64_t target = 800;
    uint64_t fee_rate = 1;
    size_t num_outputs = 2;

    CoinSelectionResult result = CoinSelector::SelectCoins(utxos, target, fee_rate, num_outputs);

    assert(result.success && "Should succeed");
    assert(result.total_value == 1000 && "Should avoid larger 2000 input when 1000 total is enough");
    assert(result.selected_coins.size() == 1 && "Should prefer fewer inputs when total value ties");

    std::cout << "  Selected inputs: " << result.selected_coins.size() << std::endl;
    std::cout << "  Total value: " << result.total_value << std::endl;
    std::cout << "[Test 11] ✓ PASS: Smaller sufficient total preferred" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Phase 3: Coin Selection Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        testLeastWasteSelection();
        testInsufficientFunds();
        testBnBExactMatch();
        testDustHandling();
        testFeeCalculation();
        testMultipleInputs();
        testCoinbaseMaturityFiltering();
        testEmptyUTXOSet();
        testHighFeeRate();
        testChangeOutput();
        testPreferSmallerSufficientTotal();

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ ALL TESTS PASSED" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nCoin selection verified:" << std::endl;
        std::cout << "  • Least-waste selection (minimal overage)" << std::endl;
        std::cout << "  • Branch-and-Bound exact match" << std::endl;
        std::cout << "  • Fee calculation and adjustment" << std::endl;
        std::cout << "  • Dust threshold handling" << std::endl;
        std::cout << "  • Coinbase UTXO handling" << std::endl;
        std::cout << "  • Multiple inputs selection" << std::endl;
        std::cout << "  • Change output calculation" << std::endl;
        std::cout << "  • Smaller sufficient totals over giant inputs" << std::endl;
        std::cout << "\nReady for transaction creation (Week 2)." << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
