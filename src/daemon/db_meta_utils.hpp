#pragma once
#include <sqlite3.h>
#include <string>
#include <memory>
#include <mutex>
#include "../../include/sqlite_txn.h"

namespace dinero::db {

// ============================================================================
// Thread-Safe Database Context
// ============================================================================
// Owns the SQLite handle with explicit lifetime and thread-safe access.
// All database operations MUST go through this context to ensure safety.
//
// Usage:
//   auto guard = g_db_context.lock();
//   sqlite3* db = guard.handle();
//   // ... use db safely ...
//   // lock released when guard goes out of scope
// ============================================================================

class DatabaseContext {
public:
    // RAII lock guard that provides access to the database handle
    class Guard {
    public:
        explicit Guard(DatabaseContext& ctx)
            : ctx_(ctx), lock_(ctx.mutex_) {}

        // Non-copyable; move assignment is invalid because ctx_ is a reference.
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&&) = default;
        Guard& operator=(Guard&&) = delete;

        sqlite3* handle() const { return ctx_.db_; }
        explicit operator bool() const { return ctx_.db_ != nullptr; }

    private:
        DatabaseContext& ctx_;
        std::unique_lock<std::mutex> lock_;
    };

    DatabaseContext() = default;
    ~DatabaseContext() = default;

    // Non-copyable, non-movable (singleton-like usage)
    DatabaseContext(const DatabaseContext&) = delete;
    DatabaseContext& operator=(const DatabaseContext&) = delete;
    DatabaseContext(DatabaseContext&&) = delete;
    DatabaseContext& operator=(DatabaseContext&&) = delete;

    // Initialize with a database handle (takes ownership conceptually)
    // The actual sqlite3* lifetime is managed externally, but access is controlled here
    void initialize(sqlite3* db) {
        std::lock_guard<std::mutex> lock(mutex_);
        db_ = db;
    }

    // Clear the handle (e.g., on shutdown)
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        db_ = nullptr;
    }

    // Acquire exclusive access to the database
    [[nodiscard]] Guard lock() { return Guard(*this); }

    // Check if initialized (thread-safe)
    bool isInitialized() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return db_ != nullptr;
    }

private:
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;

    friend class Guard;
};

// Global database context (replaces raw g_database pointer)
extern DatabaseContext g_db_context;

// ============================================================================
// Legacy API (deprecated - use g_db_context.lock() directly for new code)
// ============================================================================
// These functions acquire the lock internally for backward compatibility.
// New code should use g_db_context.lock() for explicit lock management.

[[deprecated("Use g_db_context.lock() for explicit thread-safe access")]]
void InitializeDatabase(sqlite3* db);

[[deprecated("Use g_db_context.lock() for explicit thread-safe access")]]
sqlite3* GetDatabase();

// Meta table operations (thread-safe internally)
std::string ReadMeta(const char* key);
std::string ReadMeta(sqlite3* db, const char* key);
bool WriteMeta(sqlite3* db, const char* key, const std::string& value);
bool UpdateMeta(const char* key, const std::string& value);

// Transaction management (RAII approach)
std::unique_ptr<SqliteTxn> BeginTransaction();
void CommitTransaction(std::unique_ptr<SqliteTxn> txn);
void RollbackTransaction();

}
