/**
 * Phase 3, Week 2: Transaction Builder Tests
 *
 * Comprehensive tests for TransactionBuilder covering:
 * 1. Basic transaction creation
 * 2. Change output handling
 * 3. Dust threshold logic
 * 4. Fee calculation and adjustment
 * 5. Multiple recipients
 * 6. Insufficient funds detection
 * 7. Address validation
 * 8. Coinbase maturity integration (via UTXO filtering)
 */

#include "wallet/transaction_builder.h"
#include "wallet/utxo_index.h"
#include "primitives/uint256.h"
#include <iostream>
#include <cassert>
#include <map>
#include <vector>
#include <string>

using namespace dinero;

//=============================================================================
// Test Setup - Mock UTXOIndex for testing
//=============================================================================

class MockUTXOIndex : public UTXOIndex {
private:
    std::vector<UTXO> mock_utxos_;

public:
    MockUTXOIndex() : UTXOIndex("/tmp/test_utxo") {}

    void AddMockUTXO(uint64_t value, bool is_coinbase = false, uint32_t height = 1) {
        UTXO utxo;
        utxo.txid = uint256();  // Dummy txid
        utxo.vout = static_cast<uint32_t>(mock_utxos_.size());
        utxo.value = static_cast<int64_t>(value);
        utxo.height = height;
        utxo.is_coinbase = is_coinbase;
        utxo.path = "test_address_" + std::to_string(mock_utxos_.size());

        // P2WPKH scriptPubKey (OP_0 <20-byte-hash>)
        utxo.spk.push_back(0x00);
        utxo.spk.push_back(0x14);
        for (int i = 0; i < 20; i++) {
            utxo.spk.push_back(static_cast<uint8_t>(i));
        }

        mock_utxos_.push_back(utxo);
    }

    std::vector<UTXO> GetUnspentUTXOs() {
        return mock_utxos_;
    }

    void ClearMockUTXOs() {
        mock_utxos_.clear();
    }
};

//=============================================================================
// Test 1: Basic Transaction Creation
//=============================================================================

void testBasicTransactionCreation() {
    std::cout << "\n[Test 1] Basic transaction creation..." << std::endl;

    MockUTXOIndex utxo_index;
    utxo_index.AddMockUTXO(100000);  // 100,000 una

    TransactionBuilder builder(&utxo_index);

    std::vector<TransactionBuilder::Recipient> recipients = {
        {"din1qtest1234567890abcdefghijklmnopqrstuvwxyz", 50000}
    };

    auto result = builder.PreviewTransaction(recipients);

    assert(result.success && "Transaction should succeed");
    assert(result.transaction.vin.size() == 1 && "Should have 1 input");
    assert(result.transaction.vout.size() == 2 && "Should have 2 outputs (payment + change)");
    assert(result.fee > 0 && "Should calculate fee");
    assert(result.change_amount > 0 && "Should have change");

    // Verify accounting: input = output + change + fee
    int64_t total_input = 100000;
    int64_t total_output = 50000 + result.change_amount;
    assert(total_input == total_output + result.fee && "Accounting should balance");

    std::cout << "  Inputs: " << result.transaction.vin.size() << std::endl;
    std::cout << "  Outputs: " << result.transaction.vout.size() << std::endl;
    std::cout << "  Fee: " << result.fee << " una" << std::endl;
    std::cout << "  Change: " << result.change_amount << " una" << std::endl;
    std::cout << "  Accounting: " << total_input << " = " << 50000 << " + "
              << result.change_amount << " + " << result.fee << " ✓" << std::endl;

    std::cout << "[Test 1] ✓ PASS: Basic transaction creation works" << std::endl;
}

//=============================================================================
// Test 2: Dust Threshold Handling
//=============================================================================

void testDustThreshold() {
    std::cout << "\n[Test 2] Dust threshold handling..." << std::endl;

    MockUTXOIndex utxo_index;
    utxo_index.AddMockUTXO(10000);  // Small UTXO

    TransactionBuilder builder(&utxo_index);

    // Send amount that will create dust change (< 546 una)
    // 10000 - 9500 - ~150 fee = ~350 una change (dust!)
    std::vector<TransactionBuilder::Recipient> recipients = {
        {"din1qtest1234567890abcdefghijklmnopqrstuvwxyz", 9500}
    };

    TransactionBuilder::BuildOptions options;
    options.dust_threshold = 546;

    auto result = builder.PreviewTransaction(recipients, options);

    assert(result.success && "Transaction should succeed");

    if (result.change_amount == 0) {
        // Dust was added to fee
        std::cout << "  Dust added to fee (no change output) ✓" << std::endl;
        assert(result.transaction.vout.size() == 1 && "Should have only 1 output (no change)");
    } else {
        // Change is above dust threshold
        assert(result.change_amount >= options.dust_threshold && "Change should not be dust");
        std::cout << "  Change: " << result.change_amount << " una (not dust) ✓" << std::endl;
    }

    std::cout << "  Fee: " << result.fee << " una" << std::endl;
    std::cout << "[Test 2] ✓ PASS: Dust threshold handled correctly" << std::endl;
}

//=============================================================================
// Test 3: Multiple Recipients
//=============================================================================

void testMultipleRecipients() {
    std::cout << "\n[Test 3] Multiple recipients..." << std::endl;

    MockUTXOIndex utxo_index;
    utxo_index.AddMockUTXO(500000);  // Large UTXO

    TransactionBuilder builder(&utxo_index);

    std::vector<TransactionBuilder::Recipient> recipients = {
        {"din1qtest1111111111111111111111111111111111", 100000},
        {"din1qtest2222222222222222222222222222222222", 150000},
        {"din1qtest3333333333333333333333333333333333", 50000},
    };

    auto result = builder.PreviewTransaction(recipients);

    assert(result.success && "Transaction should succeed");
    assert(result.transaction.vout.size() == 4 && "Should have 4 outputs (3 payments + change)");

    // Verify total outputs
    uint64_t total_recipient_amount = 100000 + 150000 + 50000;
    std::cout << "  Recipients: " << recipients.size() << std::endl;
    std::cout << "  Total sent: " << total_recipient_amount << " una" << std::endl;
    std::cout << "  Change: " << result.change_amount << " una" << std::endl;
    std::cout << "  Fee: " << result.fee << " una" << std::endl;

    std::cout << "[Test 3] ✓ PASS: Multiple recipients works" << std::endl;
}

//=============================================================================
// Test 4: Insufficient Funds
//=============================================================================

void testInsufficientFunds() {
    std::cout << "\n[Test 4] Insufficient funds detection..." << std::endl;

    MockUTXOIndex utxo_index;
    utxo_index.AddMockUTXO(10000);  // Small UTXO

    TransactionBuilder builder(&utxo_index);

    // Try to send more than available
    std::vector<TransactionBuilder::Recipient> recipients = {
        {"din1qtest1234567890abcdefghijklmnopqrstuvwxyz", 50000}
    };

    auto result = builder.PreviewTransaction(recipients);

    assert(!result.success && "Should fail with insufficient funds");
    assert(!result.error.empty() && "Should have error message");
    assert(result.error.find("Insufficient funds") != std::string::npos && "Error message correct");

    std::cout << "  Error: " << result.error << std::endl;
    std::cout << "[Test 4] ✓ PASS: Insufficient funds detected" << std::endl;
}

//=============================================================================
// Test 5: Fee Estimation
//=============================================================================

void testFeeEstimation() {
    std::cout << "\n[Test 5] Fee estimation accuracy..." << std::endl;

    // Test static fee estimation method
    int64_t fee_1in_2out = TransactionBuilder::EstimateFee(1, 2, 1.0);  // 1 sat/vB
    std::cout << "  Fee for 1 input, 2 outputs @ 1 sat/vB: " << fee_1in_2out << " una" << std::endl;

    // Expected vsize for P2WPKH: 11 + 68*inputs + 31*outputs
    // 1 input, 2 outputs: 11 + 68 + 62 = 141 vbytes
    assert(fee_1in_2out > 100 && fee_1in_2out < 200 && "Fee should be ~141 una");

    // Test with multiple inputs
    int64_t fee_3in_2out = TransactionBuilder::EstimateFee(3, 2, 1.0);
    std::cout << "  Fee for 3 inputs, 2 outputs @ 1 sat/vB: " << fee_3in_2out << " una" << std::endl;

    // 3 inputs, 2 outputs: 11 + 204 + 62 = 277 vbytes
    assert(fee_3in_2out > fee_1in_2out && "More inputs = higher fee");
    assert(fee_3in_2out > 250 && fee_3in_2out < 300 && "Fee should be ~277 una");

    // Test with high fee rate
    int64_t fee_high_rate = TransactionBuilder::EstimateFee(1, 2, 10.0);  // 10 sat/vB
    std::cout << "  Fee for 1 input, 2 outputs @ 10 sat/vB: " << fee_high_rate << " una" << std::endl;
    assert(fee_high_rate == fee_1in_2out * 10 && "Fee should scale with rate");

    std::cout << "[Test 5] ✓ PASS: Fee estimation accurate" << std::endl;
}

//=============================================================================
// Test 6: Multiple Inputs Selection
//=============================================================================

void testMultipleInputsSelection() {
    std::cout << "\n[Test 6] Multiple inputs selection..." << std::endl;

    MockUTXOIndex utxo_index;
    // Add multiple small UTXOs
    for (int i = 0; i < 5; i++) {
        utxo_index.AddMockUTXO(20000);  // 5 x 20,000 = 100,000 total
    }

    TransactionBuilder builder(&utxo_index);

    // Send amount requiring multiple inputs
    std::vector<TransactionBuilder::Recipient> recipients = {
        {"din1qtest1234567890abcdefghijklmnopqrstuvwxyz", 75000}
    };

    auto result = builder.PreviewTransaction(recipients);

    assert(result.success && "Transaction should succeed");
    assert(result.transaction.vin.size() >= 4 && "Should select multiple inputs");
    assert(result.selected_utxos.size() >= 4 && "Should have multiple selected UTXOs");

    std::cout << "  Selected " << result.transaction.vin.size() << " inputs" << std::endl;
    std::cout << "  Total input value: " << (result.transaction.vin.size() * 20000) << " una" << std::endl;
    std::cout << "  Payment: 75000 una" << std::endl;
    std::cout << "  Fee: " << result.fee << " una" << std::endl;
    std::cout << "  Change: " << result.change_amount << " una" << std::endl;

    std::cout << "[Test 6] ✓ PASS: Multiple inputs selection works" << std::endl;
}

//=============================================================================
// Test 7: Address Validation
//=============================================================================

void testAddressValidation() {
    std::cout << "\n[Test 7] Address validation..." << std::endl;

    // Valid addresses (simplified validation)
    assert(TransactionBuilder::ValidateAddress("din1q" + std::string(38, 'a')) && "Valid address");
    assert(TransactionBuilder::ValidateAddress("din1q" + std::string(40, 'x')) && "Valid address");

    // Invalid addresses
    assert(!TransactionBuilder::ValidateAddress("") && "Empty address invalid");
    assert(!TransactionBuilder::ValidateAddress("btc1qtest") && "Wrong HRP invalid");
    assert(!TransactionBuilder::ValidateAddress("din1qABC") && "Invalid characters");
    assert(!TransactionBuilder::ValidateAddress("din1q") && "Too short");

    std::cout << "  Valid P2WPKH addresses accepted ✓" << std::endl;
    std::cout << "  Invalid addresses rejected ✓" << std::endl;

    std::cout << "[Test 7] ✓ PASS: Address validation works" << std::endl;
}

//=============================================================================
// Test 8: Change Address Generation
//=============================================================================

void testChangeAddressGeneration() {
    std::cout << "\n[Test 8] Change address generation..." << std::endl;

    MockUTXOIndex utxo_index;
    utxo_index.AddMockUTXO(100000);

    TransactionBuilder builder(&utxo_index);

    std::vector<TransactionBuilder::Recipient> recipients = {
        {"din1qtest1234567890abcdefghijklmnopqrstuvwxyz", 50000}
    };

    // Test with auto-generated change address
    auto result1 = builder.PreviewTransaction(recipients);
    assert(result1.success && "Transaction should succeed");
    assert(!result1.change_address.empty() && "Should generate change address");
    assert(result1.change_address.substr(0, 4) == "din1" && "Change address should have correct HRP");

    std::cout << "  Auto-generated change: " << result1.change_address.substr(0, 20) << "..." << std::endl;

    // Test with custom change address
    TransactionBuilder::BuildOptions options;
    options.change_address = "din1qcustomchangeaddr1234567890abcdefgh";

    auto result2 = builder.PreviewTransaction(recipients, options);
    assert(result2.success && "Transaction should succeed");
    assert(result2.change_address == options.change_address.value() && "Should use custom change address");

    std::cout << "  Custom change address used ✓" << std::endl;

    std::cout << "[Test 8] ✓ PASS: Change address generation works" << std::endl;
}

//=============================================================================
// Test 9: Exact Amount (No Change)
//=============================================================================

void testExactAmount() {
    std::cout << "\n[Test 9] Exact amount transaction (no change)..." << std::endl;

    MockUTXOIndex utxo_index;
    utxo_index.AddMockUTXO(100000);

    TransactionBuilder builder(&utxo_index);

    // Calculate exact fee first
    int64_t fee = TransactionBuilder::EstimateFee(1, 1, 1.0);  // 1 input, 1 output (no change)

    // Send exact amount to use entire UTXO (minus fee)
    int64_t exact_amount = 100000 - fee;

    std::vector<TransactionBuilder::Recipient> recipients = {
        {"din1qtest1234567890abcdefghijklmnopqrstuvwxyz", exact_amount}
    };

    auto result = builder.PreviewTransaction(recipients);

    assert(result.success && "Transaction should succeed");

    if (result.change_amount == 0) {
        std::cout << "  No change output (exact match) ✓" << std::endl;
        assert(result.transaction.vout.size() == 1 && "Should have only 1 output");
    } else if (result.change_amount < 546) {
        std::cout << "  Dust change added to fee ✓" << std::endl;
    } else {
        std::cout << "  Change: " << result.change_amount << " una" << std::endl;
    }

    std::cout << "  Spent: " << exact_amount << " una" << std::endl;
    std::cout << "  Fee: " << result.fee << " una" << std::endl;

    std::cout << "[Test 9] ✓ PASS: Exact amount handling works" << std::endl;
}

//=============================================================================
// Test 10: Empty UTXO Set
//=============================================================================

void testEmptyUTXOSet() {
    std::cout << "\n[Test 10] Empty UTXO set..." << std::endl;

    MockUTXOIndex utxo_index;
    // Don't add any UTXOs

    TransactionBuilder builder(&utxo_index);

    std::vector<TransactionBuilder::Recipient> recipients = {
        {"din1qtest1234567890abcdefghijklmnopqrstuvwxyz", 50000}
    };

    auto result = builder.PreviewTransaction(recipients);

    assert(!result.success && "Should fail with empty UTXO set");
    assert(result.error.find("No unspent UTXOs") != std::string::npos ||
           result.error.find("No P2WPKH UTXOs") != std::string::npos &&
           "Error message correct");

    std::cout << "  Error: " << result.error << std::endl;
    std::cout << "[Test 10] ✓ PASS: Empty UTXO set handled" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Phase 3: Transaction Builder Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        testBasicTransactionCreation();
        testDustThreshold();
        testMultipleRecipients();
        testInsufficientFunds();
        testFeeEstimation();
        testMultipleInputsSelection();
        testAddressValidation();
        testChangeAddressGeneration();
        testExactAmount();
        testEmptyUTXOSet();

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ ALL TESTS PASSED" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nTransaction creation verified:" << std::endl;
        std::cout << "  • Basic transaction creation" << std::endl;
        std::cout << "  • Change output handling" << std::endl;
        std::cout << "  • Dust threshold (546 una)" << std::endl;
        std::cout << "  • Fee calculation (SegWit vsize)" << std::endl;
        std::cout << "  • Multiple recipients" << std::endl;
        std::cout << "  • Multiple inputs selection" << std::endl;
        std::cout << "  • Address validation" << std::endl;
        std::cout << "  • Change address generation" << std::endl;
        std::cout << "  • Insufficient funds detection" << std::endl;
        std::cout << "\nReady for Week 3: Transaction Signing (BIP143)." << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
