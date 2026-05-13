#pragma once

#include <string>
#include <sqlite3.h>

namespace dinero {

/**
 * Chainstate hardening utilities for database integrity and performance
 * 
 * Provides:
 * - Migration management for schema updates
 * - UTXO integrity validation
 * - Atomic transaction support
 * - Reindexing capabilities
 * - Performance monitoring
 */
class ChainstateHarden {
public:
    explicit ChainstateHarden(const std::string& db_path);
    ~ChainstateHarden();
    
    // Database initialization
    bool Initialize();
    
    // Migration management
    bool ApplyMigration002();
    bool IsMigrationApplied(const std::string& migration_key) const;
    
    // Integrity validation
    bool ValidateUTXOIntegrity() const;
    
    // Reindexing
    bool ReindexUTXOs();
    
    // Statistics
    uint64_t GetUTXOCount() const;
    uint64_t GetTotalSupply() const;
    
private:
    std::string db_path_;
    sqlite3* db_;
};

} // namespace dinero
