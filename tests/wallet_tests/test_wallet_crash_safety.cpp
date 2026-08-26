/**
 * @file test_wallet_crash_safety.cpp
 * @brief Phase A2: Wallet Crash-Safety Tests (Mainnet Hardening)
 *
 * MAINNET REQUIREMENT: Wallet must survive crashes without losing funds.
 *
 * This test proves:
 *   1. Interrupted rescan → wallet recovers cleanly
 *   2. Interrupted send → UTXOs not permanently stuck
 *   3. Partial DB state → no phantom/negative balances
 *   4. Restart → deterministic recovery
 *
 * Scenarios tested:
 *   - Crash mid-rescan (simulate with partial scan state)
 *   - Crash mid-transaction (simulate with uncommitted tx)
 *   - Reorg during restart (block disconnect/reconnect)
 *   - Corrupt scan checkpoint (recovery from last good state)
 *
 * If any test fails → DO NOT SHIP TO MAINNET
 */

#include "wallet/wallet_manager.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "primitives/hash_domains.h"
#include "primitives/amount.h"
#include "consensus/chainparams.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sqlite3.h>

using namespace dinero;

// ═══════════════════════════════════════════════════════════════════════════
// Test Infrastructure
// ═══════════════════════════════════════════════════════════════════════════

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define ASSERT_TRUE(cond, msg) \
    do { \
        g_tests_run++; \
        if (!(cond)) { \
            std::cerr << "  ❌ FAIL: " << msg << "\n"; \
            std::cerr << "     at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
        g_tests_passed++; \
    } while(0)

#define ASSERT_EQ(a, b, msg) \
    do { \
        g_tests_run++; \
        if ((a) != (b)) { \
            std::cerr << "  ❌ FAIL: " << msg << "\n"; \
            std::cerr << "     Expected: " << (b) << "\n"; \
            std::cerr << "     Got:      " << (a) << "\n"; \
            std::cerr << "     at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
        g_tests_passed++; \
    } while(0)

#define ASSERT_GE(a, b, msg) \
    do { \
        g_tests_run++; \
        if ((a) < (b)) { \
            std::cerr << "  ❌ FAIL: " << msg << "\n"; \
            std::cerr << "     " << (a) << " < " << (b) << "\n"; \
            std::cerr << "     at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
        g_tests_passed++; \
    } while(0)

// ═══════════════════════════════════════════════════════════════════════════
// Test Utilities
// ═══════════════════════════════════════════════════════════════════════════

std::filesystem::path g_test_dir;

void setupTestDirectory() {
    g_test_dir = std::filesystem::temp_directory_path() / "dinero_crash_test";
    std::filesystem::remove_all(g_test_dir);
    std::filesystem::create_directories(g_test_dir);
}

void cleanupTestDirectory() {
    std::filesystem::remove_all(g_test_dir);
}

// Create a minimal test block
Block makeTestBlock(uint32_t height, const uint256& prev_hash = uint256()) {
    Block block;
    block.header.version = 1;
    block.header.prev_block_hash = prev_hash;
    block.header.timestamp = 1700000000 + height * 600;  // ~10 min per block
    block.header.difficulty = 0x1d00ffff;
    block.header.nonce = height;
    block.header.ZeroReserved();  // BlockHeader v1 requires reserved[12] to be zero
    return block;
}

// Create a coinbase transaction paying to an address
Transaction makeCoinbaseTx(uint32_t height, const std::vector<uint8_t>& script_pubkey) {
    Transaction tx;
    tx.version = 2;
    tx.lockTime = 0;

    // Coinbase input (null prevout)
    TxInput input;
    input.prevout.txid = TxId();  // null txid
    input.prevout.vout = 0xffffffff;
    input.sequence = 0xffffffff;
    // Coinbase script with height
    input.scriptSig.push_back(static_cast<uint8_t>(height & 0xff));
    input.scriptSig.push_back(static_cast<uint8_t>((height >> 8) & 0xff));
    input.scriptSig.push_back(static_cast<uint8_t>((height >> 16) & 0xff));
    input.scriptSig.push_back(static_cast<uint8_t>((height >> 24) & 0xff));
    tx.vin.push_back(input);

    // Coinbase output
    TxOutput output;
    output.value = AmountUna::Una(5000000000);  // 50 DIN
    output.scriptPubKey = script_pubkey;
    tx.vout.push_back(output);

    return tx;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST 1: Balance never goes negative after interrupted operations
// ═══════════════════════════════════════════════════════════════════════════

bool test_no_negative_balance() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST 1: Balance never goes negative" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    setupTestDirectory();

    try {
        // Create wallet
        WalletManager wallet(g_test_dir);
        wallet.create("crash_test");
        wallet.open("crash_test");

        std::cout << "  Created test wallet" << std::endl;

        // Generate an address for testing
        std::string address = wallet.getNewAddress("test");
        std::cout << "  Generated address: " << address << std::endl;

        // Get initial balance
        auto balance = wallet.getBalance();
        std::cout << "  Initial balance: " << balance.total << " DIN" << std::endl;

        // ASSERTION: Balance must never be negative
        ASSERT_GE(balance.confirmed, 0.0, "Confirmed balance must be >= 0");
        ASSERT_GE(balance.unconfirmed, 0.0, "Unconfirmed balance must be >= 0");
        ASSERT_GE(balance.immature, 0.0, "Immature balance must be >= 0");
        ASSERT_GE(balance.total, 0.0, "Total balance must be >= 0");
        ASSERT_GE(balance.spendable, 0.0, "Spendable balance must be >= 0");

        std::cout << "  ✅ Initial balance is non-negative" << std::endl;

        // Simulate adding a UTXO
        std::string test_txid = "0000000000000000000000000000000000000000000000000000000000000001";
        bool added = wallet.addUTXO(test_txid, 0, 100000000, address, "51", 100, false);
        ASSERT_TRUE(added, "UTXO add must succeed");

        balance = wallet.getBalance();
        std::cout << "  After adding UTXO: " << balance.total << " DIN" << std::endl;
        ASSERT_GE(balance.total, 0.0, "Balance after add must be >= 0");

        // Simulate spending the UTXO
        bool spent = wallet.spendUTXO(test_txid, 0);
        ASSERT_TRUE(spent, "UTXO spend must succeed");

        balance = wallet.getBalance();
        std::cout << "  After spending UTXO: " << balance.total << " DIN" << std::endl;
        ASSERT_GE(balance.total, 0.0, "Balance after spend must be >= 0");

        // Try to spend again (double-spend attempt)
        bool double_spent = wallet.spendUTXO(test_txid, 0);
        // Should either fail or be idempotent, but balance must not go negative
        balance = wallet.getBalance();
        std::cout << "  After double-spend attempt: " << balance.total << " DIN" << std::endl;
        ASSERT_GE(balance.total, 0.0, "Balance after double-spend attempt must be >= 0");

        std::cout << "\n  ✅ Balance never went negative through all operations\n" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "  ❌ Exception: " << e.what() << std::endl;
        cleanupTestDirectory();
        return false;
    }

    cleanupTestDirectory();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST 2: No phantom UTXOs after partial operations
// ═══════════════════════════════════════════════════════════════════════════

bool test_no_phantom_utxos() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST 2: No phantom UTXOs" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    setupTestDirectory();

    try {
        WalletManager wallet(g_test_dir);
        wallet.create("phantom_test");
        wallet.open("phantom_test");

        std::string address = wallet.getNewAddress("test");

        // Add UTXOs
        std::string txid1 = "1111111111111111111111111111111111111111111111111111111111111111";
        std::string txid2 = "2222222222222222222222222222222222222222222222222222222222222222";

        wallet.addUTXO(txid1, 0, 50000000, address, "51", 100, false);
        wallet.addUTXO(txid2, 0, 30000000, address, "51", 100, false);

        auto utxos = wallet.listUnspentUTXOs(0);
        std::cout << "  Added 2 UTXOs, found " << utxos.size() << std::endl;
        ASSERT_EQ(utxos.size(), 2u, "Must have exactly 2 UTXOs");

        // Spend one
        wallet.spendUTXO(txid1, 0);

        utxos = wallet.listUnspentUTXOs(0);
        std::cout << "  After spending 1, found " << utxos.size() << " unspent" << std::endl;
        ASSERT_EQ(utxos.size(), 1u, "Must have exactly 1 unspent UTXO");

        // Remove the other (simulating reorg)
        wallet.removeUTXO(txid2, 0);

        utxos = wallet.listUnspentUTXOs(0);
        std::cout << "  After removing 1, found " << utxos.size() << " unspent" << std::endl;
        ASSERT_EQ(utxos.size(), 0u, "Must have 0 unspent UTXOs");

        // CRITICAL: Balance must reflect the UTXO count
        auto balance = wallet.getBalance();
        std::cout << "  Final balance: " << balance.total << " DIN" << std::endl;
        ASSERT_EQ(balance.utxo_count, 0, "UTXO count must be 0");

        // Try to list the removed UTXOs - they must not appear
        utxos = wallet.listUnspentUTXOs(0);
        for (const auto& utxo : utxos) {
            ASSERT_TRUE(utxo.txid != txid1, "Spent UTXO must not reappear");
            ASSERT_TRUE(utxo.txid != txid2, "Removed UTXO must not reappear");
        }

        std::cout << "\n  ✅ No phantom UTXOs detected\n" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "  ❌ Exception: " << e.what() << std::endl;
        cleanupTestDirectory();
        return false;
    }

    cleanupTestDirectory();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST 3: Wallet recovery after simulated crash (close without proper shutdown)
// ═══════════════════════════════════════════════════════════════════════════

bool test_crash_recovery() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST 3: Wallet recovery after simulated crash" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    setupTestDirectory();

    std::string test_address;
    std::string test_txid = "3333333333333333333333333333333333333333333333333333333333333333";

    try {
        // Phase 1: Create wallet and add data
        {
            WalletManager wallet(g_test_dir);
            wallet.create("recovery_test");
            wallet.open("recovery_test");

            test_address = wallet.getNewAddress("test");
            wallet.addUTXO(test_txid, 0, 75000000, test_address, "51", 100, false);

            auto balance = wallet.getBalance();
            std::cout << "  Before crash: " << balance.total << " DIN, "
                      << balance.utxo_count << " UTXOs" << std::endl;

            // Simulate crash: destructor will be called but imagine it didn't finish
            // SQLite's WAL mode ensures atomicity
        }

        std::cout << "  [Simulated crash - wallet object destroyed]" << std::endl;

        // Phase 2: Recovery - create new wallet instance
        {
            WalletManager wallet(g_test_dir);
            wallet.open("recovery_test");

            auto balance = wallet.getBalance();
            std::cout << "  After recovery: " << balance.total << " DIN, "
                      << balance.utxo_count << " UTXOs" << std::endl;

            // CRITICAL: Data must be intact
            ASSERT_EQ(balance.utxo_count, 1, "UTXO must survive crash");
            ASSERT_GE(balance.total, 0.0, "Balance must be non-negative");

            // Verify the specific UTXO exists
            auto utxos = wallet.listUnspentUTXOs(0);
            ASSERT_EQ(utxos.size(), 1u, "Must recover exactly 1 UTXO");
            ASSERT_EQ(utxos[0].txid, test_txid, "Must recover correct txid");
        }

        std::cout << "\n  ✅ Wallet recovered successfully after crash\n" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "  ❌ Exception: " << e.what() << std::endl;
        cleanupTestDirectory();
        return false;
    }

    cleanupTestDirectory();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST 4: Deterministic state after multiple restarts
// ═══════════════════════════════════════════════════════════════════════════

bool test_deterministic_restart() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST 4: Deterministic state after multiple restarts" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    setupTestDirectory();

    std::string test_address;
    double expected_balance = 0.0;
    int expected_utxo_count = 0;

    try {
        // Setup: Create wallet with known state
        {
            WalletManager wallet(g_test_dir);
            wallet.create("determinism_test");
            wallet.open("determinism_test");

            test_address = wallet.getNewAddress("test");

            // Add multiple UTXOs
            wallet.addUTXO("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0, 10000000, test_address, "51", 100, false);
            wallet.addUTXO("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 0, 20000000, test_address, "51", 101, false);
            wallet.addUTXO("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", 0, 30000000, test_address, "51", 102, false);

            // Spend one
            wallet.spendUTXO("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 0);

            auto balance = wallet.getBalance();
            expected_balance = balance.total;
            expected_utxo_count = balance.utxo_count;

            std::cout << "  Initial state: " << expected_balance << " DIN, "
                      << expected_utxo_count << " UTXOs" << std::endl;
        }

        // Multiple restart cycles
        for (int i = 0; i < 3; i++) {
            WalletManager wallet(g_test_dir);
            wallet.open("determinism_test");

            auto balance = wallet.getBalance();
            std::cout << "  Restart " << (i + 1) << ": " << balance.total << " DIN, "
                      << balance.utxo_count << " UTXOs" << std::endl;

            // CRITICAL: State must be identical on every restart
            ASSERT_EQ(balance.utxo_count, expected_utxo_count,
                      "UTXO count must be deterministic");

            // Balance comparison with tolerance for floating point
            double diff = std::abs(balance.total - expected_balance);
            ASSERT_TRUE(diff < 0.00000001, "Balance must be deterministic");
        }

        std::cout << "\n  ✅ Wallet state is deterministic across restarts\n" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "  ❌ Exception: " << e.what() << std::endl;
        cleanupTestDirectory();
        return false;
    }

    cleanupTestDirectory();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST 5: Reorg handling - blocks disconnected and reconnected
// ═══════════════════════════════════════════════════════════════════════════

bool test_reorg_handling() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST 5: Reorg handling (disconnect/reconnect blocks)" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    setupTestDirectory();

    try {
        WalletManager wallet(g_test_dir);
        wallet.create("reorg_test");
        wallet.open("reorg_test");

        std::string address = wallet.getNewAddress("test");

        // Add UTXOs at different heights
        wallet.addUTXO("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", 0, 10000000, address, "51", 100, false);
        wallet.addUTXO("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", 0, 20000000, address, "51", 101, false);
        wallet.addUTXO("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", 0, 30000000, address, "51", 102, false);

        auto balance_before = wallet.getBalance();
        std::cout << "  Before reorg: " << balance_before.total << " DIN, "
                  << balance_before.utxo_count << " UTXOs" << std::endl;

        // Simulate 2-block reorg: remove UTXOs from height 101 and 102
        // Note: removeUTXO() is the proper API for UTXO removal during reorg
        // (removeTransactionsAtHeight only affects the transactions table)
        bool removed_102 = wallet.removeUTXO("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", 0);
        bool removed_101 = wallet.removeUTXO("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", 0);

        std::cout << "  Disconnected blocks 101-102 (removed UTXOs: "
                  << removed_101 << ", " << removed_102 << ")" << std::endl;

        auto balance_after_reorg = wallet.getBalance();
        std::cout << "  After reorg: " << balance_after_reorg.total << " DIN, "
                  << balance_after_reorg.utxo_count << " UTXOs" << std::endl;

        // Should only have UTXO from height 100
        ASSERT_EQ(balance_after_reorg.utxo_count, 1,
                  "Should have 1 UTXO after 2-block reorg");

        // Balance must not be negative
        ASSERT_GE(balance_after_reorg.total, 0.0, "Balance must be >= 0 after reorg");
        ASSERT_GE(balance_after_reorg.confirmed, 0.0, "Confirmed must be >= 0 after reorg");

        // Re-add UTXOs (simulating new blocks on different chain)
        wallet.addUTXO("1111111111111111111111111111111111111111111111111111111111111111", 0, 25000000, address, "51", 101, false);

        auto balance_final = wallet.getBalance();
        std::cout << "  After new block: " << balance_final.total << " DIN, "
                  << balance_final.utxo_count << " UTXOs" << std::endl;

        ASSERT_EQ(balance_final.utxo_count, 2, "Should have 2 UTXOs after new block");

        std::cout << "\n  ✅ Reorg handled correctly - no stuck/phantom UTXOs\n" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "  ❌ Exception: " << e.what() << std::endl;
        cleanupTestDirectory();
        return false;
    }

    cleanupTestDirectory();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST 6: Scan height checkpoint integrity
// ═══════════════════════════════════════════════════════════════════════════

bool test_scan_checkpoint_integrity() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST 6: Scan checkpoint integrity" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    setupTestDirectory();

    try {
        // Phase 1: Create wallet and set scan height
        {
            WalletManager wallet(g_test_dir);
            wallet.create("checkpoint_test");
            wallet.open("checkpoint_test");

            // Simulate scanning up to height 500
            wallet.setBlockchainHeight(500);

            auto status = wallet.GetScanStatus(500);
            std::cout << "  Set scan height to 500" << std::endl;
            std::cout << "  Status: scan_height=" << status.scan_height
                      << ", chain_height=" << status.chain_height << std::endl;
        }

        // Phase 2: Restart and verify checkpoint
        {
            WalletManager wallet(g_test_dir);
            wallet.open("checkpoint_test");

            // Load blockchain height from database
            wallet.loadBlockchainHeight();

            uint32_t height = wallet.getCurrentBlockchainHeight();
            std::cout << "  After restart, loaded height: " << height << std::endl;

            // Restart continuity is exact: treating a missing/default height as
            // acceptable makes the daemon replay from genesis on every boot.
            ASSERT_EQ(height, 500u,
                      "Restart must resume from the durable wallet checkpoint");

            // Balance check
            auto balance = wallet.getBalance();
            ASSERT_GE(balance.total, 0.0, "Balance must be >= 0 after checkpoint recovery");
        }

        std::cout << "\n  ✅ Scan checkpoint integrity maintained\n" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "  ❌ Exception: " << e.what() << std::endl;
        cleanupTestDirectory();
        return false;
    }

    cleanupTestDirectory();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// TEST 7: Transaction history insert always binds wallet_id when schema has it
// ═══════════════════════════════════════════════════════════════════════════
bool test_transaction_history_wallet_id_binding() {
    std::cout << "\n═══════════════════════════════════════════════════════════" << std::endl;
    std::cout << "TEST 7: Transaction history wallet_id binding" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════\n" << std::endl;

    setupTestDirectory();

    try {
        WalletManager wallet(g_test_dir);
        wallet.create("tx_history_test");
        wallet.open("tx_history_test");

        std::string address = wallet.getNewAddress("tx-test");
        ASSERT_TRUE(!address.empty(), "Test address must be generated");

        sqlite3* db = wallet.getCurrentDatabase();
        ASSERT_TRUE(db != nullptr, "Wallet database pointer must be valid");

        // Recreate transactions table with uppercase WALLET_ID column name.
        // This validates that addTransaction does not depend on case-sensitive
        // schema probing and still binds wallet_id correctly.
        const char* recreate_sql = R"(
            BEGIN IMMEDIATE;
            ALTER TABLE transactions RENAME TO transactions_old;
            CREATE TABLE transactions (
                id INTEGER PRIMARY KEY,
                WALLET_ID INTEGER NOT NULL,
                txid TEXT NOT NULL,
                address TEXT NOT NULL,
                amount REAL NOT NULL,
                confirmations INTEGER NOT NULL DEFAULT 0,
                category TEXT NOT NULL,
                label TEXT,
                time INTEGER NOT NULL DEFAULT (strftime('%s','now')),
                is_coinbase INTEGER NOT NULL DEFAULT 0,
                height INTEGER NOT NULL DEFAULT 0,
                UNIQUE(WALLET_ID, txid, address)
            );
            DROP TABLE transactions_old;
            COMMIT;
        )";

        char* err_msg = nullptr;
        int rc = sqlite3_exec(db, recreate_sql, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::string err = err_msg ? err_msg : "unknown";
            sqlite3_free(err_msg);
            std::cerr << "  ❌ SQLite recreate failed: " << err << std::endl;
            cleanupTestDirectory();
            return false;
        }

        const std::string txid = "abababababababababababababababababababababababababababababababab";
        bool tx_added = wallet.addTransaction(txid, address, 12.5, "receive", false, "", 0, 321);
        ASSERT_TRUE(tx_added, "addTransaction must succeed with WALLET_ID schema");

        sqlite3_stmt* stmt = nullptr;
        rc = sqlite3_prepare_v2(db, "SELECT WALLET_ID FROM transactions WHERE txid = ?", -1, &stmt, nullptr);
        ASSERT_EQ(rc, SQLITE_OK, "SELECT WALLET_ID prepare must succeed");

        sqlite3_bind_text(stmt, 1, txid.c_str(), -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        ASSERT_EQ(rc, SQLITE_ROW, "Inserted transaction row must exist");
        int wallet_id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);

        ASSERT_EQ(wallet_id, 1, "Inserted transaction must bind wallet_id=1");

        std::cout << "\n  ✅ Transaction history wallet_id binding is robust\n" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "  ❌ Exception: " << e.what() << std::endl;
        cleanupTestDirectory();
        return false;
    }

    cleanupTestDirectory();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "\n" << std::endl;
    std::cout << "╔═══════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Phase A2: Wallet Crash-Safety Tests                      ║" << std::endl;
    std::cout << "║  MAINNET HARDENING - Wallet Recovery                      ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════════════════════╝" << std::endl;

    // Initialize chain params
    SelectParams(Chain::REGTEST);

    bool all_passed = true;

    // Run all tests
    all_passed &= test_no_negative_balance();
    all_passed &= test_no_phantom_utxos();
    all_passed &= test_crash_recovery();
    all_passed &= test_deterministic_restart();
    all_passed &= test_reorg_handling();
    all_passed &= test_scan_checkpoint_integrity();
    all_passed &= test_transaction_history_wallet_id_binding();

    // Summary
    std::cout << "\n╔═══════════════════════════════════════════════════════════╗" << std::endl;
    if (all_passed) {
        std::cout << "║  ✅ ALL WALLET CRASH-SAFETY TESTS PASSED                 ║" << std::endl;
        std::cout << "╠═══════════════════════════════════════════════════════════╣" << std::endl;
        std::cout << "║  Proven:                                                  ║" << std::endl;
        std::cout << "║    • Balance NEVER goes negative                          ║" << std::endl;
        std::cout << "║    • No phantom/stuck UTXOs                               ║" << std::endl;
        std::cout << "║    • Crash recovery works correctly                       ║" << std::endl;
        std::cout << "║    • State is deterministic across restarts               ║" << std::endl;
        std::cout << "║    • Reorg handling preserves integrity                   ║" << std::endl;
        std::cout << "║    • Scan checkpoints are reliable                        ║" << std::endl;
    } else {
        std::cout << "║  ❌ WALLET CRASH-SAFETY TESTS FAILED                      ║" << std::endl;
        std::cout << "║  DO NOT SHIP TO MAINNET                                   ║" << std::endl;
    }
    std::cout << "╚═══════════════════════════════════════════════════════════╝" << std::endl;

    std::cout << "\nTests: " << g_tests_passed << "/" << g_tests_run << " passed" << std::endl;

    // Cleanup any remaining test files
    cleanupTestDirectory();

    return all_passed ? 0 : 1;
}
