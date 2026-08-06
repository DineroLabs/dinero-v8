/**
 * @file test_wallet_rescan_utxoset.cpp
 * @brief Regression test for WalletManager::rescanUtxoSet (AssumeUTXO/snapshot
 *        bootstrap visibility).
 *
 * BUG: A wallet that fast-syncs from a UTXO snapshot showed ZERO for coins
 * received before the snapshot height. The block-replay rescan can only match
 * outputs from block transaction bodies, which are absent for pre-snapshot
 * heights, so those coins never landed in the wallet's local `utxos` table.
 *
 * FIX: rescanUtxoSet() scans the loaded chainstate UTXO set directly, matching
 * each coin's scriptPubKey against watch_scripts and recording owned coins.
 *
 * This test seeds a watch_script, feeds a UTXO-set producer containing one
 * matching coin and one foreign coin, and asserts the matching coin lands in
 * the wallet (balance + utxo count), is idempotent on re-run, and that the
 * scan-height watermark is advanced to the snapshot height.
 *
 * Uses exit-nonzero checks (NOT bare assert(), which is a no-op under NDEBUG).
 */

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>
#include <cstdint>
#include <unistd.h>
#include <sqlite3.h>

#include "wallet/wallet_manager.h"
#include "consensus/chainparams.h"

using dinero::WalletManager;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) {
        std::cout << "  ✓ " << what << std::endl;
    } else {
        std::cerr << "  ✗ FAIL: " << what << std::endl;
        ++g_failures;
    }
}

// Wallets created by older releases carry the original composite uniqueness
// constraint. Fresh wallets use UNIQUE(txid, vout). The snapshot upsert must
// work with both layouts because an upgraded mobile wallet keeps its database.
static void rewriteUtxosAsLegacyComposite(const std::filesystem::path& db_path) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
        const std::string error = db ? sqlite3_errmsg(db) : "open failed";
        if (db) sqlite3_close(db);
        throw std::runtime_error("legacy-schema setup failed: " + error);
    }

    const char* sql = R"SQL(
        PRAGMA foreign_keys=OFF;
        BEGIN IMMEDIATE;
        CREATE TABLE IF NOT EXISTS wallets (
            id INTEGER PRIMARY KEY,
            name TEXT NOT NULL UNIQUE
        );
        INSERT OR IGNORE INTO wallets(id, name) VALUES(1, 'snapshot_wallet');
        ALTER TABLE utxos ADD COLUMN snapshot_anchored INTEGER NOT NULL DEFAULT 0;
        ALTER TABLE utxos RENAME TO utxos_fresh_layout;
        CREATE TABLE utxos (
            id INTEGER PRIMARY KEY,
            wallet_id INTEGER NOT NULL REFERENCES wallets(id) ON DELETE CASCADE,
            txid TEXT NOT NULL,
            vout INTEGER NOT NULL,
            address TEXT NOT NULL,
            amount INTEGER NOT NULL,
            script_pubkey TEXT NOT NULL,
            height INTEGER NOT NULL,
            is_coinbase INTEGER NOT NULL DEFAULT 0,
            is_mature INTEGER NOT NULL DEFAULT 0,
            is_spent INTEGER NOT NULL DEFAULT 0,
            created_at INTEGER NOT NULL DEFAULT (strftime('%s','now')),
            spent_txid TEXT,
            spent_height INTEGER,
            confirmations INTEGER DEFAULT 0,
            snapshot_anchored INTEGER NOT NULL DEFAULT 0,
            UNIQUE(wallet_id, txid, vout)
        );
        INSERT INTO utxos
            (id, wallet_id, txid, vout, address, amount, script_pubkey, height,
             is_coinbase, is_mature, is_spent, created_at, spent_txid,
             spent_height, confirmations, snapshot_anchored)
        SELECT id, wallet_id, txid, vout, address, amount, script_pubkey, height,
               is_coinbase, is_mature, is_spent, created_at, spent_txid,
               spent_height, confirmations, snapshot_anchored
        FROM utxos_fresh_layout;
        DROP TABLE utxos_fresh_layout;
        CREATE INDEX idx_utxos_wallet_id ON utxos(wallet_id);
        CREATE INDEX idx_utxos_spent ON utxos(is_spent);
        CREATE INDEX idx_utxos_mature ON utxos(is_mature);
        CREATE INDEX idx_utxos_coinbase ON utxos(is_coinbase);
        COMMIT;
        PRAGMA foreign_keys=ON;
    )SQL";

    char* error_message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error_message);
    const std::string error = error_message ? error_message : "unknown SQLite error";
    sqlite3_free(error_message);
    sqlite3_close(db);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("legacy-schema setup failed: " + error);
    }
}

// Build a P2TR scriptPubKey: OP_1 (0x51) OP_PUSHBYTES_32 (0x20) <32-byte program>
static std::vector<uint8_t> p2tr(uint8_t fill) {
    std::vector<uint8_t> spk;
    spk.push_back(0x51);
    spk.push_back(0x20);
    for (int i = 0; i < 32; i++) spk.push_back(static_cast<uint8_t>(fill + i));
    return spk;
}

int main() {
    std::cout << "=== WalletManager::rescanUtxoSet regression test ===" << std::endl;

    dinero::SelectParams(dinero::Chain::MAINNET);

    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() /
        ("rescan_utxoset_test_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    const uint32_t kSnapshotHeight = 52287;
    const uint32_t kCoinHeight = 52000;
    const uint64_t kAmount = 5000000000ULL;  // 50 DIN in una

    // The wallet-owned script and a foreign script that must be ignored.
    std::vector<uint8_t> owned_spk = p2tr(0x10);
    std::vector<uint8_t> foreign_spk = p2tr(0x90);

    try {
        WalletManager wallet(dir);
        wallet.create("snapshot_wallet");
        wallet.open("snapshot_wallet");

        // Register only the owned script for watching.
        wallet.addWatchScript(owned_spk, "m/86'/0'/0'/0/0", false);

        // Reproduce the schema on the affected upgraded iOS wallet. The old
        // ON CONFLICT(txid, vout) clause did not match this three-column UNIQUE
        // constraint, so sqlite3_prepare_v2() failed for every owned coin while
        // the recovery scan misleadingly reported "recorded 0".
        rewriteUtxosAsLegacyComposite(
            dir / "wallets" / "wallet_snapshot_wallet.db");

        // Baseline: nothing recorded yet.
        check(wallet.getBalance().utxo_count == 0, "wallet starts empty");

        // Producer yields one owned coin and one foreign coin.
        auto producer = [&](const std::function<void(const WalletManager::UtxoSetEntry&)>& sink) {
            WalletManager::UtxoSetEntry owned;
            owned.txid_hex = "1111111111111111111111111111111111111111111111111111111111111111";
            owned.vout = 0;
            owned.amount_una = kAmount;
            owned.script_pubkey = owned_spk;
            owned.height = kCoinHeight;
            owned.is_coinbase = false;
            sink(owned);

            WalletManager::UtxoSetEntry foreign;
            foreign.txid_hex = "2222222222222222222222222222222222222222222222222222222222222222";
            foreign.vout = 0;
            foreign.amount_una = kAmount;
            foreign.script_pubkey = foreign_spk;  // not in watch_scripts
            foreign.height = kCoinHeight;
            foreign.is_coinbase = false;
            sink(foreign);
        };

        int recorded = wallet.rescanUtxoSet(producer, kSnapshotHeight);
        check(recorded == 1, "exactly one owned coin recorded (foreign coin ignored)");

        auto bal = wallet.getBalance();
        check(bal.utxo_count == 1, "wallet utxo_count == 1 after snapshot rescan");
        check(bal.total > 0.0, "wallet balance is non-zero after snapshot rescan");
        check(wallet.getCurrentBlockchainHeight() == kSnapshotHeight,
              "scan-height watermark advanced to snapshot height");

        // Idempotency: re-running must never create a duplicate row or
        // double-count the balance. Under PR #338 the sink upserts
        // (ON CONFLICT DO UPDATE SET is_spent=0, snapshot_anchored=1,
        // refresh authoritative fields) instead of the old INSERT OR IGNORE, so a
        // second pass UPDATES the SAME row in place — sqlite3_changes() reports 1
        // changed row, which is intentional (it un-spends/refreshes a coin that
        // may have been mis-flagged is_spent=1 by a prior block-replay rescan).
        // The invariant that actually matters is no duplication: the coin is the
        // SAME row, so utxo_count and the total balance are unchanged.
        const double bal_total_before = bal.total;
        int recorded2 = wallet.rescanUtxoSet(producer, kSnapshotHeight);
        check(recorded2 == 1, "re-run refreshes the existing row in place (upsert, not a new coin)");
        auto bal2 = wallet.getBalance();
        check(bal2.utxo_count == 1, "utxo_count still 1 after re-run (no duplicate row)");
        check(bal2.total == bal_total_before, "balance unchanged after re-run (no double-count)");

    } catch (const std::exception& e) {
        std::cerr << "  ✗ FAIL: exception: " << e.what() << std::endl;
        ++g_failures;
    }

    fs::remove_all(dir);

    // ────────────────────────────────────────────────────────────────────────
    // ORDERING scenario: snapshot loaded FIRST while the wallet has no matching
    // script (mirrors a node fast-syncing wallet-absent), THEN the wallet
    // registers its watch_script. Only the SECOND trigger (rescan after the
    // script exists) makes the pre-snapshot coin visible — the node is
    // headers-only below the base, so block-replay can never recover it.
    // This is what WalletService runs when a wallet becomes active while an
    // AssumeUTXO snapshot is already loaded.
    // ────────────────────────────────────────────────────────────────────────
    std::cout << "\n--- ordering: snapshot loaded before wallet registers script ---" << std::endl;
    fs::path dir2 = fs::temp_directory_path() /
        ("rescan_utxoset_order_" + std::to_string(::getpid()));
    fs::remove_all(dir2);
    fs::create_directories(dir2);

    try {
        WalletManager wallet(dir2);
        wallet.create("late_wallet");
        wallet.open("late_wallet");

        auto producer = [&](const std::function<void(const WalletManager::UtxoSetEntry&)>& sink) {
            WalletManager::UtxoSetEntry owned;
            owned.txid_hex = "3333333333333333333333333333333333333333333333333333333333333333";
            owned.vout = 0;
            owned.amount_una = kAmount;
            owned.script_pubkey = owned_spk;
            owned.height = kCoinHeight;
            owned.is_coinbase = false;
            sink(owned);
        };

        // FIRST snapshot-load while the wallet has no matching watch_script:
        // exactly what the LoadSnapshot hook does when the wallet isn't watching
        // this script yet. Nothing should be recorded.
        int early = wallet.rescanUtxoSet(producer, kSnapshotHeight);
        check(early == 0, "snapshot rescan before watch_script records nothing");
        check(wallet.getBalance().utxo_count == 0, "coin invisible until script registered");

        // Wallet now registers its script (open/import/restore completes).
        wallet.addWatchScript(owned_spk, "m/86'/0'/0'/0/0", false);

        // SECOND trigger (the new WalletService path): rescan the already-loaded
        // snapshot UTXO set now that the wallet is watching the script.
        int late = wallet.rescanUtxoSet(producer, kSnapshotHeight);
        check(late == 1, "second trigger after script registration records the coin");
        check(wallet.getBalance().utxo_count == 1, "coin now visible to late-opened wallet");
        check(wallet.getBalance().total > 0.0, "balance non-zero for late-opened wallet");

    } catch (const std::exception& e) {
        std::cerr << "  ✗ FAIL: exception: " << e.what() << std::endl;
        ++g_failures;
    }

    fs::remove_all(dir2);

    // iOS moves an app's data into a new UUID-named container during some
    // installs. The registry stores an absolute wallet path. Critically, the
    // old container can still exist, so a fallback that repairs only when the
    // old path is missing opens the wrong database and presents an empty wallet.
    // Keep both copies alive and give them different sentinels: open() must
    // choose the copy under the WalletManager's CURRENT data directory.
    std::cout << "\n--- relocation: stale registry path still exists ---" << std::endl;
    fs::path old_dir = fs::temp_directory_path() /
        ("wallet_old_container_" + std::to_string(::getpid()));
    fs::path new_dir = fs::temp_directory_path() /
        ("wallet_new_container_" + std::to_string(::getpid()));
    fs::remove_all(old_dir);
    fs::remove_all(new_dir);
    fs::create_directories(old_dir);

    try {
        {
            WalletManager old_wallet(old_dir);
            old_wallet.create("default");
            old_wallet.open("default");
            old_wallet.setSetting("container_probe", "old");
        }

        fs::copy(old_dir, new_dir, fs::copy_options::recursive);
        const fs::path new_wallet_db = new_dir / "wallets" / "wallet_default.db";
        sqlite3* db = nullptr;
        check(sqlite3_open(new_wallet_db.string().c_str(), &db) == SQLITE_OK,
              "opened relocated wallet copy for test setup");
        if (db) {
            const char* sql =
                "INSERT INTO settings(key,value) VALUES('container_probe','new') "
                "ON CONFLICT(key) DO UPDATE SET value='new'";
            check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK,
                  "marked relocated wallet copy distinctly");
            sqlite3_close(db);
        }

        check(fs::exists(old_dir / "wallets" / "wallet_default.db"),
              "stale wallet path intentionally still exists");
        WalletManager relocated(new_dir);
        relocated.open("default");
        check(relocated.getSetting("container_probe") == "new",
              "open prefers current container instead of readable stale path");

        sqlite3* registry = nullptr;
        check(sqlite3_open((new_dir / "wallet_registry.db").string().c_str(), &registry) == SQLITE_OK,
              "opened relocated registry for repair verification");
        if (registry) {
            sqlite3_stmt* stmt = nullptr;
            std::string stored_path;
            if (sqlite3_prepare_v2(registry,
                    "SELECT path FROM wallets WHERE name='default'", -1, &stmt, nullptr) == SQLITE_OK &&
                sqlite3_step(stmt) == SQLITE_ROW) {
                const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (value) stored_path = value;
            }
            sqlite3_finalize(stmt);
            sqlite3_close(registry);
            check(fs::path(stored_path).lexically_normal() == new_wallet_db.lexically_normal(),
                  "registry repaired to current container path");
        }
    } catch (const std::exception& e) {
        std::cerr << "  ✗ FAIL: relocation exception: " << e.what() << std::endl;
        ++g_failures;
    }

    fs::remove_all(old_dir);
    fs::remove_all(new_dir);

    if (g_failures == 0) {
        std::cout << "\n✓ All rescanUtxoSet checks passed." << std::endl;
        return 0;
    }
    std::cerr << "\n✗ " << g_failures << " check(s) failed." << std::endl;
    return 1;
}
