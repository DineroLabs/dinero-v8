#if !DIN_WITH_ROCKSDB
// RocksDB disabled — compile a stub TU so linking succeeds.
void __dinero_stub_database_utxo_view(){}
#else
#include "daemon/database_utxo_view.h"
#include "common/logger.h"
#include "common/blockchain_db.h"
#include "util/hex.h"
#include <sqlite3.h>
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace dinero {

DatabaseUTXOView::DatabaseUTXOView(const std::string& db_path) 
    : db_path_(db_path), db_(nullptr) {
}

DatabaseUTXOView::~DatabaseUTXOView() {
    if (db_) {
        sqlite3_close(db_);
    }
}

bool DatabaseUTXOView::Initialize() {
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("DatabaseUTXOView: Failed to open database: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }
    
    // Enable WAL mode for better concurrency
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size=10000", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA busy_timeout=5000", nullptr, nullptr, nullptr);
    
    // Ensure UTXO table exists with correct schema
    if (!EnsureUTXOTable()) {
        dinero::g_logger.error("DatabaseUTXOView: Failed to ensure UTXO table");
        return false;
    }
    
    dinero::g_logger.info("DatabaseUTXOView initialized with blockchain database: " + db_path_);
    return true;
}

bool DatabaseUTXOView::EnsureUTXOTable() {
    // Check if the table exists and has the expected schema
    const char* check_sql = "PRAGMA table_info(utxo);";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, check_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("DatabaseUTXOView::EnsureUTXOTable: Failed to check table info");
        return false;
    }
    
    bool has_height = false;
    bool has_coinbase = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* col_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (col_name && std::string(col_name) == "height") {
            has_height = true;
        }
        if (col_name && std::string(col_name) == "coinbase") {
            has_coinbase = true;
        }
    }
    sqlite3_finalize(stmt);
    
    // If table exists but missing columns, add them
    if (has_height && has_coinbase) {
        dinero::g_logger.info("DatabaseUTXOView: UTXO table schema is correct");
        return true;
    }
    
    // Add missing columns if they don't exist
    if (!has_height) {
        const char* add_height_sql = "ALTER TABLE utxo ADD COLUMN height INTEGER NOT NULL DEFAULT 0;";
        char* errmsg = nullptr;
        rc = sqlite3_exec(db_, add_height_sql, nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            dinero::g_logger.error("DatabaseUTXOView::EnsureUTXOTable: Failed to add height column: " + std::string(errmsg));
            sqlite3_free(errmsg);
            return false;
        }
        dinero::g_logger.info("DatabaseUTXOView: Added height column to UTXO table");
    }
    
    if (!has_coinbase) {
        const char* add_coinbase_sql = "ALTER TABLE utxo ADD COLUMN coinbase INTEGER NOT NULL DEFAULT 0;";
        char* errmsg = nullptr;
        rc = sqlite3_exec(db_, add_coinbase_sql, nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            dinero::g_logger.error("DatabaseUTXOView::EnsureUTXOTable: Failed to add coinbase column: " + std::string(errmsg));
            sqlite3_free(errmsg);
            return false;
        }
        dinero::g_logger.info("DatabaseUTXOView: Added coinbase column to UTXO table");
    }
    
    return true;
}

bool DatabaseUTXOView::HaveUTXO(const std::string& txid, uint32_t vout) const {
    if (!db_) {
        return false;
    }
    
    sqlite3_stmt* stmt;
        const char* sql = R"(
            SELECT 1 FROM utxo 
            WHERE tx_hash = ? AND output_index = ? AND is_spent = 0
            LIMIT 1
        )";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("DatabaseUTXOView::HaveUTXO: prepare failed: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }
    
    // Convert hex txid to bytes (canonical byte order)
    std::vector<uint8_t> txid_bytes = dinero::HexToBytes(txid);
    sqlite3_bind_blob(stmt, 1, txid_bytes.data(), txid_bytes.size(), SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, vout);
    
    bool result = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = true;
    }
    
    sqlite3_finalize(stmt);
    return result;
}

bool DatabaseUTXOView::GetUTXO(const std::string& txid, uint32_t vout, 
                               uint64_t& value, std::string& script) const {
    if (!db_) {
        return false;
    }
    
    sqlite3_stmt* stmt;
        const char* sql = R"(
            SELECT amount, script_pubkey FROM utxo 
            WHERE tx_hash = ? AND output_index = ? AND is_spent = 0
            LIMIT 1
        )";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("DatabaseUTXOView::GetUTXO: prepare failed: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }
    
    // Convert hex txid to bytes (canonical byte order)
    std::vector<uint8_t> txid_bytes = dinero::HexToBytes(txid);
    sqlite3_bind_blob(stmt, 1, txid_bytes.data(), txid_bytes.size(), SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, vout);
    
    bool result = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
        
        // Get script as blob and convert to hex string
        const void* script_blob = sqlite3_column_blob(stmt, 1);
        int script_size = sqlite3_column_bytes(stmt, 1);
        if (script_blob && script_size > 0) {
            const uint8_t* script_bytes = static_cast<const uint8_t*>(script_blob);
            std::vector<uint8_t> script_vec(script_bytes, script_bytes + script_size);
            // Convert bytes back to hex string
            std::ostringstream oss;
            for (uint8_t byte : script_vec) {
                oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
            }
            script = oss.str();
        } else {
            script = "";
        }
        result = true;
    }
    
    sqlite3_finalize(stmt);
    return result;
}

bool DatabaseUTXOView::HaveTransaction(const std::string& txid) const {
    if (!db_) {
        return false;
    }
    
    sqlite3_stmt* stmt;
        const char* sql = R"(
            SELECT 1 FROM utxo 
            WHERE tx_hash = ?
            LIMIT 1
        )";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("DatabaseUTXOView::HaveTransaction: prepare failed: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }
    
    // Convert hex txid to bytes (canonical byte order)
    std::vector<uint8_t> txid_bytes = dinero::HexToBytes(txid);
    sqlite3_bind_blob(stmt, 1, txid_bytes.data(), txid_bytes.size(), SQLITE_STATIC);
    
    bool result = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = true;
    }
    
    sqlite3_finalize(stmt);
    return result;
}

uint32_t DatabaseUTXOView::GetHeight() const {
    if (!db_) {
        return 0;
    }
    
    sqlite3_stmt* stmt;
        const char* sql = "SELECT MAX(block_height) FROM utxo";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("DatabaseUTXOView::GetHeight: prepare failed: " + std::string(sqlite3_errmsg(db_)));
        return 0;
    }
    
    uint32_t height = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        height = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return height;
}

// Helper functions moved to common/hex_utils.h

} // namespace dinero

#endif
