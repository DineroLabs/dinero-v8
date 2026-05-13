#include "sqlite_open.h"
#include <sstream>

static inline std::string last_err(sqlite3* db) {
  const char* m = sqlite3_errmsg(db);
  return m ? std::string(m) : std::string();
}

SqliteOpenResult open_sqlite(const std::string& path) {
  SqliteOpenResult res;
  // Serialized mode to be safe across threads.
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  res.rc = sqlite3_open_v2(path.c_str(), &res.db, flags, nullptr);
  if (res.rc != SQLITE_OK) {
    res.errmsg = last_err(res.db);
    return res;
  }

  // Busy timeout **must** be set via C API (not just PRAGMA).
  int busy_ms = 5000;               // 5s timeout for SQLite operations
  sqlite3_busy_timeout(res.db, busy_ms);

  if (!apply_sqlite_pragmas(res.db)) {
    res.rc = SQLITE_ERROR;
    res.errmsg = "Failed to apply PRAGMAs: " + last_err(res.db);
  }
  return res;
}

bool apply_sqlite_pragmas(sqlite3* db) {
  // Complete SQLite golden settings for production use
  // Set busy_timeout in PRAGMA so it's visible in queries (mirrors API call)
  char* err = nullptr;
  if (sqlite3_exec(db,
      "PRAGMA journal_mode=WAL;"
      "PRAGMA synchronous=NORMAL;"     // Safer than OFF; good balance for WAL
      "PRAGMA locking_mode=NORMAL;"
      "PRAGMA foreign_keys=ON;"
      "PRAGMA temp_store=MEMORY;"
      "PRAGMA wal_autocheckpoint=1000;"
      "PRAGMA busy_timeout=5000;"      // Mirrors the API call for visibility
      "PRAGMA cache_size=10000;"       // 10MB cache per connection
      "PRAGMA mmap_size=67108864;",     // 64MB memory-mapped I/O
      nullptr, nullptr, &err) != SQLITE_OK) {
    if (err) {
      std::string errmsg(err);
      sqlite3_free(err);
      return false;
    }
  }
  return true;
}
