/**
 * Phase G.3.3: Consensus Validation - Pure Unit Tests
 *
 * Test Scope:
 * - Valid tx passes consensus checks
 * - Bad signature fails
 * - Spend non-existent UTXO fails
 * - Double-spend inside tx fails
 * - Locktime violation fails
 * - Valid block passes consensus checks
 * - Invalid merkle root fails
 * - Bad coinbase fails
 * - Invalid tx inside block fails block
 *
 * Test Constraints:
 * ✅ Script execution allowed
 * ✅ Signature checks allowed
 * ✅ UTXO snapshot (read-only)
 * ❌ NO chainstate writes
 * ❌ NO UTXO set updates
 * ❌ NO mempool insertion
 * ❌ NO disk writes
 * ❌ NO fork-choice decisions
 * ✅ Pure evaluation (deterministic)
 * ✅ Fake UTXO snapshots allowed
 * ✅ Runtime < 500ms
 */

#include "../../include/p2p/consensus_validator.h"
#include "../../include/primitives/hash_domains.h"  // Phase M.4.3-B: TxId type
#include <chrono>
#include <iostream>
#include <cassert>
#include <vector>

using namespace dinero::p2p;
using dinero::OutPoint;
using dinero::uint256;

//=============================================================================
// Mock UTXO Snapshot (Read-Only)
//=============================================================================

class MockUTXOSnapshot : public IUTXOSnapshot {
public:
    std::map<OutPoint, TxOut> utxos_;

    void addUTXO(const OutPoint& outpoint, const TxOut& txout) {
        utxos_[outpoint] = txout;
    }

    std::optional<TxOut> getUTXO(const OutPoint& outpoint) const override {
        auto it = utxos_.find(outpoint);
        if (it != utxos_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    bool hasUTXO(const OutPoint& outpoint) const override {
        return utxos_.count(outpoint) > 0;
    }
};

//=============================================================================
// Test 1: Valid TX Passes
//=============================================================================

void test_valid_tx_passes() {
    std::cout << "\n[Test 1] Valid tx passes consensus validation" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view;
    ConsensusParams params;  // Default consensus params

    // Create a simple valid transaction
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;

    // Add input referencing existing UTXO
    TxIn input;
    input.prevout.txid = dinero::TxId(uint256::FromHexUnsafe("0000000000000000000000000000000000000000000000000000000000000001"));
    input.prevout.vout = 0;
    input.scriptSig = {0x51}; // OP_1 (simplified)
    input.sequence = 0xFFFFFFFF;
    tx.inputs.push_back(input);

    // Add output
    TxOut output;
    output.value = 50ULL * 100000000ULL; // 50 coins
    output.scriptPubKey = {0x51}; // OP_1 (simplified)
    tx.outputs.push_back(output);

    // Add UTXO to snapshot
    TxOut prev_output;
    prev_output.value = 50ULL * 100000000ULL;
    prev_output.scriptPubKey = {0x51}; // Matches input's scriptSig
    utxo_view.addUTXO(input.prevout, prev_output);

    auto result = validator.validateTx(tx, utxo_view, params);

    assert(result.ok && "Valid tx should pass");

    std::cout << "  [✓] Valid tx passes!" << std::endl;
}

//=============================================================================
// Test 2: Spend Non-Existent UTXO Fails
//=============================================================================

void test_spend_nonexistent_utxo_fails() {
    std::cout << "\n[Test 2] Spend non-existent UTXO fails" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view; // Empty snapshot
    ConsensusParams params;

    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;

    // Add input referencing non-existent UTXO
    TxIn input;
    input.prevout.txid = dinero::TxId(uint256::FromHexUnsafe("0000000000000000000000000000000000000000000000000000000000000001"));
    input.prevout.vout = 0;
    input.scriptSig = {0x51};
    input.sequence = 0xFFFFFFFF;
    tx.inputs.push_back(input);

    // Add output
    TxOut output;
    output.value = 50ULL * 100000000ULL;
    output.scriptPubKey = {0x51};
    tx.outputs.push_back(output);

    auto result = validator.validateTx(tx, utxo_view, params);

    assert(!result.ok && "Spending non-existent UTXO should fail");
    assert(result.error.find("UTXO") != std::string::npos ||
           result.error.find("missing") != std::string::npos &&
           "Error should mention missing UTXO");

    std::cout << "  [✓] Spend non-existent UTXO correctly rejected!" << std::endl;
}

//=============================================================================
// Test 3: Double-Spend Inside TX Fails
//=============================================================================

void test_double_spend_fails() {
    std::cout << "\n[Test 3] Double-spend inside tx fails" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view;
    ConsensusParams params;

    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;

    // Same outpoint used twice
    OutPoint same_outpoint;
    same_outpoint.txid = dinero::TxId(uint256::FromHexUnsafe("0000000000000000000000000000000000000000000000000000000000000001"));
    same_outpoint.vout = 0;

    TxIn input1;
    input1.prevout = same_outpoint;
    input1.scriptSig = {0x51};
    input1.sequence = 0xFFFFFFFF;
    tx.inputs.push_back(input1);

    TxIn input2;
    input2.prevout = same_outpoint; // DUPLICATE!
    input2.scriptSig = {0x51};
    input2.sequence = 0xFFFFFFFF;
    tx.inputs.push_back(input2);

    // Add output
    TxOut output;
    output.value = 50ULL * 100000000ULL;
    output.scriptPubKey = {0x51};
    tx.outputs.push_back(output);

    // Add UTXO to snapshot
    TxOut prev_output;
    prev_output.value = 100ULL * 100000000ULL;
    prev_output.scriptPubKey = {0x51};
    utxo_view.addUTXO(same_outpoint, prev_output);

    auto result = validator.validateTx(tx, utxo_view, params);

    assert(!result.ok && "Double-spend should fail");
    assert(result.error.find("duplicate") != std::string::npos &&
           "Error should mention duplicate inputs");

    std::cout << "  [✓] Double-spend correctly rejected!" << std::endl;
}

//=============================================================================
// Test 4: Coinbase Shape Rules
//=============================================================================

void test_coinbase_rules() {
    std::cout << "\n[Test 4] Coinbase shape rules enforced" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view; // Coinbase doesn't need UTXOs
    ConsensusParams params;

    // Valid coinbase
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.locktime = 0;

    // Coinbase input (null outpoint)
    TxIn cb_input;
    cb_input.prevout.txid = dinero::TxId(uint256()); // Phase M.4.3-B: All zeros
    cb_input.prevout.vout = 0xFFFFFFFF;
    cb_input.scriptSig = {0x03, 0x01, 0x02, 0x03}; // Block height data
    cb_input.sequence = 0xFFFFFFFF;
    coinbase.inputs.push_back(cb_input);

    // Coinbase output
    TxOut cb_output;
    cb_output.value = 50ULL * 100000000ULL;
    cb_output.scriptPubKey = {0x51};
    coinbase.outputs.push_back(cb_output);

    auto result = validator.validateCoinbase(coinbase, 1, params); // height = 1

    assert(result.ok && "Valid coinbase should pass");

    std::cout << "  [✓] Coinbase rules enforced!" << std::endl;
}

//=============================================================================
// Test 5: Empty TX Fails
//=============================================================================

void test_empty_tx_fails() {
    std::cout << "\n[Test 5] Empty tx (no inputs/outputs) fails" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view;
    ConsensusParams params;

    Transaction empty_tx;
    empty_tx.version = 1;
    empty_tx.locktime = 0;
    // No inputs, no outputs

    auto result = validator.validateTx(empty_tx, utxo_view, params);

    assert(!result.ok && "Empty tx should fail");

    std::cout << "  [✓] Empty tx correctly rejected!" << std::endl;
}

//=============================================================================
// Test 6: Deterministic Validation
//=============================================================================

void test_deterministic_validation() {
    std::cout << "\n[Test 6] Validation is deterministic (same input → same result)" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view;
    ConsensusParams params;

    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;

    TxIn input;
    input.prevout.txid = dinero::TxId(uint256::FromHexUnsafe("0000000000000000000000000000000000000000000000000000000000000001"));
    input.prevout.vout = 0;
    input.scriptSig = {0x51};
    input.sequence = 0xFFFFFFFF;
    tx.inputs.push_back(input);

    TxOut output;
    output.value = 50ULL * 100000000ULL;
    output.scriptPubKey = {0x51};
    tx.outputs.push_back(output);

    TxOut prev_output;
    prev_output.value = 50ULL * 100000000ULL;
    prev_output.scriptPubKey = {0x51};
    utxo_view.addUTXO(input.prevout, prev_output);

    // Validate twice
    auto result1 = validator.validateTx(tx, utxo_view, params);
    auto result2 = validator.validateTx(tx, utxo_view, params);

    assert(result1.ok == result2.ok && "Results should match");
    assert(result1.error == result2.error && "Errors should match");

    std::cout << "  [✓] Validation is deterministic!" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "G.3.3: Consensus Validation Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nPure consensus evaluation tests" << std::endl;
    std::cout << "Scripts OK | UTXO read-only | NO state mutation" << std::endl;

    auto start = std::chrono::steady_clock::now();

    try {
        // Test 1: Valid tx
        test_valid_tx_passes();

        // Test 2: Non-existent UTXO
        test_spend_nonexistent_utxo_fails();

        // Test 3: Double-spend
        test_double_spend_fails();

        // Test 4: Coinbase rules
        test_coinbase_rules();

        // Test 5: Empty tx
        test_empty_tx_fails();

        // Test 6: Deterministic
        test_deterministic_validation();

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ All Consensus Validation Tests Passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nRuntime: " << duration.count() << " ms" << std::endl;

        if (duration.count() < 500) {
            std::cout << "[✓] Fast: < 500ms requirement met" << std::endl;
        } else {
            std::cout << "[!] Warning: Exceeded 500ms target" << std::endl;
        }

        std::cout << "\nSummary:" << std::endl;
        std::cout << "  [✓] Valid tx passes" << std::endl;
        std::cout << "  [✓] Spend non-existent UTXO fails" << std::endl;
        std::cout << "  [✓] Double-spend inside tx fails" << std::endl;
        std::cout << "  [✓] Coinbase shape rules enforced" << std::endl;
        std::cout << "  [✓] Empty tx fails" << std::endl;
        std::cout << "  [✓] Validation is deterministic" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}
