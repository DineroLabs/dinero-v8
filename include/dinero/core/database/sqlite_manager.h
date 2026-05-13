#pragma once

#include <sqlite3.h>
#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace dinero {

// Bitcoin Core-compatible database manager
// NOTE: Only wallet uses SQLite. Blockchain data is in ChainDB (RocksDB).
class SQLiteManager {
public:
    explicit SQLiteManager(const std::string& datadir);
    ~SQLiteManager();

    // Initialize wallet database
    bool initialize();

    // Database access methods (wallet only)
    sqlite3* getWalletDB() const { return wallet_db_; }

    // REMOVED: getBlockchainDB() - use ChainDB (RocksDB) instead
    // REMOVED: getMempoolDB() - mempool is in-memory only
    // REMOVED: getPeersDB() - peers use binary format

private:
    // Database file paths
    std::string datadir_;
    std::string wallet_db_path_;

    // Database connections (wallet only)
    sqlite3* wallet_db_{nullptr};

    // Initialize wallet database
    bool initializeWalletDB();

    // Create wallet schema
    bool createWalletSchema();

    // Schema file loading (SQL-first architecture)
    std::string loadSchemaFile(const std::string& schema_name);
    bool applySchema(sqlite3* db, const std::string& sql, const std::string& component_name);
    int getSchemaVersion(sqlite3* db, const std::string& component);

    // Migration support
    bool runMigrations(sqlite3* db, const std::string& component, int target_version);
    std::vector<std::string> scanMigrationFiles(const std::string& component);
    int parseMigrationVersion(const std::string& filename);

    // Helper methods
    bool executeSQL(sqlite3* db, const std::string& sql);
    void closeDatabase(sqlite3*& db);
};

} // namespace dinero
