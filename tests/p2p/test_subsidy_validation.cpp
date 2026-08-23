/**
 * Phase G.3.3 Extension: Subsidy Validation Tests
 *
 * TESTS WRITTEN FIRST - NO IMPLEMENTATION YET
 *
 * Test Scope:
 * - Coinbase value limits across all subsidy schedule transitions
 * - Genesis block coinbase
 * - First halving boundary
 * - Exact halving boundary (off-by-one detection)
 * - Tail-emission transition and perpetual subsidy floor
 * - Tail-emission blocks with transaction fees
 * - Overflow protection
 * - Invalid coinbase overpay detection
 * - Fee accumulation correctness
 * - Block-level validation (all txs + coinbase economics)
 *
 * Test Constraints:
 * ✅ Pure evaluation (read-only UTXO snapshot)
 * ✅ Deterministic (same input → same result)
 * ❌ NO state mutation
 * ❌ NO disk writes
 * ❌ NO mempool
 * ✅ Runtime < 500ms
 *
 * Critical: This is the LAST consensus validation layer before G.3.4.
 *          Errors here cannot be fixed cheaply after ConnectBlock exists.
 */

#include "../../include/p2p/consensus_validator.h"
#include "../../include/consensus/subsidy.h"
#include "../../include/primitives/hash_domains.h"  // Phase M.4.3-B: TxId type
#include <chrono>
#include <iostream>
#include <cassert>
#include <vector>

using namespace dinero::p2p;
using dinero::OutPoint;
using dinero::uint256;
using dinero::TxId;

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
// Helper: Create Coinbase Transaction
//=============================================================================

Transaction createCoinbase(uint64_t coinbase_value, uint32_t height) {
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.locktime = 0;

    // Coinbase input (null outpoint)
    TxIn cb_input;
    cb_input.prevout.txid = TxId(uint256()); // Phase M.4.3-B: All zeros
    cb_input.prevout.vout = 0xFFFFFFFF;

    // Encode height in scriptSig (BIP34-style)
    // Simplified: just push height as 4 bytes
    cb_input.scriptSig = {
        static_cast<uint8_t>(height & 0xFF),
        static_cast<uint8_t>((height >> 8) & 0xFF),
        static_cast<uint8_t>((height >> 16) & 0xFF),
        static_cast<uint8_t>((height >> 24) & 0xFF)
    };
    cb_input.sequence = 0xFFFFFFFF;
    coinbase.inputs.push_back(cb_input);

    // Coinbase output
    TxOut cb_output;
    cb_output.value = coinbase_value;
    cb_output.scriptPubKey = {0x51}; // OP_1 (simplified)
    coinbase.outputs.push_back(cb_output);

    return coinbase;
}

//=============================================================================
// Helper: Create Regular Transaction
//=============================================================================

Transaction createRegularTx(
    const uint256& prev_hash,
    uint32_t prev_index,
    uint64_t input_value,
    uint64_t output_value
) {
    Transaction tx;
    tx.version = 1;
    tx.locktime = 0;

    // Input
    TxIn input;
    input.prevout.txid = TxId(prev_hash);  // Phase M.4.3-B: Wrap uint256 in TxId
    input.prevout.vout = prev_index;
    input.scriptSig = {0x51}; // OP_1
    input.sequence = 0xFFFFFFFF;
    tx.inputs.push_back(input);

    // Output
    TxOut output;
    output.value = output_value;
    output.scriptPubKey = {0x51}; // OP_1
    tx.outputs.push_back(output);

    return tx;
}

//=============================================================================
// Helper: Create Block
//=============================================================================

Block createBlock(const Transaction& coinbase, const std::vector<Transaction>& txs) {
    Block block;
    block.transactions.push_back(coinbase);
    for (const auto& tx : txs) {
        block.transactions.push_back(tx);
    }
    return block;
}

//=============================================================================
// Test 1: Genesis Block Coinbase (Max Subsidy)
//=============================================================================

void test_genesis_coinbase() {
    std::cout << "\n[Test 1] Genesis block coinbase (height 0)" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view; // Empty for genesis
    ConsensusParams params;

    uint32_t height = 0;
    uint64_t subsidy = GetBlockSubsidy(height, params); // Should be 0 (genesis)

    // Valid: Coinbase claims exactly subsidy (no fees in genesis)
    auto coinbase_valid = createCoinbase(subsidy, height);
    auto block_valid = createBlock(coinbase_valid, {});

    auto result = validator.validateBlock(block_valid, height, utxo_view, params);

    assert(result.ok && "Genesis coinbase with correct subsidy should pass");
    assert(result.subsidy == subsidy && "Subsidy should match GetBlockSubsidy()");
    assert(result.total_fees == 0 && "Genesis has no fees");
    assert(result.coinbase_value == subsidy && "Coinbase value should equal subsidy");

    std::cout << "  [✓] Genesis coinbase valid (subsidy = " << subsidy << ")" << std::endl;

    // Invalid: Coinbase claims more than subsidy
    auto coinbase_overpay = createCoinbase(subsidy + 1, height);
    auto block_overpay = createBlock(coinbase_overpay, {});

    auto result_overpay = validator.validateBlock(block_overpay, height, utxo_view, params);

    assert(!result_overpay.ok && "Genesis coinbase overpay should fail");
    assert(result_overpay.error.find("subsidy") != std::string::npos ||
           result_overpay.error.find("coinbase") != std::string::npos &&
           "Error should mention subsidy or coinbase");

    std::cout << "  [✓] Genesis coinbase overpay correctly rejected" << std::endl;
}

//=============================================================================
// Test 2: Regular Block with Fees
//=============================================================================

void test_regular_block_with_fees() {
    std::cout << "\n[Test 2] Regular block with fees (height 100)" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view;
    ConsensusParams params;

    uint32_t height = 100;
    uint64_t subsidy = GetBlockSubsidy(height, params);

    // Create regular transaction with fee
    uint256 prev_hash = uint256::FromHexUnsafe("0000000000000000000000000000000000000000000000000000000000000001");
    uint64_t input_value = 100ULL * 100000000ULL; // 100 DIN
    uint64_t output_value = 99ULL * 100000000ULL; // 99 DIN
    uint64_t fee = input_value - output_value; // 1 DIN fee

    auto regular_tx = createRegularTx(prev_hash, 0, input_value, output_value);

    // Add UTXO for regular tx input
    TxOut prev_output;
    prev_output.value = input_value;
    prev_output.scriptPubKey = {0x51};
    utxo_view.addUTXO(regular_tx.inputs[0].prevout, prev_output);

    // Coinbase claims subsidy + fee
    uint64_t coinbase_value = subsidy + fee;
    auto coinbase = createCoinbase(coinbase_value, height);

    auto block = createBlock(coinbase, {regular_tx});

    auto result = validator.validateBlock(block, height, utxo_view, params);

    assert(result.ok && "Block with correct subsidy + fees should pass");
    assert(result.total_fees == fee && "Total fees should be 1 DIN");
    assert(result.coinbase_value == coinbase_value && "Coinbase should claim subsidy + fees");
    assert(result.subsidy == subsidy && "Subsidy should match height");

    std::cout << "  [✓] Block with fees validated (subsidy=" << subsidy
              << ", fees=" << fee << ")" << std::endl;

    // Invalid: Coinbase claims more than subsidy + fees
    auto coinbase_overpay = createCoinbase(subsidy + fee + 1, height);
    auto block_overpay = createBlock(coinbase_overpay, {regular_tx});

    auto result_overpay = validator.validateBlock(block_overpay, height, utxo_view, params);

    assert(!result_overpay.ok && "Coinbase overpay should fail");

    std::cout << "  [✓] Coinbase overpay correctly rejected" << std::endl;
}

//=============================================================================
// Test 3: First Halving Boundary
//=============================================================================

void test_first_halving_boundary() {
    std::cout << "\n[Test 3] First halving boundary" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view;
    ConsensusParams params;

    // PoW emission starts at height 1, so the first epoch covers heights
    // 1..HALVING_INTERVAL and the first halved subsidy is the next height.
    const uint32_t halving_interval = dinero::ConsensusSubsidy::HALVING_INTERVAL;
    const uint32_t height_before = halving_interval;
    const uint32_t height_at = halving_interval + 1;
    const uint32_t height_after = halving_interval + 2;

    uint64_t subsidy_before = GetBlockSubsidy(height_before, params); // 100 DIN
    uint64_t subsidy_at = GetBlockSubsidy(height_at, params);         // 50 DIN
    uint64_t subsidy_after = GetBlockSubsidy(height_after, params);   // 50 DIN

    // Test block before halving
    auto coinbase_before = createCoinbase(subsidy_before, height_before);
    auto block_before = createBlock(coinbase_before, {});
    auto result_before = validator.validateBlock(block_before, height_before, utxo_view, params);

    assert(result_before.ok && "Block before halving should pass");
    assert(result_before.subsidy == subsidy_before && "Subsidy before halving should be 100 DIN");

    std::cout << "  [✓] Block before halving validated (subsidy=" << subsidy_before << ")" << std::endl;

    // Test block at halving boundary
    auto coinbase_at = createCoinbase(subsidy_at, height_at);
    auto block_at = createBlock(coinbase_at, {});
    auto result_at = validator.validateBlock(block_at, height_at, utxo_view, params);

    assert(result_at.ok && "Block at halving should pass");
    assert(result_at.subsidy == subsidy_at && "Subsidy at halving should be 50 DIN");
    assert(result_at.subsidy == subsidy_before / 2 && "Subsidy should halve exactly");

    std::cout << "  [✓] Block at halving validated (subsidy=" << subsidy_at << ")" << std::endl;

    // Test block after halving
    auto coinbase_after = createCoinbase(subsidy_after, height_after);
    auto block_after = createBlock(coinbase_after, {});
    auto result_after = validator.validateBlock(block_after, height_after, utxo_view, params);

    assert(result_after.ok && "Block after halving should pass");
    assert(result_after.subsidy == subsidy_after && "Subsidy after halving should be 50 DIN");

    std::cout << "  [✓] Block after halving validated (subsidy=" << subsidy_after << ")" << std::endl;

    // Invalid: Try to claim old subsidy after halving
    auto coinbase_invalid = createCoinbase(subsidy_before, height_at); // Claim 100 DIN at halving
    auto block_invalid = createBlock(coinbase_invalid, {});
    auto result_invalid = validator.validateBlock(block_invalid, height_at, utxo_view, params);

    assert(!result_invalid.ok && "Claiming old subsidy after halving should fail");

    std::cout << "  [✓] Old subsidy claim after halving correctly rejected" << std::endl;
}

//=============================================================================
// Test 4: Tail Emission Transition
//=============================================================================

void test_tail_emission_transition() {
    std::cout << "\n[Test 4] Perpetual tail-emission transition" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view;
    ConsensusParams params;

    // Epoch 6 still pays 1.5625 DIN; epoch 7 is floored to 1 DIN and every
    // later epoch remains at that floor.
    constexpr uint32_t tail_epoch = 7;
    const uint32_t height_before = tail_epoch * dinero::ConsensusSubsidy::HALVING_INTERVAL;
    const uint32_t height_at = height_before + 1;
    const uint32_t far_future_height = 100 * dinero::ConsensusSubsidy::HALVING_INTERVAL + 1;
    const uint64_t subsidy_before = GetBlockSubsidy(height_before, params);
    const uint64_t subsidy_at = GetBlockSubsidy(height_at, params);
    const uint64_t subsidy_future = GetBlockSubsidy(far_future_height, params);

    assert(subsidy_before > dinero::ConsensusSubsidy::TAIL_EMISSION_UNA);
    assert(subsidy_at == dinero::ConsensusSubsidy::TAIL_EMISSION_UNA);
    assert(subsidy_future == dinero::ConsensusSubsidy::TAIL_EMISSION_UNA);

    auto coinbase = createCoinbase(subsidy_at, height_at);
    auto block = createBlock(coinbase, {});
    auto result = validator.validateBlock(block, height_at, utxo_view, params);
    assert(result.ok && "First tail-emission block should pass");
    assert(result.subsidy == dinero::ConsensusSubsidy::TAIL_EMISSION_UNA);

    auto zero_coinbase = createCoinbase(0, far_future_height);
    auto future_block = createBlock(zero_coinbase, {});
    auto future_result = validator.validateBlock(future_block, far_future_height, utxo_view, params);
    assert(future_result.ok && "Claiming less than the maximum subsidy remains valid");
    assert(future_result.subsidy == dinero::ConsensusSubsidy::TAIL_EMISSION_UNA);

    std::cout << "  [✓] Tail floor remains 1 DIN in the far future" << std::endl;
}

//=============================================================================
// Test 5: Tail-Emission Block with Fees
//=============================================================================

void test_tail_emission_with_fees() {
    std::cout << "\n[Test 5] Tail-emission block with fees" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view;
    ConsensusParams params;

    const uint32_t height = 100 * dinero::ConsensusSubsidy::HALVING_INTERVAL + 1;
    uint64_t subsidy = GetBlockSubsidy(height, params);
    assert(subsidy == dinero::ConsensusSubsidy::TAIL_EMISSION_UNA);

    // Create transaction with fee
    uint256 prev_hash = uint256::FromHexUnsafe("0000000000000000000000000000000000000000000000000000000000000001");
    uint64_t input_value = 10ULL * 100000000ULL;
    uint64_t output_value = 9ULL * 100000000ULL;
    uint64_t fee = input_value - output_value;

    auto regular_tx = createRegularTx(prev_hash, 0, input_value, output_value);

    TxOut prev_output;
    prev_output.value = input_value;
    prev_output.scriptPubKey = {0x51};
    utxo_view.addUTXO(regular_tx.inputs[0].prevout, prev_output);

    // Coinbase claims the perpetual tail subsidy plus fees.
    auto coinbase = createCoinbase(subsidy + fee, height);
    auto block = createBlock(coinbase, {regular_tx});

    auto result = validator.validateBlock(block, height, utxo_view, params);

    assert(result.ok && "Tail-emission block with fees should pass");
    assert(result.subsidy == dinero::ConsensusSubsidy::TAIL_EMISSION_UNA);
    assert(result.total_fees == fee && "Fees should be accumulated");
    assert(result.coinbase_value == subsidy + fee && "Coinbase should claim subsidy plus fees");

    std::cout << "  [✓] Tail subsidy plus fees validated (fees=" << fee << ")" << std::endl;
}

//=============================================================================
// Test 6: Overflow Protection
//=============================================================================

void test_overflow_protection() {
    std::cout << "\n[Test 6] Overflow protection" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view;
    ConsensusParams params;

    uint32_t height = 100;

    // Test: Coinbase value overflow (UINT64_MAX)
    auto coinbase_overflow = createCoinbase(UINT64_MAX, height);
    auto block_overflow = createBlock(coinbase_overflow, {});

    auto result_overflow = validator.validateBlock(block_overflow, height, utxo_view, params);

    assert(!result_overflow.ok && "Coinbase with UINT64_MAX should fail");
    assert(result_overflow.error.find("overflow") != std::string::npos ||
           result_overflow.error.find("subsidy") != std::string::npos &&
           "Error should mention overflow or subsidy");

    std::cout << "  [✓] Coinbase overflow correctly rejected" << std::endl;

    // Test: Transaction fee overflow
    uint256 prev_hash = uint256::FromHexUnsafe("0000000000000000000000000000000000000000000000000000000000000001");

    // Create tx with potential overflow: input = UINT64_MAX, output = 1
    // Fee would be UINT64_MAX - 1, but accumulation could overflow
    auto tx_overflow = createRegularTx(prev_hash, 0, UINT64_MAX, 1);

    TxOut prev_output;
    prev_output.value = UINT64_MAX;
    prev_output.scriptPubKey = {0x51};
    utxo_view.addUTXO(tx_overflow.inputs[0].prevout, prev_output);

    uint64_t subsidy = GetBlockSubsidy(height, params);
    auto coinbase = createCoinbase(subsidy + (UINT64_MAX - 1), height); // Overflow risk

    auto block = createBlock(coinbase, {tx_overflow});

    auto result = validator.validateBlock(block, height, utxo_view, params);

    // Should fail due to overflow in fee accumulation or coinbase value
    assert(!result.ok && "Block with overflow risk should fail");

    std::cout << "  [✓] Fee overflow correctly rejected" << std::endl;
}

//=============================================================================
// Test 7: Multiple Transactions Fee Accumulation
//=============================================================================

void test_multi_tx_fee_accumulation() {
    std::cout << "\n[Test 7] Multiple transactions fee accumulation" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view;
    ConsensusParams params;

    uint32_t height = 100;
    uint64_t subsidy = GetBlockSubsidy(height, params);

    // Create 3 transactions, each with 1 DIN fee
    std::vector<Transaction> txs;
    uint64_t total_fees = 0;

    for (int i = 0; i < 3; i++) {
        uint256 prev_hash = uint256::FromHexUnsafe("000000000000000000000000000000000000000000000000000000000000000" + std::to_string(i));
        uint64_t input_value = 10ULL * 100000000ULL;
        uint64_t output_value = 9ULL * 100000000ULL;
        uint64_t fee = input_value - output_value;

        auto tx = createRegularTx(prev_hash, i, input_value, output_value);
        txs.push_back(tx);

        TxOut prev_output;
        prev_output.value = input_value;
        prev_output.scriptPubKey = {0x51};
        utxo_view.addUTXO(tx.inputs[0].prevout, prev_output);

        total_fees += fee;
    }

    // Coinbase claims subsidy + accumulated fees
    auto coinbase = createCoinbase(subsidy + total_fees, height);
    auto block = createBlock(coinbase, txs);

    auto result = validator.validateBlock(block, height, utxo_view, params);

    assert(result.ok && "Block with multiple txs should pass");
    assert(result.total_fees == total_fees && "Fees should accumulate correctly");
    assert(result.coinbase_value == subsidy + total_fees && "Coinbase should claim all fees");

    std::cout << "  [✓] Multi-tx fee accumulation validated (total_fees=" << total_fees << ")" << std::endl;
}

//=============================================================================
// Test 8: Deterministic Validation
//=============================================================================

void test_deterministic_validation() {
    std::cout << "\n[Test 8] Validation is deterministic" << std::endl;

    ConsensusValidator validator;
    MockUTXOSnapshot utxo_view;
    ConsensusParams params;

    uint32_t height = 100;
    uint64_t subsidy = GetBlockSubsidy(height, params);

    auto coinbase = createCoinbase(subsidy, height);
    auto block = createBlock(coinbase, {});

    // Validate twice
    auto result1 = validator.validateBlock(block, height, utxo_view, params);
    auto result2 = validator.validateBlock(block, height, utxo_view, params);

    assert(result1.ok == result2.ok && "Results should match");
    assert(result1.total_fees == result2.total_fees && "Fees should match");
    assert(result1.subsidy == result2.subsidy && "Subsidy should match");
    assert(result1.coinbase_value == result2.coinbase_value && "Coinbase value should match");

    std::cout << "  [✓] Validation is deterministic" << std::endl;
}

//=============================================================================
// Main Test Runner
//=============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "G.3.3 Extension: Subsidy Validation Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nTESTS WRITTEN FIRST - NO IMPLEMENTATION YET" << std::endl;
    std::cout << "\nPure consensus evaluation | Read-only UTXO | NO state mutation" << std::endl;

    auto start = std::chrono::steady_clock::now();

    try {
        // Test 1: Genesis coinbase
        test_genesis_coinbase();

        // Test 2: Regular block with fees
        test_regular_block_with_fees();

        // Test 3: First halving boundary
        test_first_halving_boundary();

        // Test 4: Tail-emission transition
        test_tail_emission_transition();

        // Test 5: Tail emission with fees
        test_tail_emission_with_fees();

        // Test 6: Overflow protection
        test_overflow_protection();

        // Test 7: Multi-tx fee accumulation
        test_multi_tx_fee_accumulation();

        // Test 8: Deterministic
        test_deterministic_validation();

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        std::cout << "\n========================================" << std::endl;
        std::cout << "✅ All Subsidy Validation Tests Passed!" << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "\nRuntime: " << duration.count() << " ms" << std::endl;

        if (duration.count() < 500) {
            std::cout << "[✓] Fast: < 500ms requirement met" << std::endl;
        } else {
            std::cout << "[!] Warning: Exceeded 500ms target" << std::endl;
        }

        std::cout << "\nSummary:" << std::endl;
        std::cout << "  [✓] Genesis coinbase validated" << std::endl;
        std::cout << "  [✓] Regular block with fees" << std::endl;
        std::cout << "  [✓] First halving boundary" << std::endl;
        std::cout << "  [✓] Perpetual tail-emission transition" << std::endl;
        std::cout << "  [✓] Tail subsidy with fees" << std::endl;
        std::cout << "  [✓] Overflow protection" << std::endl;
        std::cout << "  [✓] Multi-tx fee accumulation" << std::endl;
        std::cout << "  [✓] Deterministic validation" << std::endl;

        std::cout << "\n⚠️  NEXT STEP: Implement G.3.3 extension to make tests pass" << std::endl;
        std::cout << "    Required: validateBlock(), TxValidationResult extensions" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ Test failed with unknown exception" << std::endl;
        return 1;
    }
}
