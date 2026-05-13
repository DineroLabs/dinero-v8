#pragma once

#include <sqlite3.h>
#include <string>
#include <vector>

namespace dinero {
namespace db {

/**
 * Production-grade SQLite configuration helper
 * Applies WAL mode, safety pragmas, and performance optimizations
 */
class DatabasePragmas {
public:
    struct Config {
        // WAL configuration
        bool enable_wal;
        int wal_autocheckpoint;
        
        // Safety settings
        enum SyncMode { OFF = 0, NORMAL = 1, FULL = 2 };
        SyncMode synchronous;
        bool foreign_keys;
        
        // Performance settings
        int busy_timeout_ms;
        int cache_size_kb;  // 64MB default
        int page_size;
        int mmap_size_mb;     // 256MB mmap
        
        // Security settings
        bool secure_delete;
        bool recursive_triggers;
        
        // Maintenance settings
        bool auto_vacuum;
        int temp_store;  // MEMORY
        
        // Default constructor
        Config() : enable_wal(true), wal_autocheckpoint(1000), synchronous(NORMAL), 
                   foreign_keys(true), busy_timeout_ms(5000), cache_size_kb(64000),
                   page_size(4096), mmap_size_mb(256), secure_delete(true),
                   recursive_triggers(true), auto_vacuum(true), temp_store(2) {}
    };
    
    /**
     * Apply production pragmas to SQLite database
     * @param db SQLite database handle
     * @param config Configuration options
     * @return true on success, false on error
     */
    static bool Apply(sqlite3* db, const Config& config = Config());
    
    /**
     * Verify database integrity and WAL mode
     * @param db SQLite database handle
     * @return true if database is healthy
     */
    static bool VerifyIntegrity(sqlite3* db);
    
    /**
     * Perform WAL checkpoint and optimize
     * @param db SQLite database handle
     * @param force_full_checkpoint Force FULL checkpoint
     * @return true on success
     */
    static bool Checkpoint(sqlite3* db, bool force_full_checkpoint = false);
    
    /**
     * Get database statistics for monitoring
     */
    struct Stats {
        int64_t page_count = 0;
        int64_t page_size = 0;
        int64_t wal_size = 0;
        int64_t cache_hit_ratio = 0;
        std::string journal_mode;
        std::string synchronous_mode;
    };
    
    static Stats GetStats(sqlite3* db);
    
    /**
     * Emergency recovery: attempt to repair corrupted database
     * @param db_path Path to database file
     * @param backup_path Path to create backup before repair
     * @return true if recovery succeeded
     */
    static bool EmergencyRecovery(const std::string& db_path, 
                                  const std::string& backup_path);

private:
    static bool ExecutePragma(sqlite3* db, const std::string& pragma);
    static std::string GetPragmaValue(sqlite3* db, const std::string& pragma);
};

} // namespace db
} // namespace dinero
