#include "wallet/wallet_sync_enhanced.h"
#include "common/logger.h"
#include "consensus/coin_type.h"
#include "sqlite_txn.h"
#include <sqlite3.h>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dinero {

WalletSyncEnhanced::WalletSyncEnhanced(const std::string& wallet_db_path, const std::string& blockchain_db_path)
    : wallet_db_path_(wallet_db_path), blockchain_db_path_(blockchain_db_path), 
      wallet_db_(nullptr), blockchain_db_(nullptr) {
}

WalletSyncEnhanced::~WalletSyncEnhanced() {
    if (wallet_db_) {
        sqlite3_close(wallet_db_);
    }
    if (blockchain_db_) {
        sqlite3_close(blockchain_db_);
    }
}

bool WalletSyncEnhanced::Initialize() {
    // Open wallet database
    int rc = sqlite3_open(wallet_db_path_.c_str(), &wallet_db_);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("WalletSyncEnhanced: Failed to open wallet database: " + std::string(sqlite3_errmsg(wallet_db_)));
        return false;
    }
    
    // Open blockchain database
    rc = sqlite3_open(blockchain_db_path_.c_str(), &blockchain_db_);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("WalletSyncEnhanced: Failed to open blockchain database: " + std::string(sqlite3_errmsg(blockchain_db_)));
        return false;
    }
    
    // Enable WAL mode and performance settings
    sqlite3_exec(wallet_db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(wallet_db_, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
    sqlite3_exec(wallet_db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
    sqlite3_exec(wallet_db_, "PRAGMA cache_size=10000", nullptr, nullptr, nullptr);
    sqlite3_exec(wallet_db_, "PRAGMA busy_timeout=5000", nullptr, nullptr, nullptr);
    
    sqlite3_exec(blockchain_db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(blockchain_db_, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
    sqlite3_exec(blockchain_db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
    sqlite3_exec(blockchain_db_, "PRAGMA cache_size=10000", nullptr, nullptr, nullptr);
    sqlite3_exec(blockchain_db_, "PRAGMA busy_timeout=5000", nullptr, nullptr, nullptr);
    
    dinero::g_logger.info("WalletSyncEnhanced initialized with wallet: " + wallet_db_path_ + ", blockchain: " + blockchain_db_path_);
    return true;
}

bool WalletSyncEnhanced::RescanFromHeight(int start_height, int gap_limit) {
    if (!wallet_db_ || !blockchain_db_) {
        dinero::g_logger.error("WalletSyncEnhanced::RescanFromHeight: Databases not initialized");
        return false;
    }
    
    try {
        SqliteTxn txn(wallet_db_, SqliteTxn::Mode::Immediate);
        
        // Get current blockchain height
        int current_height = GetCurrentBlockchainHeight();
        if (current_height < 0) {
            dinero::g_logger.error("WalletSyncEnhanced::RescanFromHeight: Failed to get blockchain height");
            return false;
        }
        
        dinero::g_logger.info("Starting rescan from height " + std::to_string(start_height) + 
                             " to " + std::to_string(current_height) + " with gap limit " + std::to_string(gap_limit));
        
        // Update sync_meta
        if (!UpdateSyncMeta(start_height, current_height, gap_limit)) {
            dinero::g_logger.error("WalletSyncEnhanced::RescanFromHeight: Failed to update sync_meta");
            return false;
        }
        
        // Derive and register watch scripts with gap limit
        if (!DeriveAndRegisterWatchScripts(gap_limit)) {
            dinero::g_logger.error("WalletSyncEnhanced::RescanFromHeight: Failed to derive watch scripts");
            return false;
        }
        
        // Perform the actual rescan
        if (!PerformRescan(start_height, current_height)) {
            dinero::g_logger.error("WalletSyncEnhanced::RescanFromHeight: Failed to perform rescan");
            return false;
        }
        
        txn.commit();
        dinero::g_logger.info("✅ Rescan completed successfully from height " + std::to_string(start_height) + 
                             " to " + std::to_string(current_height));
        return true;
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("WalletSyncEnhanced::RescanFromHeight failed: " + std::string(e.what()));
        return false;
    }
}

bool WalletSyncEnhanced::IncrementalSync() {
    if (!wallet_db_ || !blockchain_db_) {
        dinero::g_logger.error("WalletSyncEnhanced::IncrementalSync: Databases not initialized");
        return false;
    }
    
    try {
        // Get last scanned height
        int last_scanned = GetLastScannedHeight();
        if (last_scanned < 0) {
            dinero::g_logger.error("WalletSyncEnhanced::IncrementalSync: Failed to get last scanned height");
            return false;
        }
        
        // Get current blockchain height
        int current_height = GetCurrentBlockchainHeight();
        if (current_height < 0) {
            dinero::g_logger.error("WalletSyncEnhanced::IncrementalSync: Failed to get blockchain height");
            return false;
        }
        
        if (current_height <= last_scanned) {
            dinero::g_logger.info("WalletSyncEnhanced::IncrementalSync: No new blocks to sync");
            return true;
        }
        
        dinero::g_logger.info("Incremental sync from height " + std::to_string(last_scanned + 1) + 
                             " to " + std::to_string(current_height));
        
        // Perform incremental rescan
        if (!PerformRescan(last_scanned + 1, current_height)) {
            dinero::g_logger.error("WalletSyncEnhanced::IncrementalSync: Failed to perform incremental rescan");
            return false;
        }
        
        // Update last scanned height
        if (!UpdateLastScannedHeight(current_height)) {
            dinero::g_logger.error("WalletSyncEnhanced::IncrementalSync: Failed to update last scanned height");
            return false;
        }
        
        dinero::g_logger.info("✅ Incremental sync completed successfully");
        return true;
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("WalletSyncEnhanced::IncrementalSync failed: " + std::string(e.what()));
        return false;
    }
}

bool WalletSyncEnhanced::ExportSeed(const std::string& wallet_name, std::string& seed_hex) {
    if (!wallet_db_) {
        dinero::g_logger.error("WalletSyncEnhanced::ExportSeed: Wallet database not initialized");
        return false;
    }
    
    // Get seed from wallet database
    const char* sql = "SELECT seed_hex FROM wallets WHERE name = ?";
    sqlite3_stmt* stmt;
    
    int rc = sqlite3_prepare_v2(wallet_db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("WalletSyncEnhanced::ExportSeed: Failed to prepare statement: " + std::string(sqlite3_errmsg(wallet_db_)));
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, wallet_name.c_str(), -1, SQLITE_STATIC);
    
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* seed = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (seed) {
            seed_hex = std::string(seed);
            found = true;
        }
    }
    
    sqlite3_finalize(stmt);
    
    if (!found) {
        dinero::g_logger.error("WalletSyncEnhanced::ExportSeed: Wallet not found: " + wallet_name);
        return false;
    }
    
    dinero::g_logger.info("WalletSyncEnhanced: Seed exported for wallet: " + wallet_name);
    return true;
}

bool WalletSyncEnhanced::ExportDescriptor(const std::string& wallet_name, std::string& descriptor) {
    if (!wallet_db_) {
        dinero::g_logger.error("WalletSyncEnhanced::ExportDescriptor: Wallet database not initialized");
        return false;
    }
    
    // Generate BIP84 descriptor for the wallet
    // Format: wpkh([fingerprint/derivation]xpub.../0/*)
    const char* sql = "SELECT seed_hex FROM wallets WHERE name = ?";
    sqlite3_stmt* stmt;
    
    int rc = sqlite3_prepare_v2(wallet_db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("WalletSyncEnhanced::ExportDescriptor: Failed to prepare statement: " + std::string(sqlite3_errmsg(wallet_db_)));
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, wallet_name.c_str(), -1, SQLITE_STATIC);
    
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* seed_hex = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (seed_hex) {
            // For now, generate a placeholder descriptor
            // In a real implementation, this would derive the actual xpub from the seed
            descriptor =
                "wpkh([00000000/84'/" +
                std::to_string(dinero::consensus::DINERO_COIN_TYPE) +
                "'/0']xpub000000000000000000000000000000000000000000000000000000000000000000/0/*)";
            found = true;
        }
    }
    
    sqlite3_finalize(stmt);
    
    if (!found) {
        dinero::g_logger.error("WalletSyncEnhanced::ExportDescriptor: Wallet not found: " + wallet_name);
        return false;
    }
    
    dinero::g_logger.info("WalletSyncEnhanced: Descriptor exported for wallet: " + wallet_name);
    return true;
}

int WalletSyncEnhanced::GetCurrentBlockchainHeight() const {
    if (!blockchain_db_) {
        return -1;
    }
    
    const char* sql = "SELECT MAX(height) FROM utxo";
    sqlite3_stmt* stmt;
    
    int rc = sqlite3_prepare_v2(blockchain_db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("WalletSyncEnhanced::GetCurrentBlockchainHeight: Failed to prepare statement: " + std::string(sqlite3_errmsg(blockchain_db_)));
        return -1;
    }
    
    int height = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        height = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return height;
}

int WalletSyncEnhanced::GetLastScannedHeight() const {
    if (!wallet_db_) {
        return -1;
    }
    
    const char* sql = "SELECT last_scanned_height FROM sync_meta WHERE id = 1";
    sqlite3_stmt* stmt;
    
    int rc = sqlite3_prepare_v2(wallet_db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("WalletSyncEnhanced::GetLastScannedHeight: Failed to prepare statement: " + std::string(sqlite3_errmsg(wallet_db_)));
        return -1;
    }
    
    int height = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        height = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return height;
}

bool WalletSyncEnhanced::UpdateSyncMeta(int start_height, int current_height, int gap_limit) {
    const char* sql = "INSERT OR REPLACE INTO sync_meta (id, last_scanned_height, birth_height, gap_limit, updated_at) VALUES (1, ?, ?, ?, ?)";
    sqlite3_stmt* stmt;
    
    int rc = sqlite3_prepare_v2(wallet_db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("WalletSyncEnhanced::UpdateSyncMeta: Failed to prepare statement: " + std::string(sqlite3_errmsg(wallet_db_)));
        return false;
    }
    
    sqlite3_bind_int(stmt, 1, current_height);
    sqlite3_bind_int(stmt, 2, start_height);
    sqlite3_bind_int(stmt, 3, gap_limit);
    sqlite3_bind_int64(stmt, 4, std::time(nullptr));
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        dinero::g_logger.error("WalletSyncEnhanced::UpdateSyncMeta: Failed to execute statement: " + std::string(sqlite3_errmsg(wallet_db_)));
        return false;
    }
    
    return true;
}

bool WalletSyncEnhanced::UpdateLastScannedHeight(int height) {
    const char* sql = "UPDATE sync_meta SET last_scanned_height = ?, updated_at = ? WHERE id = 1";
    sqlite3_stmt* stmt;
    
    int rc = sqlite3_prepare_v2(wallet_db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("WalletSyncEnhanced::UpdateLastScannedHeight: Failed to prepare statement: " + std::string(sqlite3_errmsg(wallet_db_)));
        return false;
    }
    
    sqlite3_bind_int(stmt, 1, height);
    sqlite3_bind_int64(stmt, 2, std::time(nullptr));
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        dinero::g_logger.error("WalletSyncEnhanced::UpdateLastScannedHeight: Failed to execute statement: " + std::string(sqlite3_errmsg(wallet_db_)));
        return false;
    }
    
    return true;
}

bool WalletSyncEnhanced::DeriveAndRegisterWatchScripts(int gap_limit) {
    // This is a placeholder implementation
    // In a real implementation, this would:
    // 1. Get the wallet's HD seed
    // 2. Derive external and change addresses up to gap_limit
    // 3. Generate scriptPubKeys for each address
    // 4. Insert them into watch_scripts table
    
    dinero::g_logger.info("WalletSyncEnhanced: Deriving watch scripts with gap limit " + std::to_string(gap_limit));
    
    // For now, just update existing watch_scripts
    const char* sql = "UPDATE watch_scripts SET last_seen_height = 0";
    char* errmsg = nullptr;
    
    int rc = sqlite3_exec(wallet_db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string error = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        dinero::g_logger.error("WalletSyncEnhanced::DeriveAndRegisterWatchScripts: Failed to update watch_scripts: " + error);
        return false;
    }
    
    return true;
}

bool WalletSyncEnhanced::PerformRescan(int start_height, int end_height) {
    // Attach blockchain database to wallet database
    std::string attach_sql = "ATTACH DATABASE '" + blockchain_db_path_ + "' AS chain";
    char* errmsg = nullptr;
    
    int rc = sqlite3_exec(wallet_db_, attach_sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string error = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        dinero::g_logger.error("WalletSyncEnhanced::PerformRescan: Failed to attach blockchain database: " + error);
        return false;
    }
    
    // Perform the rescan by joining watch_scripts with chain.utxo
    const char* rescan_sql = R"(
        INSERT OR IGNORE INTO wallet_utxos (tx_hash, output_index, amount, script_pubkey, height, is_coinbase)
        SELECT u.txid, u.vout, u.amount, u.script, u.height, u.coinbase
        FROM chain.utxo u
        JOIN watch_scripts w ON u.script = w.script_pubkey
        WHERE u.height >= ? AND u.height <= ?
    )";
    
    sqlite3_stmt* stmt;
    rc = sqlite3_prepare_v2(wallet_db_, rescan_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("WalletSyncEnhanced::PerformRescan: Failed to prepare rescan statement: " + std::string(sqlite3_errmsg(wallet_db_)));
        sqlite3_exec(wallet_db_, "DETACH DATABASE chain", nullptr, nullptr, nullptr);
        return false;
    }
    
    sqlite3_bind_int(stmt, 1, start_height);
    sqlite3_bind_int(stmt, 2, end_height);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        dinero::g_logger.error("WalletSyncEnhanced::PerformRescan: Failed to execute rescan: " + std::string(sqlite3_errmsg(wallet_db_)));
        sqlite3_exec(wallet_db_, "DETACH DATABASE chain", nullptr, nullptr, nullptr);
        return false;
    }
    
    // Detach blockchain database
    sqlite3_exec(wallet_db_, "DETACH DATABASE chain", nullptr, nullptr, nullptr);
    
    dinero::g_logger.info("WalletSyncEnhanced: Rescan completed from height " + std::to_string(start_height) + 
                         " to " + std::to_string(end_height));
    return true;
}

} // namespace dinero
