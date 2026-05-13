#pragma once
#include <sqlite3.h>
#include <string>
#include <stdexcept>

namespace Dinero {
namespace Wallet {

enum class Durability { FAST, NORMAL, SAFE };  // FAST=bench, NORMAL=prod, SAFE=chaos

inline const char* to_cstr(Durability d) {
  switch (d) { 
    case Durability::FAST: return "FAST";
    case Durability::NORMAL: return "NORMAL";
    case Durability::SAFE: return "SAFE"; 
  }
  return "UNKNOWN";
}

inline void exec_or_throw(sqlite3* db, const char* sql){
    char* err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "exec failed";
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec: " + msg + " SQL: " + sql);
    }
}

inline void apply_wallet_pragmas(sqlite3* db, Durability d) {
  // Safety + determinism
  sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
  sqlite3_busy_timeout(db, 10'000);               // avoid "database is locked" blips
  exec_or_throw(db, "PRAGMA foreign_keys=ON;");
  exec_or_throw(db, "PRAGMA locking_mode=NORMAL;");
  exec_or_throw(db, "PRAGMA temp_store=MEMORY;");
  exec_or_throw(db, "PRAGMA mmap_size=0;");

  // WAL + durability profile
  exec_or_throw(db, "PRAGMA journal_mode=WAL;");
  switch (d) {
    case Durability::FAST:   exec_or_throw(db, "PRAGMA synchronous=OFF;");    break;
    case Durability::NORMAL: exec_or_throw(db, "PRAGMA synchronous=NORMAL;"); break; // prod default
    case Durability::SAFE:   exec_or_throw(db, "PRAGMA synchronous=FULL;");   break; // chaos/kill-9
  }

  // Keep WAL under control
  exec_or_throw(db, "PRAGMA wal_autocheckpoint=1000;");
  exec_or_throw(db, "PRAGMA journal_size_limit=67108864;"); // 64MB cap
}

inline void wal_startup_checkpoint(sqlite3* db) {
  int log=0, ckpt=0;
  // Replays stale WAL after an unclean exit and shrinks if possible.
  sqlite3_wal_checkpoint_v2(db, nullptr, SQLITE_CHECKPOINT_FULL, &log, &ckpt);
}

inline void wal_clean_shutdown(sqlite3* db) {
  int log=0, ckpt=0;
  // Leaves main DB compact for next open; harmless if no WAL.
  sqlite3_wal_checkpoint_v2(db, nullptr, SQLITE_CHECKPOINT_TRUNCATE, &log, &ckpt);
}

inline bool has_table(sqlite3* db, const char* name){
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;",
        -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        ok = (sqlite3_step(st) == SQLITE_ROW);
    }
    sqlite3_finalize(st);
    return ok;
}

inline bool has_column(sqlite3* db, const char* table, const char* col){
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, ("PRAGMA table_info(" + std::string(table) + ");").c_str(),
        -1, &st, nullptr) != SQLITE_OK) return false;
    bool found = false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
        if (name && std::string(name) == col) { 
            found = true; 
            break; 
        }
    }
    sqlite3_finalize(st);
    return found;
}

struct Savepoint {
    sqlite3* db; 
    std::string name; 
    bool done = false;
    
    Savepoint(sqlite3* d, std::string n): db(d), name(std::move(n)) {
        exec_or_throw(db, ("SAVEPOINT " + name).c_str());
    }
    
    void release() { 
        if (!done) { 
            exec_or_throw(db, ("RELEASE SAVEPOINT " + name).c_str()); 
            done = true; 
        } 
    }
    
    void rollback() { 
        if (!done) { 
            exec_or_throw(db, ("ROLLBACK TO " + name).c_str()); 
            exec_or_throw(db, ("RELEASE SAVEPOINT " + name).c_str()); 
            done = true; 
        } 
    }
    
    ~Savepoint() { 
        if (!done) { 
            try { 
                exec_or_throw(db, ("ROLLBACK TO " + name).c_str()); 
                exec_or_throw(db, ("RELEASE SAVEPOINT " + name).c_str()); 
            } catch(...) {} 
        } 
    }
};

} // namespace Wallet
} // namespace Dinero
