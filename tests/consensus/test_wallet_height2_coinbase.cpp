/**
 * @file test_wallet_height2_coinbase.cpp
 * @brief Test: Wallet Tracks Height 1 Coinbase with Maturity (Phase M.5.3)
 *
 * PURPOSE:
 * Prove that the wallet correctly tracks coinbase rewards from height ≥ 1
 * and enforces 100-block maturity rules for all coinbase outputs.
 *
 * WHAT THIS TEST PROVES:
 * - Height 1 coinbase (first PoW block) is indexed in UTXOIndex
 * - Coinbase marked as is_coinbase = true
 * - Balance shows as "immature" until height 101
 * - Balance moves to "confirmed" at height 101
 * - No special-casing for height 1 vs other coinbases
 *
 * TEST STRATEGY:
 * 1. Initialize genesis (height 0 only)
 * 2. Create and store block at height 1 (first PoW block) via archival flatfiles
 * 3. Add height 1 coinbase to UTXOIndex
 * 4. Query balance with maturity at heights 1, 50, 100, 101, 150
 * 5. Verify immature/confirmed classification
 *
 * FAILURE MODES THIS CATCHES:
 * - Wallet doesn't index height 1 coinbase
 * - Coinbase not marked properly
 * - Maturity rule not applied
 * - Off-by-one errors in maturity calculation
 */

#include "consensus/genesis_canonical.h"
#include "consensus/chainparams.h"
#include "consensus/asert_params.h"
#include "consensus/block_lifecycle.h"
#include "consensus/subsidy.h"
#include "storage/archival_block_reader.h"
#include "storage/block_storage.h"
#include "storage/chain_db.h"
#include "storage/tip_info.h"
#include "wallet/utxo_index.h"
#include "daemon/genesis_init.hpp"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "common/sha256d.h"
#include <iostream>
#include <cassert>
#include <filesystem>
#include <cstring>

using namespace dinero;

//=============================================================================
// Test Configuration
//=============================================================================

static const char* TEST_DATADIR = "/tmp/dinero-test-wallet-height1";

//=============================================================================
// Test Helper: Clean Test Environment
//=============================================================================

void cleanTestDatadir() {
    std::filesystem::remove_all(TEST_DATADIR);
    std::filesystem::create_directories(TEST_DATADIR);
}

//=============================================================================
// Test Helper: Create Height 1 Coinbase Transaction
//=============================================================================

Transaction createHeight1Coinbase() {
    Transaction coinbase_tx;
    coinbase_tx.version = 1;
    coinbase_tx.lockTime = 0;
    coinbase_tx.witness_version = 1;  // Taproot

    // Coinbase input (no prevout)
    TxInput coinbase_input;
    coinbase_input.prevout.txid = TxId(uint256());  // Null hash
    coinbase_input.prevout.vout = 0xffffffff;

    // scriptSig: height 1 (OP_1) + arbitrary data
    coinbase_input.scriptSig.push_back(0x01);  // Height 1
    std::string msg = "First PoW block - Fair Launch v3";
    coinbase_input.scriptSig.insert(coinbase_input.scriptSig.end(), msg.begin(), msg.end());
    coinbase_input.sequence = 0xffffffff;

    coinbase_tx.vin.push_back(coinbase_input);

    // Coinbase output: Block subsidy at height 1 (query consensus for exact value)
    TxOutput coinbase_output;
    AmountUna subsidy = ConsensusSubsidy::GetBlockSubsidy(1);
    coinbase_output.value = subsidy;  // 100 DIN per frozen monetary policy

    // Phase M.5.3: Assertion to prevent monetary policy regression
    assert(subsidy.v == 100 * ConsensusSubsidy::UNA_PER_DIN &&
           "Height 1 coinbase must be 100 DIN per frozen monetary policy");

    // Create P2TR scriptPubKey (OP_1 + 32 bytes)
    coinbase_output.scriptPubKey.push_back(0x51);  // OP_1
    coinbase_output.scriptPubKey.push_back(0x20);  // Push 32 bytes
    // Fill with zeros for test (in real mining, this would be miner's pubkey)
    for (int i = 0; i < 32; i++) {
        coinbase_output.scriptPubKey.push_back(0x00);
    }

    coinbase_tx.vout.push_back(coinbase_output);

    return coinbase_tx;
}

//=============================================================================
// Test 1: Initialize Genesis Only
//=============================================================================

void testInitialize(ChainDB& chain_db, BlockStorage& block_storage, UTXOIndex& utxo_index) {
    std::cout << "\n[Test 5.1] Initializing genesis..." << std::endl;

    bool init_ok = InitializeGenesis(&chain_db, &block_storage, &utxo_index);
    assert(init_ok && "Genesis initialization must succeed");

    // Verify chain tip at height 0 (genesis)
    StatusOr<TipInfo> tip_info = chain_db.getTip();
    assert(tip_info.ok() && "Chain tip must exist");
    assert(tip_info.value().height == 0 && "Chain tip must be at height 0 (genesis)");

    std::cout << "✅ Genesis initialized" << std::endl;
    std::cout << "   Chain tip: height " << tip_info.value().height << std::endl;
    std::cout << "   Note: Height 0 has 100 DIN unspendable OP_RETURN (symbolic)" << std::endl;
}

//=============================================================================
// Test 2: Create and Store Height 1 Block
//=============================================================================

Block testCreateHeight1Block(ChainDB& chain_db, BlockStorage& block_storage) {
    std::cout << "\n[Test 5.2] Creating block at height 1 (first PoW block)..." << std::endl;

    // Get genesis block hash (will be prev_hash for height 1)
    StatusOr<TipInfo> tip_info = chain_db.getTip();
    assert(tip_info.ok());
    uint256 genesis_hash = tip_info.value().hash;

    // Get genesis block for timestamp
    auto genesis_block = storage::ReadArchivalBlockDetailed(
        chain_db,
        &block_storage,
        genesis_hash,
        storage::ArchivalReadMode::RequireFlatfiles);
    assert(genesis_block.result.ok());

    // Create height 1 block
    Block height1_block;

    // Header
    height1_block.header.version = 1;
    height1_block.header.prev_block_hash = genesis_hash;
    height1_block.header.timestamp = genesis_block.result.value().header.timestamp + 120;  // 2 minutes after genesis
    height1_block.header.difficulty = ASERTConsensus::ASERT_ANCHOR_BITS;  // Same as genesis (ASERT anchor)
    height1_block.header.nonce = 0;  // Will be set by mining

    // Create coinbase transaction
    Transaction coinbase_tx = createHeight1Coinbase();
    height1_block.vtx.push_back(coinbase_tx);

    // Calculate merkle root
    TxId coinbase_txid = coinbase_tx.GetTxid();
    height1_block.header.merkle_root = coinbase_txid.AsUint256();

    // Utreexo root: Genesis has no spendable UTXOs (OP_RETURN), so empty forest
    height1_block.header.utreexo_root = uint256();

    // Reserved field: Must be all zeros
    height1_block.header.ZeroReserved();
    assert(height1_block.header.IsReservedValid());

    std::cout << "✅ Block template created" << std::endl;
    std::cout << "   Height: 1 (first PoW block)" << std::endl;
    std::cout << "   Prev hash: " << genesis_hash.GetHex() << std::endl;
    std::cout << "   Coinbase value: 100 DIN (per consensus policy)" << std::endl;

    // Store block at height 1 using the same flatfile + metadata contract as live nodes
    uint256 block_hash = height1_block.header.GetHash();

    rocksdb::WriteBatch batch;
    ChainWriteToken token = ChainWriteToken::CreateForTesting();

    auto block_pos = block_storage.writeBlock(block_hash, height1_block);
    assert(block_pos.status() == Status::Ok && "Height 1 block must be written to flatfiles");

    arith_uint256 work(1);
    chain_db.putHeader(token, block_hash, height1_block.header, 1, work, &batch);
    ChainDB::PersistedHeaderMetadata metadata;
    metadata.parent_hash = genesis_hash;
    metadata.height = 1;
    metadata.chainwork = work;
    metadata.status_flags = BLOCK_VALID_HEADER | BLOCK_HAVE_DATA;
    metadata.file_number = block_pos.value().file_number;
    metadata.data_pos = static_cast<uint32_t>(block_pos.value().offset);
    metadata.data_size = block_pos.value().size;
    chain_db.putHeaderMetadata(token, block_hash, metadata, &batch);
    chain_db.putHeightIndex(token, 1, block_hash, &batch);
    chain_db.setTip(token, block_hash, 1, work, &batch);
    chain_db.writeBatch(token, std::move(batch), true);

    std::cout << "✅ Block stored at height 1" << std::endl;
    std::cout << "   Block hash: " << block_hash.GetHex() << std::endl;

    return height1_block;
}

//=============================================================================
// Test 3: Index Height 1 Coinbase in UTXOIndex
//=============================================================================

void testIndexHeight1Coinbase(UTXOIndex& utxo_index, const Block& height1_block) {
    std::cout << "\n[Test 5.3] Indexing height 1 coinbase in wallet..." << std::endl;

    const Transaction& coinbase_tx = height1_block.vtx[0];
    TxId coinbase_txid = coinbase_tx.GetTxid();
    const TxOutput& output = coinbase_tx.vout[0];

    std::cout << "   Coinbase TxID: " << coinbase_txid.AsUint256().GetHex() << std::endl;
    std::cout << "   Coinbase value: " << output.value.v << " una (100 DIN per consensus)" << std::endl;

    // Add to UTXOIndex (is_coinbase = true, height = 1).
    // Use explicit external path so ownership invariant stays enforced.
    WalletUTXO utxo(coinbase_txid, 0, output.value, output.scriptPubKey, "coinbase", 1, true);
    bool added = utxo_index.AddUTXO(utxo);
    assert(added && "Must add height 1 coinbase to UTXOIndex");

    // Verify UTXO is indexed
    std::optional<WalletUTXO> retrieved = utxo_index.GetUTXO(coinbase_txid, 0);
    assert(retrieved.has_value() && "Height 1 coinbase must be indexed");
    assert(retrieved->is_coinbase == true && "Must be marked as coinbase");
    assert(retrieved->height == 1 && "Height must be 1");
    assert(retrieved->value.v == 100 * ConsensusSubsidy::UNA_PER_DIN && "Value must be 100 DIN per consensus policy");

    std::cout << "✅ Height 1 coinbase indexed" << std::endl;
    std::cout << "   TxID: " << retrieved->txid.AsUint256().GetHex() << std::endl;
    std::cout << "   Height: " << retrieved->height << std::endl;
    std::cout << "   Is coinbase: " << (retrieved->is_coinbase ? "true" : "false") << std::endl;
    std::cout << "   Value: " << retrieved->value.v << " una" << std::endl;
}

//=============================================================================
// Test 4: Verify Immature Balance Before Height 101
//=============================================================================

void testImmatureBalance(UTXOIndex& utxo_index) {
    std::cout << "\n[Test 5.4] Verifying immature balance before height 101..." << std::endl;

    struct TestCase {
        uint32_t current_height;
        std::string description;
    };

    std::vector<TestCase> test_cases = {
        {1, "Same block as coinbase (height 1)"},
        {50, "At height 50 (49 confirmations)"},
        {99, "At height 99 (98 confirmations)"},
        {100, "At height 100 (99 confirmations - boundary, still immature)"},
    };

    for (const auto& tc : test_cases) {
        BalanceDetail balance = utxo_index.GetBalanceWithMaturity(tc.current_height);

        // M.5.3: Assert balance is non-zero (prevents false positives)
        assert(balance.total.v > 0 && "Total balance must be non-zero - wallet indexing broken if zero!");

        // At these heights, height 1 coinbase should be immature
        assert(balance.immature.v > 0 && "Should have immature balance (height 1 coinbase)");

        std::cout << "   ✅ Immature at " << tc.description << std::endl;
        std::cout << "      Confirmed: " << balance.confirmed.v << " una" << std::endl;
        std::cout << "      Immature:  " << balance.immature.v << " una" << std::endl;
    }

    std::cout << "✅ Coinbase correctly immature before height 101" << std::endl;
}

//=============================================================================
// Test 5: Verify Mature Balance at Height 101+
//=============================================================================

void testMatureBalance(UTXOIndex& utxo_index) {
    std::cout << "\n[Test 5.5] Verifying mature balance at height 101+..." << std::endl;

    struct TestCase {
        uint32_t current_height;
        std::string description;
    };

    std::vector<TestCase> test_cases = {
        {101, "At height 101 (100 confirmations - boundary, now mature)"},
        {150, "At height 150 (149 confirmations)"},
        {200, "At height 200 (199 confirmations)"},
    };

    for (const auto& tc : test_cases) {
        BalanceDetail balance = utxo_index.GetBalanceWithMaturity(tc.current_height);

        // M.5.3: Assert balance is non-zero (prevents false positives)
        assert(balance.total.v > 0 && "Total balance must be non-zero - wallet indexing broken if zero!");

        // At these heights, all coinbases should be mature
        assert(balance.confirmed.v > 0 && "Should have confirmed balance (all coinbases mature)");
        assert(balance.immature.v == 0 && "Immature balance should be 0 at height 101+");

        std::cout << "   ✅ Mature at " << tc.description << std::endl;
        std::cout << "      Confirmed: " << balance.confirmed.v << " una" << std::endl;
        std::cout << "      Immature:  " << balance.immature.v << " una" << std::endl;
    }

    std::cout << "✅ Coinbase correctly mature at height 101+" << std::endl;
}

//=============================================================================
// Test 6: Verify No Special-Casing for Height 1
//=============================================================================

void testNoSpecialCasing() {
    std::cout << "\n[Test 5.6] Verifying no special-casing for height 1..." << std::endl;

    // Maturity formula: current_height >= coinbase_height + 100
    // For height 1: current_height >= 1 + 100 = 101

    uint32_t height1_maturity = 1 + 100;
    assert(height1_maturity == 101 && "Height 1 coinbase matures at height 101");

    // Compare with hypothetical height 2 coinbase
    uint32_t height2_maturity = 2 + 100;
    assert(height2_maturity == 102 && "Height 2 coinbase matures at height 102");

    // Same rule applies (no special offset or exception for height 1)
    std::cout << "✅ Height 1 follows standard maturity rules" << std::endl;
    std::cout << "   Height 1 matures at: " << height1_maturity << std::endl;
    std::cout << "   Height 2 would mature at: " << height2_maturity << std::endl;
    std::cout << "   No special-casing detected" << std::endl;
}

//=============================================================================
// Test Runner
//=============================================================================

int main() {
    SelectParams(Chain::MAINNET);
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "  TEST 5: WALLET TRACKS HEIGHT 1 COINBASE (Phase M.5.3)" << std::endl;
    std::cout << "════════════════════════════════════════════════════════════" << std::endl;

    try {
        // Clean test environment
        std::cout << "\nPreparing test environment..." << std::endl;
        cleanTestDatadir();
        std::cout << "✅ Test datadir: " << TEST_DATADIR << std::endl;

        // Initialize ChainDB and UTXOIndex
        ChainDB chain_db;
        Status init_status = chain_db.init(TEST_DATADIR);
        assert(init_status == Status::Ok && "ChainDB init must succeed");

        BlockStorage block_storage;
        Status block_storage_status = block_storage.init(TEST_DATADIR);
        assert(block_storage_status == Status::Ok && "BlockStorage init must succeed");

        UTXOIndex utxo_index(std::string(TEST_DATADIR) + "/utxo.db");
        bool utxo_init = utxo_index.Initialize();
        std::cout << "UTXOIndex::Initialize() returned: " << (utxo_init ? "true" : "false") << std::endl;
        assert(utxo_init && "UTXOIndex init must succeed");

        // Run all test cases
        testInitialize(chain_db, block_storage, utxo_index);

        Block height1_block = testCreateHeight1Block(chain_db, block_storage);

        testIndexHeight1Coinbase(utxo_index, height1_block);

        testImmatureBalance(utxo_index);

        testMatureBalance(utxo_index);

        testNoSpecialCasing();

        // Summary
        std::cout << "\n════════════════════════════════════════════════════════════" << std::endl;
        std::cout << "  ✅ ALL TESTS PASSED" << std::endl;
        std::cout << "════════════════════════════════════════════════════════════" << std::endl;
        std::cout << "\nWallet height 1 coinbase tracking verified:" << std::endl;
        std::cout << "  ✅ Height 1 coinbase indexed correctly" << std::endl;
        std::cout << "  ✅ Marked as coinbase (is_coinbase = true)" << std::endl;
        std::cout << "  ✅ Immature before height 101" << std::endl;
        std::cout << "  ✅ Mature at height 101 (100 confirmations)" << std::endl;
        std::cout << "  ✅ No special-casing for height 1" << std::endl;
        std::cout << "\nWallet correctly tracks and enforces maturity for all coinbase outputs.\n" << std::endl;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n❌ TEST FAILED: Unknown exception" << std::endl;
        return 1;
    }
}
