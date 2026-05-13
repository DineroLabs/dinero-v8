#include "wallet/wallet_db_impl.h"
#include "sqlite_open.h"
#include <iostream>

namespace din {

WalletDBImpl::WalletDBImpl(const std::string& db_path) 
    : db_path_(db_path), db_(nullptr), in_batch_(false),
      stmt_put_(nullptr), stmt_get_(nullptr), stmt_remove_(nullptr), stmt_iterate_(nullptr) {}

WalletDBImpl::~WalletDBImpl() {
    shutdown();
}

bool WalletDBImpl::initialize() {
    try {
        db_ = open_sqlite(db_path_);
    } catch (const std::exception& e) {
        std::cerr << "Failed to open wallet database: " << e.what() << std::endl;
        return false;
    }
    
    if (!createTables()) {
        shutdown();
        return false;
    }
    
    if (!prepareStatements()) {
        shutdown();
        return false;
    }
    
    return true;
}

void WalletDBImpl::shutdown() {
    finalizeStatements();
    
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool WalletDBImpl::createTables() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS wallet_kv (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
        
        CREATE INDEX IF NOT EXISTS idx_wallet_kv_key ON wallet_kv(key);
    )";
    
    char* err_msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
        std::cerr << "Failed to create wallet tables: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }
    
    return true;
}

bool WalletDBImpl::prepareStatements() {
    // PUT statement
    if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO wallet_kv (key, value) VALUES (?, ?)", 
                          -1, &stmt_put_, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare PUT statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    // GET statement
    if (sqlite3_prepare_v2(db_, "SELECT value FROM wallet_kv WHERE key = ?", 
                          -1, &stmt_get_, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare GET statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    // REMOVE statement
    if (sqlite3_prepare_v2(db_, "DELETE FROM wallet_kv WHERE key = ?", 
                          -1, &stmt_remove_, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare REMOVE statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    // ITERATE statement
    if (sqlite3_prepare_v2(db_, "SELECT key, value FROM wallet_kv WHERE key LIKE ? ORDER BY key", 
                          -1, &stmt_iterate_, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare ITERATE statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    
    return true;
}

void WalletDBImpl::finalizeStatements() {
    if (stmt_put_) { sqlite3_finalize(stmt_put_); stmt_put_ = nullptr; }
    if (stmt_get_) { sqlite3_finalize(stmt_get_); stmt_get_ = nullptr; }
    if (stmt_remove_) { sqlite3_finalize(stmt_remove_); stmt_remove_ = nullptr; }
    if (stmt_iterate_) { sqlite3_finalize(stmt_iterate_); stmt_iterate_ = nullptr; }
}

bool WalletDBImpl::put(const std::string& key, const std::string& value) {
    if (!db_ || !stmt_put_) return false;
    
    sqlite3_reset(stmt_put_);
    sqlite3_bind_text(stmt_put_, 1, key.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_put_, 2, value.c_str(), -1, SQLITE_STATIC);
    
    return sqlite3_step(stmt_put_) == SQLITE_DONE;
}

std::optional<std::string> WalletDBImpl::get(const std::string& key) const {
    if (!db_ || !stmt_get_) return std::nullopt;
    
    sqlite3_reset(stmt_get_);
    sqlite3_bind_text(stmt_get_, 1, key.c_str(), -1, SQLITE_STATIC);
    
    if (sqlite3_step(stmt_get_) == SQLITE_ROW) {
        const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt_get_, 0));
        return std::string(value ? value : "");
    }
    
    return std::nullopt;
}

bool WalletDBImpl::remove(const std::string& key) {
    if (!db_ || !stmt_remove_) return false;
    
    sqlite3_reset(stmt_remove_);
    sqlite3_bind_text(stmt_remove_, 1, key.c_str(), -1, SQLITE_STATIC);
    
    return sqlite3_step(stmt_remove_) == SQLITE_DONE;
}

bool WalletDBImpl::beginBatch() {
    if (!db_ || in_batch_) return false;
    
    if (sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, nullptr) == SQLITE_OK) {
        in_batch_ = true;
        return true;
    }
    return false;
}

bool WalletDBImpl::commitBatch() {
    if (!db_ || !in_batch_) return false;
    
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK) {
        in_batch_ = false;
        return true;
    }
    return false;
}

bool WalletDBImpl::abortBatch() {
    if (!db_ || !in_batch_) return false;
    
    if (sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK) {
        in_batch_ = false;
        return true;
    }
    return false;
}

bool WalletDBImpl::iterate(const std::string& prefix, 
                          std::function<bool(const std::string&, const std::string&)> callback) const {
    if (!db_ || !stmt_iterate_) return false;
    
    sqlite3_reset(stmt_iterate_);
    std::string pattern = prefix + "%";
    sqlite3_bind_text(stmt_iterate_, 1, pattern.c_str(), -1, SQLITE_STATIC);
    
    while (sqlite3_step(stmt_iterate_) == SQLITE_ROW) {
        const char* key = reinterpret_cast<const char*>(sqlite3_column_text(stmt_iterate_, 0));
        const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt_iterate_, 1));
        
        if (!callback(key ? key : "", value ? value : "")) {
            break; // Callback requested stop
        }
    }
    
    return true;
}

} // namespace din
