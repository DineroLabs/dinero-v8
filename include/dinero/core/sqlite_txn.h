#pragma once
#include <sqlite3.h>
#include <string>
#include <stdexcept>
#include <atomic>

class SqliteTxn {
public:
  enum class Mode { Immediate, Deferred, SavepointAuto };

  explicit SqliteTxn(sqlite3* db, Mode mode = Mode::Immediate)
      : db_(db), savepoint_(false), committed_(false) {
    // Are we already inside a transaction?
    const bool in_txn = sqlite3_get_autocommit(db_) == 0;

    if (in_txn) {
      savepoint_ = true;
      name_ = make_sp_name();
      exec("SAVEPOINT " + name_);
    } else {
      if (mode == Mode::Immediate)
        exec("BEGIN IMMEDIATE");
      else
        exec("BEGIN");
    }
  }

  void commit() {
    if (committed_) return;
    if (savepoint_)
      exec("RELEASE SAVEPOINT " + name_);
    else
      exec("COMMIT");
    committed_ = true;
  }

  ~SqliteTxn() {
    if (!committed_) {
      if (savepoint_) exec("ROLLBACK TO " + name_), exec("RELEASE " + name_);
      else exec("ROLLBACK");
    }
  }

private:
  sqlite3* db_;
  bool savepoint_;
  bool committed_;
  std::string name_;

  static std::string make_sp_name() {
    static std::atomic<uint64_t> ctr{0};
    return "sp_din_" + std::to_string(++ctr);
  }

  void exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
      std::string msg = err ? err : "unknown";
      sqlite3_free(err);
      throw std::runtime_error("sqlite exec failed: " + sql + " => " + msg);
    }
  }
};
