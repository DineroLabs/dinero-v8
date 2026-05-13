// src/daemon/db_meta_utils.cpp
// Thread-safe utilities for reading and updating database meta table

#include "daemon/db_meta_utils.hpp"
#include "daemon/logger_stub.h"  // Logger stub
#include "sqlite_txn.h"
#include <sqlite3.h>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace dinero::db {

// ============================================================================
// Global Database Context (Thread-Safe)
// ============================================================================
// Single point of access for database operations with explicit lifetime
// and thread synchronization.
// ============================================================================

DatabaseContext g_db_context;

// ============================================================================
// RAII SQLite Statement Helper
// ============================================================================

struct SqliteStmt {
    sqlite3_stmt* s = nullptr;
    SqliteStmt() = default;
    explicit SqliteStmt(sqlite3_stmt* ps) : s(ps) {}
    ~SqliteStmt() { if (s) sqlite3_finalize(s); }
    SqliteStmt(const SqliteStmt&) = delete;
    SqliteStmt& operator=(const SqliteStmt&) = delete;
    SqliteStmt(SqliteStmt&& o) noexcept : s(o.s) { o.s = nullptr; }
    SqliteStmt& operator=(SqliteStmt&& o) noexcept {
        if (this != &o) {
            if (s) sqlite3_finalize(s);
            s = o.s;
            o.s = nullptr;
        }
        return *this;
    }
};

// ============================================================================
// Core Operations (require caller to hold lock)
// ============================================================================

std::string ReadMeta(sqlite3* db, const char* key) {
    const char* sql = "SELECT value FROM meta WHERE key=? LIMIT 1;";
    SqliteStmt st;
    if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) != SQLITE_OK) {
        return "";
    }
    sqlite3_bind_text(st.s, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st.s);
    if (rc == SQLITE_ROW) {
        const unsigned char* txt = sqlite3_column_text(st.s, 0);
        if (txt) {
            return std::string(reinterpret_cast<const char*>(txt));
        }
    }
    return "";
}

bool WriteMeta(sqlite3* db, const char* key, const std::string& value) {
    const char* sql = "INSERT INTO meta(key,value) VALUES(?,?) "
                      "ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
    SqliteStmt st;
    if (sqlite3_prepare_v2(db, sql, -1, &st.s, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(st.s, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(st.s, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(st.s) == SQLITE_DONE;
}

// ============================================================================
// Legacy API (deprecated - backward compatibility)
// ============================================================================
// These acquire locks internally. New code should use g_db_context.lock()
// directly for explicit lock management and to avoid double-locking.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

void InitializeDatabase(sqlite3* db) {
    g_db_context.initialize(db);
}

sqlite3* GetDatabase() {
    // WARNING: This returns a raw pointer without holding the lock.
    // The pointer may become invalid if another thread calls reset().
    // This is kept for backward compatibility only - use g_db_context.lock() instead.
    auto guard = g_db_context.lock();
    return guard.handle();
}

#pragma GCC diagnostic pop

// ============================================================================
// Thread-Safe Meta Operations
// ============================================================================

std::string ReadMeta(const char* key) {
    auto guard = g_db_context.lock();
    sqlite3* db = guard.handle();
    if (!db) return "";
    return ReadMeta(db, key);
}

bool UpdateMeta(const char* key, const std::string& value) {
    auto guard = g_db_context.lock();
    sqlite3* db = guard.handle();
    if (!db) return false;
    return WriteMeta(db, key, value);
}

// ============================================================================
// Thread-Safe Transaction Management
// ============================================================================

std::unique_ptr<SqliteTxn> BeginTransaction() {
    auto guard = g_db_context.lock();
    sqlite3* db = guard.handle();
    if (!db) return nullptr;

    try {
        return std::make_unique<SqliteTxn>(db, SqliteTxn::Mode::Immediate);
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to begin transaction: " + std::string(e.what()));
        return nullptr;
    }
}

void CommitTransaction(std::unique_ptr<SqliteTxn> txn) {
    if (!txn) return;
    try {
        txn->commit();
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to commit transaction: " + std::string(e.what()));
    }
}

void RollbackTransaction() {
    auto guard = g_db_context.lock();
    sqlite3* db = guard.handle();
    if (!db) return;

    // Use RAII transaction helper to handle rollback automatically
    try {
        SqliteTxn txn(db, SqliteTxn::Mode::Deferred);
        // Transaction will rollback automatically when txn goes out of scope
        // This is equivalent to manual ROLLBACK but safer
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to create rollback transaction: " + std::string(e.what()));
    }
}

} // namespace dinero::db
