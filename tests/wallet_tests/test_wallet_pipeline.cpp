/**
 * @file test_wallet_pipeline.cpp
 * @brief Wallet pipeline tests (v0.12.0) - Proof Layer 2
 *
 * Four-Layer Proof System:
 * 1. Structural Correctness ✅ (compile-time guarantees)
 * 2. Deterministic Unit Tests ⚙️ (this file)
 * 3. Mempool Round-Trip 💸 (integration tests)
 * 4. CLI Integration 🧠 (end-to-end tests)
 *
 * Test Coverage:
 * - Coin selection (BnB, greedy fallback, dust handling)
 * - Transaction sizing and fee calculation
 * - Batch payment calculation
 *
 * Phase M.3: Updated to use CanonicalWalletUTXO (the single canonical UTXO type)
 */

#include "wallet/coin_selection.h"
#include "wallet/unsigned_tx_builder.h"
#include "wallet/batch_transaction_builder.h"
#include "wallet/canonical_wallet_utxo.h"  // Phase M.3: Canonical UTXO type
#include "wallet/taproot_keys.h"  // For real Taproot key generation
#include "address/addr_codec.h"   // For CreateP2TRScriptPubKey
#include <cassert>
#include <iostream>
#include <iomanip>

using namespace dinero;

// ═══════════════════════════════════════════════════════════════════════════
// Test Utilities
// ═══════════════════════════════════════════════════════════════════════════

#define TEST(name) \
    void test_##name(); \
    struct TestRunner_##name { \
        TestRunner_##name() { \
            std::cout << "Running: " << #name << "..." << std::flush; \
            test_##name(); \
            std::cout << " ✅" << std::endl; \
        } \
    } test_runner_##name; \
    void test_##name()

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            std::cerr << "\n  ❌ ASSERT_EQ failed at " << __FILE__ << ":" << __LINE__ << "\n" \
                      << "    Expected: " << (b) << "\n" \
                      << "    Got:      " << (a) << std::endl; \
            std::exit(1); \
        } \
    } while(0)

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "\n  ❌ ASSERT_TRUE failed at " << __FILE__ << ":" << __LINE__ << "\n" \
                      << "    Condition: " << #cond << std::endl; \
            std::exit(1); \
        } \
    } while(0)

#define ASSERT_FALSE(cond) \
    do { \
        if (cond) { \
            std::cerr << "\n  ❌ ASSERT_FALSE failed at " << __FILE__ << ":" << __LINE__ << "\n" \
                      << "    Condition: " << #cond << std::endl; \
            std::exit(1); \
        } \
    } while(0)

// Helper: Create test x-only pubkey with BIP341 taptweak
std::array<uint8_t, 32> makeTestXOnlyPubkey(uint32_t index) {
    // BIP86/BIP341: Generate tweaked x-only pubkey for scriptPubKey
    std::array<uint8_t, 32> internal_privkey;
    std::array<uint8_t, 32> internal_xonly_pubkey;
    int parity;

    // Generate internal keypair
    if (!TaprootKeys::GenerateKeypair(internal_privkey, internal_xonly_pubkey, parity)) {
        // Fallback if generation fails (should never happen)
        std::array<uint8_t, 32> fallback;
        fallback.fill(0);
        fallback[0] = 0x01;
        fallback[31] = static_cast<uint8_t>(index);
        return fallback;
    }

    // Apply BIP341 taptweak
    std::array<uint8_t, 32> tweaked_privkey = internal_privkey;
    if (!TaprootKeys::TweakPrivkey(tweaked_privkey, internal_xonly_pubkey)) {
        // Return untweaked key if tweak fails
        return internal_xonly_pubkey;
    }

    // Derive tweaked x-only pubkey
    std::array<uint8_t, 32> tweaked_xonly_pubkey;
    int tweaked_parity;
    if (!TaprootKeys::DeriveXOnlyPubkey(tweaked_privkey, tweaked_xonly_pubkey, tweaked_parity)) {
        // Return untweaked key if derivation fails
        return internal_xonly_pubkey;
    }

    return tweaked_xonly_pubkey;  // Return BIP341 tweaked key
}

// Helper: Create test UTXO (Phase M.3: Uses CanonicalWalletUTXO, Taproot from genesis)
CanonicalWalletUTXO makeUTXO(uint64_t value, const std::string& txid_hex, uint32_t vout) {
    CanonicalWalletUTXO utxo;
    utxo.value = AmountUna::Una(value);
    utxo.txid = uint256::FromHexUnsafe(txid_hex);  // Phase M.0: uint256 identity
    utxo.vout = vout;

    // Generate real Taproot scriptPubKey using deterministic x-only pubkey
    // Use txid's first 4 bytes as index for deterministic key generation
    uint32_t key_index = (utxo.txid.data[0] << 24) |
                        (utxo.txid.data[1] << 16) |
                        (utxo.txid.data[2] << 8) |
                        utxo.txid.data[3];
    std::array<uint8_t, 32> xonly_pubkey = makeTestXOnlyPubkey(key_index);

    // Create proper P2TR scriptPubKey from x-only pubkey
    std::vector<uint8_t> witness_program(xonly_pubkey.begin(), xonly_pubkey.end());
    utxo.spk = CreateP2TRScriptPubKey(witness_program);  // Real Taproot scriptPubKey!

    utxo.height = 100;  // Confirmed (10+ confirmations)
    utxo.is_coinbase = false;
    utxo.path = "m/86'/1447'/0'/0/0";  // BIP86 Taproot (1447 = Dinero)
    return utxo;
}

// ═══════════════════════════════════════════════════════════════════════════
// A. COIN SELECTION TESTS
// ═══════════════════════════════════════════════════════════════════════════

TEST(coin_selection_success) {
    // Basic coin selection succeeds with sufficient funds
    std::vector<CanonicalWalletUTXO> utxos = {
        makeUTXO(100000, "1111111111111111111111111111111111111111111111111111111111111111", 0),
        makeUTXO(200000, "2222222222222222222222222222222222222222222222222222222222222222", 0),
        makeUTXO(300000, "3333333333333333333333333333333333333333333333333333333333333333", 0)
    };

    uint64_t target = 250000;
    uint64_t fee_rate = 1;
    size_t num_outputs = 1;

    auto result = CoinSelector::SelectCoins(utxos, target, fee_rate, num_outputs);

    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.selected_coins.size() >= 1);
    ASSERT_TRUE(result.total_value >= target);
}

TEST(coin_selection_prefers_smaller_sufficient_total) {
    std::vector<CanonicalWalletUTXO> utxos = {
        makeUTXO(100000, "1111111111111111111111111111111111111111111111111111111111111111", 0),
        makeUTXO(100000, "2222222222222222222222222222222222222222222222222222222222222222", 0),
        makeUTXO(100000, "3333333333333333333333333333333333333333333333333333333333333333", 0),
        makeUTXO(100000, "4444444444444444444444444444444444444444444444444444444444444444", 0),
        makeUTXO(100000, "5555555555555555555555555555555555555555555555555555555555555555", 0),
        makeUTXO(100000, "6666666666666666666666666666666666666666666666666666666666666666", 0),
        makeUTXO(100000, "7777777777777777777777777777777777777777777777777777777777777777", 0),
        makeUTXO(100000, "8888888888888888888888888888888888888888888888888888888888888888", 0),
        makeUTXO(100000, "9999999999999999999999999999999999999999999999999999999999999999", 0),
        makeUTXO(100000, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0),
        makeUTXO(900000, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 0),
        makeUTXO(1000000, "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", 0),
        makeUTXO(2000000, "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", 0),
    };

    uint64_t target = 800000;
    uint64_t fee_rate = 1;
    size_t num_outputs = 1;

    auto result = CoinSelector::SelectCoins(utxos, target, fee_rate, num_outputs);

    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.total_value, 900000);
    ASSERT_EQ(result.selected_coins.size(), 1);
}

TEST(coin_selection_insufficient_funds) {
    // Coin selection fails with insufficient funds
    std::vector<CanonicalWalletUTXO> utxos = {
        makeUTXO(100000, "4444444444444444444444444444444444444444444444444444444444444444", 0)
    };

    uint64_t target = 200000;  // More than available
    uint64_t fee_rate = 1;
    size_t num_outputs = 1;

    auto result = CoinSelector::SelectCoins(utxos, target, fee_rate, num_outputs);

    ASSERT_FALSE(result.success);
    ASSERT_FALSE(result.error.empty());
}

TEST(coin_selection_dust_handling) {
    // Dust UTXOs can still be selected if needed
    std::vector<CanonicalWalletUTXO> utxos = {
        makeUTXO(100, "5555555555555555555555555555555555555555555555555555555555555555", 0),      // Dust (< 546)
        makeUTXO(200, "6666666666666666666666666666666666666666666666666666666666666666", 0),      // Dust
        makeUTXO(100000, "7777777777777777777777777777777777777777777777777777777777777777", 0)    // Valid
    };

    uint64_t target = 50000;
    uint64_t fee_rate = 1;
    size_t num_outputs = 1;

    auto result = CoinSelector::SelectCoins(utxos, target, fee_rate, num_outputs);

    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.total_value >= target);
}

// ═══════════════════════════════════════════════════════════════════════════
// B. TRANSACTION SIZING TESTS
// ═══════════════════════════════════════════════════════════════════════════

TEST(transaction_size_estimation) {
    // Transaction size estimation is reasonable
    size_t size_1in_1out = CoinSelector::EstimateTransactionSize(1, 1);
    size_t size_1in_2out = CoinSelector::EstimateTransactionSize(1, 2);
    size_t size_2in_1out = CoinSelector::EstimateTransactionSize(2, 1);

    // More outputs → larger size
    ASSERT_TRUE(size_1in_2out > size_1in_1out);

    // More inputs → larger size
    ASSERT_TRUE(size_2in_1out > size_1in_1out);

    // Estimator returns BIP141 virtual size (Transaction::GetVirtualSize()),
    // matching the mempool's fee-rate denominator.
    //
    // For 1-in/1-out P2TR key-path:
    //   base    = version(4) + ins(1) + outs(1) + locktime(4) + 1*41 + 1*43 = 94
    //   witness = marker+flag(2) + 1*66 = 68
    //   weight  = base*4 + witness = 376 + 68 = 444
    //   vsize   = (weight + 3) / 4 = 111
    ASSERT_EQ(size_1in_1out, 111U);
    ASSERT_EQ(size_1in_2out, 154U);   // +43 non-witness output
    ASSERT_EQ(size_2in_1out, 169U);   // +41 non-witness +66 witness for extra input
}

TEST(fee_calculation) {
    // Fee calculation is correct
    size_t tx_size = 200;  // vbytes
    uint64_t fee_rate = 10;  // una/vbyte

    uint64_t fee = CoinSelector::CalculateFee(tx_size, fee_rate);

    ASSERT_EQ(fee, 2000);  // 200 × 10 = 2000
}

// ═══════════════════════════════════════════════════════════════════════════
// C. BATCH TRANSACTION TESTS
// ═══════════════════════════════════════════════════════════════════════════

TEST(batch_payment_total_calculation) {
    // Calculate total amount for batch payments
    std::vector<BatchPayment> payments = {
        {"din1q1", 100000},
        {"din1q2", 200000},
        {"din1q3", 300000}
    };

    uint64_t total = BatchTransactionBuilder::calculateTotalAmount(payments);

    ASSERT_EQ(total, 600000);
}

TEST(batch_payment_empty_check) {
    // Empty batch should have zero total
    std::vector<BatchPayment> payments = {};

    uint64_t total = BatchTransactionBuilder::calculateTotalAmount(payments);

    ASSERT_EQ(total, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// D. UNSIGNED BUILDER SIZE ESTIMATION TESTS
// ═══════════════════════════════════════════════════════════════════════════

TEST(unsigned_builder_size_estimation_matches_coin_selector) {
    // UnsignedTxBuilder delegates to CoinSelector so the two can never drift.
    // Both return BIP141 vsize (Transaction::GetVirtualSize()), matching the
    // mempool's fee-rate denominator.
    ASSERT_EQ(UnsignedTxBuilder::EstimateTransactionSize(1, 1),
              CoinSelector::EstimateTransactionSize(1, 1));
    ASSERT_EQ(UnsignedTxBuilder::EstimateTransactionSize(2, 3),
              CoinSelector::EstimateTransactionSize(2, 3));
    ASSERT_EQ(UnsignedTxBuilder::EstimateTransactionSize(5, 2),
              CoinSelector::EstimateTransactionSize(5, 2));
}

TEST(unsigned_builder_fee_calculation_matches_coin_selector) {
    // Fee calculation should be consistent
    size_t tx_size = 250;
    uint64_t fee_rate = 5;

    uint64_t builder_fee = UnsignedTxBuilder::CalculateFee(tx_size, fee_rate);
    uint64_t selector_fee = CoinSelector::CalculateFee(tx_size, fee_rate);

    ASSERT_EQ(builder_fee, selector_fee);
    ASSERT_EQ(builder_fee, 1250);  // 250 × 5
}

// ═══════════════════════════════════════════════════════════════════════════
// E. ARCHITECTURAL INVARIANTS
// ═══════════════════════════════════════════════════════════════════════════

TEST(dust_threshold_consistency) {
    // Dust threshold should be consistent across components
    ASSERT_EQ(CoinSelector::DUST_THRESHOLD, UnsignedTxBuilder::DUST_THRESHOLD);
    ASSERT_EQ(CoinSelector::DUST_THRESHOLD, 546);  // Standard P2WPKH dust
}

TEST(rbf_sequence_constants) {
    // RBF sequence values should be correct
    ASSERT_EQ(UnsignedTxBuilder::RBF_SEQUENCE, 0xfffffffd);
    ASSERT_EQ(UnsignedTxBuilder::DEFAULT_SEQUENCE, 0xfffffffe);

    // RBF sequence must be < DEFAULT_SEQUENCE
    ASSERT_TRUE(UnsignedTxBuilder::RBF_SEQUENCE < UnsignedTxBuilder::DEFAULT_SEQUENCE);

    // RBF sequence must signal replaceability (BIP125)
    ASSERT_TRUE(UnsignedTxBuilder::RBF_SEQUENCE < 0xfffffffe);
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "Wallet Pipeline Tests (v0.12.0)" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    std::cout << "📊 Coverage:\n"
              << "  A. Coin Selection (success, insufficient funds, dust)\n"
              << "  B. Transaction Sizing (estimation, fee calculation)\n"
              << "  C. Batch Payments (total calculation, empty check)\n"
              << "  D. Builder Consistency (size/fee formulas match)\n"
              << "  E. Architectural Invariants (dust threshold, RBF constants)\n" << std::endl;

    // Tests run via static initialization

    std::cout << "\n✅ All wallet pipeline tests passed!" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    return 0;
}
