#pragma once
#include <sqlite3.h>
#include <chrono>
#include <thread>
#include <mutex>

inline std::mutex& db_writer_mutex() { static std::mutex m; return m; }

class Tx {
  sqlite3* db_;
  bool active_{false};
public:
  explicit Tx(sqlite3* db): db_(db) {
    std::lock_guard<std::mutex> lk(db_writer_mutex()); // single-writer guard
    // BEGIN IMMEDIATE + retry-on-BUSY
    for (int i=0; i<10; ++i) {
      if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK) {
        active_ = true;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50 * (i+1)));
    }
    throw std::runtime_error("Failed to begin transaction after retries");
  }
  void commit() {
    if (active_) {
      if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("COMMIT failed");
      active_ = false;
    }
  }
  ~Tx() { if (active_) sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr); }
};
