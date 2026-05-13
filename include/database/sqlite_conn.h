#pragma once

#include <sqlite3.h>
#include <string>
#include <stdexcept>

namespace dinero {

/**
 * Per-thread SQLite connection helper
 * Implements the golden rule: One sqlite3* per thread
 */
struct SqliteConn {
    sqlite3* db{nullptr};
    
    explicit SqliteConn(const std::string& path) {
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
            throw std::runtime_error("Failed to open SQLite database: " + path);
        }
        
        // Configure for multi-threaded access with WAL mode
        Exec("PRAGMA journal_mode=WAL;");
        Exec("PRAGMA synchronous=NORMAL;");
        sqlite3_busy_timeout(db, 5000);  // 5 second timeout for lock contention
    }
    
    ~SqliteConn() { 
        if (db) {
            sqlite3_close(db);
        }
    }
    
    // Non-copyable, non-movable (each thread gets its own)
    SqliteConn(const SqliteConn&) = delete;
    SqliteConn& operator=(const SqliteConn&) = delete;
    SqliteConn(SqliteConn&&) = delete;
    SqliteConn& operator=(SqliteConn&&) = delete;
    
    void Exec(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string error_msg = err ? err : "Unknown SQLite error";
            sqlite3_free(err);
            throw std::runtime_error("SQLite exec failed: " + error_msg);
        }
    }
    
    sqlite3_stmt* Prepare(const char* sql) {
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error("SQLite prepare failed: " + std::string(sqlite3_errmsg(db)));
        }
        return stmt;
    }
};

} // namespace dinero
