#include "wallet/wallet_balance_service.h"
#include "common/logger.h"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace dinero {

WalletBalanceService::WalletBalanceService(sqlite3* wallet_db, const std::string& explorer_db_path) 
    : wallet_db_(wallet_db), explorer_db_path_(explorer_db_path) {
}

WalletBalanceService::~WalletBalanceService() {
    Shutdown();
}

bool WalletBalanceService::Initialize() {
    // CRITICAL FIX: Use shared database connection, don't open own
    if (!wallet_db_) {
        g_logger.error("WalletBalanceService: No shared wallet database provided");
        return false;
    }
    
    // Create tables if they don't exist
    if (!CreateTables()) {
        return false;
    }
    
    // NOTE: ExplorerDB attachment removed (December 2025)
    // Balance tracking now uses ChainDB (RocksDB) directly
    
    g_logger.info("Wallet balance service initialized with shared database connection");
    return true;
}

void WalletBalanceService::Shutdown() {
    // CRITICAL FIX: Don't close shared database connection
    // The SQLiteManager owns the connection and will close it
    wallet_db_ = nullptr;
}

bool WalletBalanceService::CreateTables() {
    const char* sql_addresses = R"(
        CREATE TABLE IF NOT EXISTS wallet_addresses (
            addr TEXT PRIMARY KEY,
            script_pubkey TEXT NOT NULL,
            derivation_path TEXT,
            purpose TEXT DEFAULT 'receive',
            created_at INTEGER DEFAULT (strftime('%s', 'now'))
        );
    )";
    
    const char* sql_tip = R"(
        CREATE TABLE IF NOT EXISTS tip (
            height INTEGER PRIMARY KEY
        );
        INSERT OR IGNORE INTO tip (height) VALUES (0);
    )";
    
    char* err_msg = nullptr;
    
    if (sqlite3_exec(wallet_db_, sql_addresses, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        g_logger.error("Failed to create wallet_addresses table: " + std::string(err_msg));
        sqlite3_free(err_msg);
        return false;
    }
    
    if (sqlite3_exec(wallet_db_, sql_tip, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        g_logger.error("Failed to create tip table: " + std::string(err_msg));
        sqlite3_free(err_msg);
        return false;
    }
    
    return true;
}

bool WalletBalanceService::AttachExplorerDB() {
    // NOTE: ExplorerDB has been removed (December 2025)
    // Balance tracking now uses ChainDB (RocksDB) directly
    // This stub remains for API compatibility
    return true;
}

// Removed CreateViews() method - SQLite doesn't support views referencing attached databases
// All queries are now performed directly in GetBalance() and ListUnspent() methods

WalletBalance WalletBalanceService::GetBalance(bool include_unconfirmed, bool include_immature) {
    WalletBalance balance;
    
    // Direct query to attached explorer database
    const char* sql = R"(
        WITH current_tip AS (SELECT height FROM tip LIMIT 1)
        SELECT
            COALESCE(SUM(CASE
                WHEN u.spk_hex LIKE '%6a%' AND ((SELECT height FROM current_tip) - u.height + 1) < 100 THEN 0
                WHEN u.height IS NULL THEN 0
                ELSE u.value 
            END), 0) AS confirmed_spendable,
            COALESCE(SUM(CASE 
                WHEN u.height IS NULL THEN u.value 
                ELSE 0 
            END), 0) AS unconfirmed,
            COALESCE(SUM(CASE 
                WHEN u.spk_hex LIKE '%6a%' AND ((SELECT height FROM current_tip) - u.height + 1) < 100 THEN u.value 
                ELSE 0 
            END), 0) AS immature
        FROM explorer.addr_utxo u
        JOIN wallet_addresses a ON a.script_pubkey = u.spk_hex
        WHERE u.is_spent = 0
    )";
    
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(wallet_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare balance query: " + std::string(sqlite3_errmsg(wallet_db_)));
        return balance;
    }
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        balance.confirmed_spendable = sqlite3_column_int64(stmt, 0);
        balance.unconfirmed = sqlite3_column_int64(stmt, 1);
        balance.immature = sqlite3_column_int64(stmt, 2);
        
        balance.total_spendable = balance.confirmed_spendable;
        if (include_unconfirmed) {
            balance.total_spendable += balance.unconfirmed;
        }
        if (include_immature) {
            balance.total_spendable += balance.immature;
        }
    }
    
    sqlite3_finalize(stmt);
    return balance;
}

std::vector<WalletUTXO> WalletBalanceService::ListUnspent(int minconf, int maxconf) {
    std::vector<WalletUTXO> utxos;
    
    // Direct query to attached explorer database
    const char* sql = R"(
        SELECT u.txid, u.vout, u.value, u.spk_hex, u.height,
               CASE WHEN u.height IS NULL THEN 0 
                    ELSE (SELECT height FROM tip) - u.height + 1 
               END AS confirmations,
               CASE WHEN u.spk_hex LIKE '%6a%' THEN 1 ELSE 0 END AS is_coinbase
        FROM explorer.addr_utxo u
        JOIN wallet_addresses a ON a.script_pubkey = u.spk_hex
        WHERE u.is_spent = 0 
          AND (CASE WHEN u.height IS NULL THEN 0 ELSE (SELECT height FROM tip) - u.height + 1 END) >= ?
          AND (CASE WHEN u.height IS NULL THEN 0 ELSE (SELECT height FROM tip) - u.height + 1 END) <= ?
        ORDER BY u.value DESC
    )";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(wallet_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare listunspent query: " + std::string(sqlite3_errmsg(wallet_db_)));
        return utxos;
    }
    
    sqlite3_bind_int(stmt, 1, minconf);
    sqlite3_bind_int(stmt, 2, maxconf);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WalletUTXO utxo;
        utxo.txid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        utxo.vout = sqlite3_column_int(stmt, 1);
        utxo.value = sqlite3_column_int64(stmt, 2);
        utxo.scriptpubkey = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        utxo.height = sqlite3_column_int(stmt, 4);
        utxo.confirmations = sqlite3_column_int(stmt, 5);
        utxo.is_coinbase = sqlite3_column_int(stmt, 6) != 0;
        
        // Check if spendable (coinbase maturity)
        utxo.is_spendable = !utxo.is_coinbase || utxo.confirmations >= COINBASE_MATURITY;
        
        utxos.push_back(utxo);
    }
    
    sqlite3_finalize(stmt);
    return utxos;
}

bool WalletBalanceService::AddWalletAddress(const std::string& address, const std::string& scriptpubkey_hex,
                                          const std::string& derivation_path, const std::string& purpose) {
    const char* sql = R"(
        INSERT OR REPLACE INTO wallet_addresses (addr, script_pubkey, derivation_path, purpose)
        VALUES (?, ?, ?, ?)
    )";
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(wallet_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare add address statement: " + std::string(sqlite3_errmsg(wallet_db_)));
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, address.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, scriptpubkey_hex.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, derivation_path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, purpose.c_str(), -1, SQLITE_STATIC);
    
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        g_logger.error("Failed to add wallet address: " + std::string(sqlite3_errmsg(wallet_db_)));
        return false;
    }
    
    g_logger.info("Added wallet address: " + address);
    return true;
}

std::vector<std::string> WalletBalanceService::GetWalletAddresses() {
    std::vector<std::string> addresses;
    
    const char* sql = "SELECT addr FROM wallet_addresses ORDER BY created_at";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(wallet_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare get addresses query: " + std::string(sqlite3_errmsg(wallet_db_)));
        return addresses;
    }
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        addresses.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    
    sqlite3_finalize(stmt);
    return addresses;
}

void WalletBalanceService::OnBlockConnected(int height) {
    // Update tip height
    const char* sql = "UPDATE tip SET height = ?";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(wallet_db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, height);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    
    g_logger.debug("Wallet balance service: block connected at height " + std::to_string(height));
}

void WalletBalanceService::OnBlockDisconnected(int height) {
    // Update tip height (rollback)
    const char* sql = "UPDATE tip SET height = ?";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(wallet_db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, height - 1);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    
    g_logger.debug("Wallet balance service: block disconnected from height " + std::to_string(height));
}

void WalletBalanceService::OnMempoolTxAdded(const std::string& txid) {
    g_logger.debug("Wallet balance service: mempool tx added " + txid);
}

void WalletBalanceService::OnMempoolTxRemoved(const std::string& txid) {
    g_logger.debug("Wallet balance service: mempool tx removed " + txid);
}

int WalletBalanceService::GetCurrentTipHeight() {
    const char* sql = "SELECT height FROM tip LIMIT 1";
    sqlite3_stmt* stmt;
    int height = 0;
    
    if (sqlite3_prepare_v2(wallet_db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            height = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    
    return height;
}

std::string WalletBalanceService::ScriptPubKeyToHex(const std::string& scriptpubkey) {
    std::stringstream ss;
    for (unsigned char c : scriptpubkey) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
    return ss.str();
}

std::string WalletBalanceService::HexToScriptPubKey(const std::string& hex) {
    std::string result;
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        char byte = static_cast<char>(std::stoi(byteString, nullptr, 16));
        result += byte;
    }
    return result;
}

} // namespace dinero
