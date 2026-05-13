#pragma once
#include <sqlite3.h>
#include <string>
#include <stdexcept>
#include <atomic>
#include <iostream>

class SqliteTxn {
public:
  enum class Mode { Immediate, Deferred, SavepointAuto };

  explicit SqliteTxn(sqlite3* db, Mode mode = Mode::Immediate)
      : db_(db), committed_(false) {
    // Simplified: Always use BEGIN, no nested savepoints for now
    // This avoids "SAVEPOINT => unknown" errors
    if (mode == Mode::Immediate)
      exec("BEGIN IMMEDIATE");
    else
      exec("BEGIN");
  }

  void commit() {
    if (committed_) return;
    exec("COMMIT");
    committed_ = true;
  }

  ~SqliteTxn() {
    if (!committed_) {
      exec("ROLLBACK");
    }
  }

private:
  sqlite3* db_;
  bool committed_;
  std::string name_;

  static std::string make_sp_name() {
    static std::atomic<uint64_t> ctr{0};
    return "sp_din_" + std::to_string(++ctr);
  }

  void exec(const std::string& sql) {
    char* err = nullptr;

    // Debug logging
    std::cerr << "[SQL DEBUG] Executing: " << sql << std::endl;
    std::cerr << "[SQL DEBUG] db_ pointer: " << (void*)db_ << std::endl;

    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    std::cerr << "[SQL DEBUG] Result code: " << rc << " (SQLITE_OK=" << SQLITE_OK << ")" << std::endl;

    if (rc != SQLITE_OK) {
      std::string msg = err ? err : "unknown";
      std::string errMsg = db_ ? sqlite3_errmsg(db_) : "null db";
      std::cerr << "[SQL DEBUG] Error from sqlite3_exec: " << msg << std::endl;
      std::cerr << "[SQL DEBUG] Error from sqlite3_errmsg: " << errMsg << std::endl;
      std::cerr << "[SQL DEBUG] Is autocommit? " << (db_ ? sqlite3_get_autocommit(db_) : -1) << std::endl;
      sqlite3_free(err);
      throw std::runtime_error("sqlite exec failed: " + sql + " => " + msg + " (errmsg: " + errMsg + ")");
    }
  }
};
