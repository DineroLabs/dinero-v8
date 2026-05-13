#include "db/DatabasePragmas.h"
#include <sstream>
#include <iostream>
#include <filesystem>
#include <fstream>

namespace dinero {
namespace db {

// Forward declaration
static std::string GetPragmaValue(sqlite3* db, const std::string& pragma);

bool DatabasePragmas::Apply(sqlite3* db, const Config& config) {
    if (!db) return false;
    
    std::vector<std::string> pragmas;
    
    // WAL mode configuration
    if (config.enable_wal) {
        pragmas.emplace_back("PRAGMA journal_mode=WAL");
        pragmas.emplace_back("PRAGMA wal_autocheckpoint=" + std::to_string(config.wal_autocheckpoint));
    }
    
    // Safety settings
    std::string sync_mode;
    switch (config.synchronous) {
        case Config::OFF: sync_mode = "OFF"; break;
        case Config::NORMAL: sync_mode = "NORMAL"; break;
        case Config::FULL: sync_mode = "FULL"; break;
    }
    pragmas.emplace_back("PRAGMA synchronous=" + sync_mode);
    
    if (config.foreign_keys) {
        pragmas.emplace_back("PRAGMA foreign_keys=ON");
    }
    
    // Performance settings
    pragmas.emplace_back("PRAGMA busy_timeout=" + std::to_string(config.busy_timeout_ms));
    pragmas.emplace_back("PRAGMA cache_size=" + std::to_string(-(config.cache_size_kb)));  // Negative = KB
    pragmas.emplace_back("PRAGMA page_size=" + std::to_string(config.page_size));
    pragmas.emplace_back("PRAGMA mmap_size=" + std::to_string(config.mmap_size_mb * 1024 * 1024));
    
    // Security settings
    if (config.secure_delete) {
        pragmas.emplace_back("PRAGMA secure_delete=ON");
    }
    
    if (config.recursive_triggers) {
        pragmas.emplace_back("PRAGMA recursive_triggers=ON");
    }
    
    // Maintenance settings
    if (config.auto_vacuum) {
        pragmas.emplace_back("PRAGMA auto_vacuum=INCREMENTAL");
    }
    
    pragmas.emplace_back("PRAGMA temp_store=" + std::to_string(config.temp_store));
    
    // Additional production optimizations
    pragmas.emplace_back("PRAGMA optimize");
    pragmas.emplace_back("PRAGMA analysis_limit=1000");
    
    // Execute all pragmas
    for (const auto& pragma : pragmas) {
        if (!ExecutePragma(db, pragma)) {
            std::cerr << "Failed to execute pragma: " << pragma << std::endl;
            return false;
        }
    }
    
    // Verify WAL mode was enabled
    if (config.enable_wal) {
        std::string journal_mode = GetPragmaValue(db, "journal_mode");
        if (journal_mode != "wal") {
            std::cerr << "Failed to enable WAL mode, got: " << journal_mode << std::endl;
            return false;
        }
    }
    
    return true;
}

bool DatabasePragmas::VerifyIntegrity(sqlite3* db) {
    if (!db) return false;
    
    // Quick integrity check
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    bool is_ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        is_ok = (result && std::string(result) == "ok");
    }
    
    sqlite3_finalize(stmt);
    
    if (!is_ok) {
        std::cerr << "Database integrity check failed" << std::endl;
        return false;
    }
    
    // Verify WAL mode
    std::string journal_mode = GetPragmaValue(db, "journal_mode");
    if (journal_mode != "wal") {
        std::cerr << "Database not in WAL mode: " << journal_mode << std::endl;
        return false;
    }
    
    return true;
}

bool DatabasePragmas::Checkpoint(sqlite3* db, bool force_full_checkpoint) {
    if (!db) return false;
    
    int mode = force_full_checkpoint ? SQLITE_CHECKPOINT_FULL : SQLITE_CHECKPOINT_PASSIVE;
    
    int log_size = 0, checkpointed = 0;
    int rc = sqlite3_wal_checkpoint_v2(db, nullptr, mode, &log_size, &checkpointed);
    
    if (rc != SQLITE_OK) {
        std::cerr << "WAL checkpoint failed: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    
    // Log checkpoint stats
    std::cout << "WAL checkpoint: " << checkpointed << "/" << log_size << " pages" << std::endl;
    
    return true;
}

DatabasePragmas::Stats DatabasePragmas::GetStats(sqlite3* db) {
    Stats stats;
    if (!db) return stats;
    
    // Get basic database info
    stats.page_count = std::stoll(GetPragmaValue(db, "page_count"));
    stats.page_size = std::stoll(GetPragmaValue(db, "page_size"));
    stats.journal_mode = GetPragmaValue(db, "journal_mode");
    stats.synchronous_mode = GetPragmaValue(db, "synchronous");
    
    // WAL size
    std::string wal_size_str = GetPragmaValue(db, "wal_checkpoint");
    if (!wal_size_str.empty()) {
        // Parse "busy=0 log=123 checkpointed=456"
        size_t log_pos = wal_size_str.find("log=");
        if (log_pos != std::string::npos) {
            stats.wal_size = std::stoll(wal_size_str.substr(log_pos + 4));
        }
    }
    
    // Cache hit ratio (approximation)
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, "PRAGMA cache_spill", -1, &stmt, nullptr);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        // This is a rough approximation - in production you'd want more detailed metrics
        stats.cache_hit_ratio = 95; // Placeholder
    }
    sqlite3_finalize(stmt);
    
    return stats;
}

bool DatabasePragmas::EmergencyRecovery(const std::string& db_path, 
                                        const std::string& backup_path) {
    try {
        // Create backup of corrupted database
        std::filesystem::copy_file(db_path, backup_path, 
                                   std::filesystem::copy_options::overwrite_existing);
        
        // Try to open and dump/restore
        sqlite3* db = nullptr;
        int rc = sqlite3_open(db_path.c_str(), &db);
        if (rc != SQLITE_OK) {
            std::cerr << "Cannot open corrupted database: " << sqlite3_errmsg(db) << std::endl;
            sqlite3_close(db);
            return false;
        }
        
        // Try integrity check first
        if (VerifyIntegrity(db)) {
            sqlite3_close(db);
            return true; // Database is actually fine
        }
        
        // Attempt recovery using .recover command (SQLite 3.37+)
        sqlite3_close(db);
        
        std::string recovery_path = db_path + ".recovered";
        std::string cmd = "sqlite3 " + db_path + " \".recover\" | sqlite3 " + recovery_path;
        
        int result = std::system(cmd.c_str());
        if (result == 0 && std::filesystem::exists(recovery_path)) {
            // Replace original with recovered
            std::filesystem::rename(recovery_path, db_path);
            
            // Verify recovered database
            rc = sqlite3_open(db_path.c_str(), &db);
            if (rc == SQLITE_OK) {
                bool is_ok = VerifyIntegrity(db);
                sqlite3_close(db);
                
                if (is_ok) {
                    std::cout << "Database recovery successful" << std::endl;
                    return true;
                }
            }
        }
        
        std::cerr << "Database recovery failed" << std::endl;
        return false;
        
    } catch (const std::exception& e) {
        std::cerr << "Recovery exception: " << e.what() << std::endl;
        return false;
    }
}

bool DatabasePragmas::ExecutePragma(sqlite3* db, const std::string& pragma) {
    char* error_msg = nullptr;
    int rc = sqlite3_exec(db, pragma.c_str(), nullptr, nullptr, &error_msg);
    
    if (rc != SQLITE_OK) {
        std::cerr << "Pragma failed: " << pragma << " - " << 
                     (error_msg ? error_msg : "Unknown error") << std::endl;
        if (error_msg) sqlite3_free(error_msg);
        return false;
    }
    
    return true;
}

static std::string GetPragmaValue(sqlite3* db, const std::string& pragma) {
    std::string query = "PRAGMA " + pragma;
    sqlite3_stmt* stmt = nullptr;
    
    int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return "";
    }
    
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (value) result = value;
    }
    
    sqlite3_finalize(stmt);
    return result;
}

} // namespace db
} // namespace dinero
