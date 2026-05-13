#pragma once
#include <sqlite3.h>
#include <string>
#include <stdexcept>

namespace dinero::db {
  // Updates meta table when connecting a new tip
  // Call this in your ConnectTip function to keep meta in sync
  inline bool UpdateTipMeta(sqlite3* db, int64_t height, const std::string& hash, const std::string& chainwork) {
    const char* sql = R"(
      BEGIN;
      INSERT INTO meta(key,value) VALUES('besthash', ?)
        ON CONFLICT(key) DO UPDATE SET value=excluded.value;
      INSERT INTO meta(key,value) VALUES('height', ?)
        ON CONFLICT(key) DO UPDATE SET value=excluded.value;
      INSERT INTO meta(key,value) VALUES('chainwork', ?)
        ON CONFLICT(key) DO UPDATE SET value=excluded.value;
      COMMIT;
    )";
    
    sqlite3_stmt* stmt = nullptr;
    bool success = false;
    
    try {
      // Begin transaction and prepare statement
      if (sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to begin transaction");
      }
      
      // Update besthash
      if (sqlite3_prepare_v2(db, "INSERT INTO meta(key,value) VALUES('besthash', ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value;", -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare besthash update");
      }
      sqlite3_bind_text(stmt, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw std::runtime_error("Failed to update besthash");
      }
      sqlite3_finalize(stmt);
      stmt = nullptr;
      
      // Update height
      std::string height_str = std::to_string(height);
      if (sqlite3_prepare_v2(db, "INSERT INTO meta(key,value) VALUES('height', ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value;", -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare height update");
      }
      sqlite3_bind_text(stmt, 1, height_str.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw std::runtime_error("Failed to update height");
      }
      sqlite3_finalize(stmt);
      stmt = nullptr;
      
      // Update chainwork
      if (sqlite3_prepare_v2(db, "INSERT INTO meta(key,value) VALUES('chainwork', ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value;", -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare chainwork update");
      }
      sqlite3_bind_text(stmt, 1, chainwork.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(stmt) != SQLITE_DONE) {
        throw std::runtime_error("Failed to update chainwork");
      }
      sqlite3_finalize(stmt);
      stmt = nullptr;
      
      // Commit transaction
      if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to commit transaction");
      }
      
      success = true;
      
    } catch (const std::exception& e) {
      // Rollback on error
      sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
      if (stmt) sqlite3_finalize(stmt);
      throw; // Re-throw the exception
    }
    
    return success;
  }
}
