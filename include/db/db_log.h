#pragma once
#include "common/logger.h"
#include <sqlite3.h>
#include <string>
#include <sstream>

namespace dinero {

struct DbProbe {
    static void afterOpen(sqlite3* db, const std::string& dbPath) {
        dinero::g_logger.info("[DB-OPEN] file=" + dbPath);
        
        auto pragma = [&](const char* psql) {
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db, psql, -1, &stmt, nullptr) == SQLITE_OK) {
                std::string vals;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* val = (const char*)sqlite3_column_text(stmt, 0);
                    if (val) vals += std::string(val) + " ";
                }
                sqlite3_finalize(stmt);
                dinero::g_logger.info("[DB-PRAGMA] " + std::string(psql) + " -> " + vals);
            }
        };
        
        pragma("PRAGMA user_version;");
        pragma("PRAGMA journal_mode;");
        pragma("PRAGMA foreign_keys;");
        pragma("PRAGMA query_only;");
        pragma("PRAGMA busy_timeout;");
    }
};

struct SqlLog {
    static bool prepare(sqlite3_stmt** stmt, sqlite3* db, const char* sql, const char* tag) {
        int rc = sqlite3_prepare_v2(db, sql, -1, stmt, nullptr);
        bool ok = (rc == SQLITE_OK);
        dinero::g_logger.info("[SQL-PREP] " + std::string(tag) + " " + std::string(sql) + " ok=" + (ok ? "true" : "false"));
        if (!ok) {
            dinero::g_logger.error("[SQL-PREP-ERR] " + std::string(tag) + " " + std::string(sqlite3_errmsg(db)));
        }
        return ok;
    }
    
    static bool exec(sqlite3_stmt* stmt, const char* tag) {
        int rc = sqlite3_step(stmt);
        bool ok = (rc == SQLITE_DONE || rc == SQLITE_ROW);
        int changes = sqlite3_changes(sqlite3_db_handle(stmt));
        
        dinero::g_logger.info("[SQL-EXEC] " + std::string(tag) + " ok=" + (ok ? "true" : "false") + " rowsChanged=" + std::to_string(changes));
        
        if (!ok) {
            dinero::g_logger.error("[SQL-EXEC-ERR] " + std::string(tag) + " " + std::string(sqlite3_errmsg(sqlite3_db_handle(stmt))));
        }
        
        return ok;
    }
    
    static long long lastRowId(sqlite3* db) {
        return sqlite3_last_insert_rowid(db);
    }
};

#ifdef DIN_SQLITE_TRACE
inline void attachSqliteTrace(sqlite3* db) {
    sqlite3_trace_v2(db, SQLITE_TRACE_STMT, [](unsigned, void*, void* pStmt, void*) -> int {
        auto* stmt = static_cast<sqlite3_stmt*>(pStmt);
        dinero::g_logger.info("[SQLITE-TRACE] " + std::string(sqlite3_sql(stmt)));
        return 0;
    }, nullptr);
}
#else
inline void attachSqliteTrace(sqlite3*) {}
#endif

} // namespace dinero
