/**
 * Test T24: Reorg + Coinbase Maturity Regression
 *
 * Phase F.11: Reorg Safety Across Mining & Wallet
 *
 * Validates: I.11.3 - Coinbase Maturity Reorg Transition
 *
 * "When a reorg moves the chain tip below a coinbase's maturity threshold
 *  (100 confirmations), that coinbase MUST become immature again. Transactions
 *  spending it MUST be rejected."
 *
 * Test Flow:
 * 1. Create coinbase UTXO at height 1
 * 2. Test at height 101 (101 confirmations - MATURE)
 *    - Create transaction spending coinbase
 *    - Submit to mempool → MUST ACCEPT
 * 3. Simulate reorg to height 51 (51 confirmations - IMMATURE)
 *    - Re-submit same transaction to mempool → MUST REJECT
 * 4. Verify maturity threshold enforced correctly
 *
 * Expected Result: ✅ Coinbase maturity flips correctly: mature → immature
 *
 * Architectural Principle:
 * - Mempool validates against current chain height
 * - Coinbase maturity = (current_height - coinbase_height + 1)
 * - Maturity changes when chain height changes (reorg)
 * - This proves consensus correctness across reorgs
 */

#include "mempool/mempool.h"
#include "consensus/coins_db.h"
#include "primitives/transaction.h"
#include "crypto/hash.h"
#include <chrono>
#include <iostream>
#include <filesystem>
#include <cstring>
#include <memory>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <ctime>

namespace fs = std::filesystem;

using dinero::Transaction;
using dinero::TxOutput;
using dinero::TxInput;
using dinero::TxOutPoint;
using dinero::uint256;
using dinero::OutPoint;
using dinero::TxId;
using dinero::AmountUna;

// Helper: Create coinbase transaction
Transaction createCoinbase(uint32_t height, const std::vector<uint8_t>& scriptPubKey, uint64_t amount) {
    Transaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout = TxOutPoint();
    coinbase.vin[0].prevout.vout = 0xffffffff;
    coinbase.vin[0].scriptSig = {0x01, static_cast<uint8_t>(height)};

    TxOutput output;
    output.value = AmountUna::Una(amount);
    output.scriptPubKey = scriptPubKey;
    coinbase.vout.push_back(output);

    return coinbase;
}

int main() {
    std::cout << "========================================\\n";
    std::cout << "Test T24: Reorg + Coinbase Maturity Regression\\n";
    std::cout << "========================================\\n\\n";

    std::cout << "Invariant: I.11.3 - Coinbase Maturity Reorg Transition\\n";
    std::cout << "\\\"When a reorg moves the chain tip below coinbase maturity\\n";
    std::cout << " threshold, the coinbase MUST become immature.\\\"\\n\\n";

    std::cout << "Architectural Principle:\\n";
    std::cout << "- Mempool validates against current chain height\\n";
    std::cout << "- Maturity = (current_height - coinbase_height + 1)\\n";
    std::cout << "- Reorg changes chain height → maturity flips\\n\\n";

    // Setup test directory (portable; replaces POSIX mkdtemp).
    auto unique_suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    fs::path test_dir = fs::temp_directory_path() /
        ("dinero_test_t24_standalone_" + unique_suffix);
    std::error_code _ec;
    fs::create_directories(test_dir, _ec);
    if (_ec) {
        std::cerr << "Failed to create temp directory: " << _ec.message() << "\\n";
        return 1;
    }
    fs::path coinsdb_path = test_dir / "coinsdb";

    std::cout << "[SETUP] Test directory: " << test_dir << "\\n";
    std::cout << "[SETUP] CoinsDB path: " << coinsdb_path << "\\n\\n";

    try {
        // ========================================
        // T24.1: Initialize CoinsDB
        // ========================================
        std::cout << "[T24.1] Initializing CoinsDB...\\n";

        dinero::consensus::CoinsDB coins_db;
        coins_db.open(coinsdb_path.string());
        dinero::consensus::CoinsViewCache view(&coins_db);

        std::cout << "✓ CoinsDB initialized\\n\\n";

        // ========================================
        // T24.2: Create coinbase at height 1
        // ========================================
        std::cout << "[T24.2] Creating coinbase at height 1...\\n";

        uint64_t coinbase_amount = 100ULL * 100000000ULL;
        std::vector<uint8_t> script_coinbase = {0x76, 0xa9, 0x14};
        for (int i = 0; i < 20; i++) script_coinbase.push_back(0xAA);
        script_coinbase.push_back(0x88);
        script_coinbase.push_back(0xac);

        Transaction coinbase = createCoinbase(1, script_coinbase, coinbase_amount);
        TxId coinbase_txid = coinbase.GetTxid();

        // Add coinbase UTXO to CoinsDB
        OutPoint coinbase_outpoint(coinbase_txid, 0);
        dinero::consensus::UTXOEntry coinbase_coin;
        coinbase_coin.value = AmountUna::Una(coinbase_amount);
        coinbase_coin.scriptPubKey = script_coinbase;
        coinbase_coin.isCoinbase = true;
        coinbase_coin.height = 1;
        view.addCoin(coinbase_outpoint, coinbase_coin);

        std::cout << "✓ Coinbase created at height 1\\n";
        std::cout << "  Txid: " << coinbase_txid.v.GetHex() << "\\n";
        std::cout << "  Amount: 100 DIN\\n\\n";

        // ========================================
        // T24.3: Create transaction spending coinbase
        // ========================================
        std::cout << "[T24.3] Creating transaction spending coinbase...\\n";

        Transaction spend_tx;
        spend_tx.version = 2;
        spend_tx.witness_version = 1;
        spend_tx.lockTime = 0;

        TxInput input;
        input.prevout = TxOutPoint(coinbase_txid, 0);
        input.sequence = 0xfffffffe;
        spend_tx.vin.push_back(input);

        uint64_t fee = 1000000ULL;  // 0.01 DIN
        TxOutput output;
        output.value = AmountUna::Una(coinbase_amount - fee);
        output.scriptPubKey = {0x76, 0xa9, 0x14};
        for (int i = 0; i < 20; i++) output.scriptPubKey.push_back(0xBB);
        output.scriptPubKey.push_back(0x88);
        output.scriptPubKey.push_back(0xac);
        spend_tx.vout.push_back(output);

        TxId spend_txid = spend_tx.GetTxid();

        std::cout << "✓ Transaction created\\n";
        std::cout << "  Txid: " << spend_txid.v.GetHex() << "\\n";
        std::cout << "  Spends: Coinbase from height 1\\n\\n";

        // ========================================
        // T24.4: Test at height 101 (MATURE)
        // ========================================
        std::cout << "[T24.4] Testing at height 101 (coinbase MATURE)...\\n";
        std::cout << "  Coinbase height: 1\\n";
        std::cout << "  Current height: 101\\n";
        std::cout << "  Confirmations: 101 (>= 100 required)\\n";
        std::cout << "  Expected: ACCEPT\\n\\n";

        dinero::mempool::MempoolConfig mempool_config;
        mempool_config.min_fee_rate = 1.0;
        mempool_config.max_ancestors = 25;
        mempool_config.max_descendants = 25;

        dinero::mempool::Mempool mempool(mempool_config);

        uint32_t height_mature = 101;
        auto result_mature = mempool.submitTransaction(
            spend_tx,
            view,
            height_mature,
            std::time(nullptr),
            dinero::mempool::MempoolSubmitMode::TEST_ONLY  // Skip script validation, test maturity only
        );

        std::cout << "  Mempool result: " << dinero::mempool::MempoolAcceptResultToString(result_mature) << "\\n\\n";

        if (result_mature != dinero::mempool::MempoolAcceptResult::OK) {
            std::cerr << "❌ FAIL - Mempool should ACCEPT mature coinbase spend\\n";
            std::cerr << "  Coinbase at height 1 with 101 confirmations is MATURE\\n";
            return 1;
        }

        std::cout << "✓ Mempool ACCEPTED spend (coinbase is mature)\\n";
        std::cout << "  Mempool size: " << mempool.getCount() << "\\n\\n";

        // ========================================
        // T24.5: Simulate reorg to height 51 (IMMATURE)
        // ========================================
        std::cout << "[T24.5] Simulating reorg to height 51 (coinbase IMMATURE)...\\n";
        std::cout << "  Coinbase height: 1\\n";
        std::cout << "  New height: 51\\n";
        std::cout << "  Confirmations: 51 (< 100 required)\\n";
        std::cout << "  Expected: REJECT\\n\\n";

        // Clear mempool (simulating reorg)
        dinero::mempool::Mempool mempool_after_reorg(mempool_config);

        uint32_t height_immature = 51;
        auto result_immature = mempool_after_reorg.submitTransaction(
            spend_tx,
            view,
            height_immature,
            std::time(nullptr),
            dinero::mempool::MempoolSubmitMode::TEST_ONLY  // Skip script validation, test maturity only
        );

        std::cout << "  Mempool result: " << dinero::mempool::MempoolAcceptResultToString(result_immature) << "\\n\\n";

        if (result_immature == dinero::mempool::MempoolAcceptResult::OK) {
            std::cerr << "❌ FAIL - Mempool should REJECT immature coinbase spend\\n";
            std::cerr << "  Coinbase at height 1 with 51 confirmations is IMMATURE\\n";
            return 1;
        }

        std::cout << "✓ Mempool REJECTED spend (coinbase is immature)\\n";
        std::cout << "  Mempool size: " << mempool_after_reorg.getCount() << "\\n\\n";

        // ========================================
        // T24.6: Verify maturity transition
        // ========================================
        std::cout << "[T24.6] Verifying coinbase maturity transition...\\n\\n";

        std::cout << "✅ Maturity Transition Validated:\\n";
        std::cout << "  At height 101: Coinbase MATURE → spend ACCEPTED ✓\\n";
        std::cout << "  At height 51: Coinbase IMMATURE → spend REJECTED ✓\\n";
        std::cout << "  Maturity flips correctly with chain height ✓\\n\\n";

        // ========================================
        // Summary
        // ========================================
        std::cout << "✅ PASS - Reorg changes coinbase maturity (I.11.3 validated)\\n\\n";

        std::cout << "I.11.3 Invariant Satisfied:\\n";
        std::cout << "\\\"When a reorg moves the chain tip below coinbase maturity\\n";
        std::cout << " threshold, the coinbase MUST become immature.\\\"\\n\\n";

        std::cout << "Consensus Correctness Demonstrated:\\n";
        std::cout << "- Coinbase created at height 1 ✓\\n";
        std::cout << "- Mature at height 101 (101 confirmations) ✓\\n";
        std::cout << "- Immature at height 51 (51 confirmations) ✓\\n";
        std::cout << "- Mempool enforces maturity at current height ✓\\n";
        std::cout << "- Reorg safety validated ✓\\n\\n";

        std::cout << "Note: This validates coinbase maturity across chain reorganizations.\\n";
        std::cout << "      Mempool correctly validates against current chain height,\\n";
        std::cout << "      preventing immature coinbase spends after reorgs.\\n";

        // Cleanup (non-throwing — RocksDB lock files keep the dir locked on
        // Windows until the still-alive CoinsDB drops at end of try block).
        std::error_code rm_ec;
        fs::remove_all(test_dir, rm_ec);

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\\n";
        std::error_code rm_ec;
        fs::remove_all(test_dir, rm_ec);
        return 1;
    }
}
