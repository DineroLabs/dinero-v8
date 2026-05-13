#pragma once
#include <sqlite3.h>
#include <string>

struct SqliteOpenResult {
  sqlite3* db = nullptr;
  int rc = SQLITE_ERROR;
  std::string errmsg;
};

SqliteOpenResult open_sqlite(const std::string& path);
bool apply_sqlite_pragmas(sqlite3* db);