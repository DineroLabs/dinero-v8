#include "daemon/chainstate_harden.h"
#include "common/logger.h"
#include "sqlite_txn.h"
#include <sqlite3.h>
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace dinero {

ChainstateHarden::ChainstateHarden(const std::string& db_path) 
    : db_path_(db_path), db_(nullptr) {
}

ChainstateHarden::~ChainstateHarden() {
    if (db_) {
        sqlite3_close(db_);
    }
}

bool ChainstateHarden::Initialize() {
    int rc = sqlite3_open(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        dinero::g_logger.error("ChainstateHarden: Failed to open database: " + std::string(sqlite3_errmsg(db_)));
        return false;
    }
    
    // Enable WAL mode and other performance settings
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA cache_size=10000", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA busy_timeout=5000", nullptr, nullptr, nullptr);
    
    dinero::g_logger.info("ChainstateHarden initialized with database: " + db_path_);
    return true;
}

bool ChainstateHarden::ApplyMigration002() {
    if (!db_) {
        dinero::g_logger.error("ChainstateHarden::ApplyMigration002: Database not initialized");
        return false;
    }
    
    // Check if migration already applied
    if (IsMigrationApplied("migration_002_applied")) {
        dinero::g_logger.info("ChainstateHarden: Migration 002 already applied");
        return true;
    }
    
    try {
        SqliteTxn txn(db_, SqliteTxn::Mode::Immediate);
        
        // Apply migration SQL
        const char* migration_sql = R"(
            -- Add covering indexes for common query patterns
            CREATE INDEX IF NOT EXISTS idx_utxo_script ON utxo(script);
            CREATE INDEX IF NOT EXISTS idx_utxo_height ON utxo(height);
            CREATE INDEX IF NOT EXISTS idx_utxo_coinbase ON utxo(coinbase);
            CREATE INDEX IF NOT EXISTS idx_utxo_amount ON utxo(amount);
            
            -- Add composite indexes for wallet queries
            CREATE INDEX IF NOT EXISTS idx_utxo_script_height ON utxo(script, height);
            CREATE INDEX IF NOT EXISTS idx_utxo_script_coinbase ON utxo(script, coinbase);
            
            -- Add metadata for migration tracking
            INSERT OR REPLACE INTO meta (key, value) VALUES ('migration_002_applied', '1');
            INSERT OR REPLACE INTO meta (key, value) VALUES ('migration_002_timestamp', CAST(strftime('%s', 'now') AS TEXT));
        )";
        
        char* errmsg = nullptr;
        int rc = sqlite3_exec(db_, migration_sql, nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            std::string error = errmsg ? errmsg : "unknown error";
            sqlite3_free(errmsg);
            throw std::runtime_error("Migration 002 failed: " + error);
        }
        
        txn.commit();
        dinero::g_logger.info("ChainstateHarden: Migration 002 applied successfully");
        return true;
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("ChainstateHarden::ApplyMigration002 failed: " + std::string(e.what()));
        return false;
    }
}

bool ChainstateHarden::IsMigrationApplied(const std::string& migration_key) const {
    if (!db_) {
        return false;
    }
    
    sqlite3_stmt* stmt;
    const char* sql = "SELECT value FROM meta WHERE key = ?";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_text(stmt, 1, migration_key.c_str(), -1, SQLITE_STATIC);
    
    bool applied = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        applied = (value && std::string(value) == "1");
    }
    
    sqlite3_finalize(stmt);
    return applied;
}

bool ChainstateHarden::ValidateUTXOIntegrity() const {
    if (!db_) {
        dinero::g_logger.error("ChainstateHarden::ValidateUTXOIntegrity: Database not initialized");
        return false;
    }
    
    try {
        // Check for duplicate UTXOs (should be impossible with PRIMARY KEY, but verify)
        const char* duplicate_sql = R"(
            SELECT txid, vout, COUNT(*) as count 
            FROM utxo 
            GROUP BY txid, vout 
            HAVING COUNT(*) > 1
        )";
        
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db_, duplicate_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            dinero::g_logger.error("ChainstateHarden::ValidateUTXOIntegrity: Failed to prepare duplicate check");
            return false;
        }
        
        bool has_duplicates = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            has_duplicates = true;
            const void* txid_blob = sqlite3_column_blob(stmt, 0);
            int txid_size = sqlite3_column_bytes(stmt, 0);
            int vout = sqlite3_column_int(stmt, 1);
            int count = sqlite3_column_int(stmt, 2);
            
            // Convert txid to hex for logging
            std::ostringstream oss;
            const uint8_t* txid_bytes = static_cast<const uint8_t*>(txid_blob);
            for (int i = 0; i < txid_size; i++) {
                oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(txid_bytes[i]);
            }
            
            dinero::g_logger.error("ChainstateHarden: Duplicate UTXO found: " + oss.str() + ":" + std::to_string(vout) + " (count: " + std::to_string(count) + ")");
        }
        sqlite3_finalize(stmt);
        
        if (has_duplicates) {
            dinero::g_logger.error("ChainstateHarden: UTXO integrity check failed - duplicates found");
            return false;
        }
        
        // Check for invalid amounts (should be > 0)
        const char* invalid_amount_sql = "SELECT COUNT(*) FROM utxo WHERE amount <= 0";
        rc = sqlite3_prepare_v2(db_, invalid_amount_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            dinero::g_logger.error("ChainstateHarden::ValidateUTXOIntegrity: Failed to prepare amount check");
            return false;
        }
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int invalid_count = sqlite3_column_int(stmt, 0);
            if (invalid_count > 0) {
                dinero::g_logger.error("ChainstateHarden: UTXO integrity check failed - " + std::to_string(invalid_count) + " UTXOs with invalid amounts");
                sqlite3_finalize(stmt);
                return false;
            }
        }
        sqlite3_finalize(stmt);
        
        // Check for invalid heights (should be >= 0)
        const char* invalid_height_sql = "SELECT COUNT(*) FROM utxo WHERE height < 0";
        rc = sqlite3_prepare_v2(db_, invalid_height_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            dinero::g_logger.error("ChainstateHarden::ValidateUTXOIntegrity: Failed to prepare height check");
            return false;
        }
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int invalid_count = sqlite3_column_int(stmt, 0);
            if (invalid_count > 0) {
                dinero::g_logger.error("ChainstateHarden: UTXO integrity check failed - " + std::to_string(invalid_count) + " UTXOs with invalid heights");
                sqlite3_finalize(stmt);
                return false;
            }
        }
        sqlite3_finalize(stmt);
        
        dinero::g_logger.info("ChainstateHarden: UTXO integrity check passed");
        return true;
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("ChainstateHarden::ValidateUTXOIntegrity failed: " + std::string(e.what()));
        return false;
    }
}

bool ChainstateHarden::ReindexUTXOs() {
    if (!db_) {
        dinero::g_logger.error("ChainstateHarden::ReindexUTXOs: Database not initialized");
        return false;
    }
    
    try {
        SqliteTxn txn(db_, SqliteTxn::Mode::Immediate);
        
        // Clear existing UTXOs
        const char* clear_sql = "DELETE FROM utxo";
        char* errmsg = nullptr;
        int rc = sqlite3_exec(db_, clear_sql, nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            std::string error = errmsg ? errmsg : "unknown error";
            sqlite3_free(errmsg);
            throw std::runtime_error("Failed to clear UTXOs: " + error);
        }
        
        // TODO: Implement block-by-block UTXO reconstruction
        // This would require:
        // 1. Reading blocks from storage (files or database)
        // 2. Parsing transactions
        // 3. Tracking UTXO creation and spending
        // 4. Rebuilding the UTXO set
        
        dinero::g_logger.info("ChainstateHarden::ReindexUTXOs: Reindexing not yet implemented - UTXO table cleared");
        
        txn.commit();
        return true;
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("ChainstateHarden::ReindexUTXOs failed: " + std::string(e.what()));
        return false;
    }
}

uint64_t ChainstateHarden::GetUTXOCount() const {
    if (!db_) {
        return 0;
    }
    
    sqlite3_stmt* stmt;
    const char* sql = "SELECT COUNT(*) FROM utxo";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return 0;
    }
    
    uint64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return count;
}

uint64_t ChainstateHarden::GetTotalSupply() const {
    if (!db_) {
        return 0;
    }
    
    sqlite3_stmt* stmt;
    const char* sql = "SELECT SUM(amount) FROM utxo";
    
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return 0;
    }
    
    uint64_t total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = sqlite3_column_int64(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return total;
}

} // namespace dinero
