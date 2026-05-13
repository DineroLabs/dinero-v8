#pragma once

#include <sqlite3.h>
#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace dinero {

// Bitcoin Core-compatible database manager
// NOTE: Only manages wallet.db (SQLite) - consensus data uses RocksDB
class SQLiteManager {
public:
    explicit SQLiteManager(const std::string& datadir);
    ~SQLiteManager();

    // Initialize wallet database
    bool initialize();

    // Database access methods
    sqlite3* getWalletDB() const { return wallet_db_; }

    // REMOVED: getBlockchainDB() - use ChainDB (RocksDB) instead
    // REMOVED: getMempoolDB() - use RocksDB mempool instead
    // REMOVED: getPeersDB() - use binary peer storage instead

private:
    // Database file paths
    std::string datadir_;
    std::string wallet_db_path_;
    // REMOVED: blockchain_db_path_ (explorer.db)
    // REMOVED: mempool_db_path_ (mempool.db)
    // REMOVED: peers_db_path_ (peers.db)

    // Database connections
    sqlite3* wallet_db_{nullptr};
    // REMOVED: blockchain_db_ (explorer.db)
    // REMOVED: mempool_db_ (mempool.db)
    // REMOVED: peers_db_ (peers.db)

    // Initialize wallet database
    bool initializeWalletDB();
    // REMOVED: initializeBlockchainDB()
    // REMOVED: initializeMempoolDB()
    // REMOVED: initializePeersDB()

    // Create wallet schema
    bool createWalletSchema();
    // REMOVED: createBlockchainSchema()
    // REMOVED: createMempoolSchema()
    // REMOVED: createPeersSchema()

    // Helper methods
    bool executeSQL(sqlite3* db, const std::string& sql);
    void closeDatabase(sqlite3*& db);
    
    // SQL-first schema loading (Phase 2 - Nov 2025)
    std::string loadSchemaFile(const std::string& schema_name);
    int getSchemaVersion(sqlite3* db, const std::string& component);
    bool applySchema(sqlite3* db, const std::string& sql, const std::string& component_name);
    
    // Schema migration runner (Phase 3 - Nov 2025)
    bool runMigrations(sqlite3* db, const std::string& component, int target_version);
    std::vector<std::string> scanMigrationFiles(const std::string& component);
    int parseMigrationVersion(const std::string& filename);
};

} // namespace dinero
