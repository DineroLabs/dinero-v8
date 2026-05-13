#pragma once
#include "wallet/wallet_iface.h"
#include <sqlite3.h>
#include <functional>

namespace din {

/**
 * @brief SQLite-based wallet database implementation
 * 
 * Provides persistent storage for wallet data using SQLite backend.
 * Handles keys, addresses, UTXOs, and transaction metadata.
 */
class WalletDBImpl : public IWalletDB {
public:
    explicit WalletDBImpl(const std::string& db_path);
    ~WalletDBImpl();
    
    // Database lifecycle
    bool initialize();
    void shutdown();
    
    // IWalletDB implementation
    bool put(const std::string& key, const std::string& value) override;
    std::optional<std::string> get(const std::string& key) const override;
    bool remove(const std::string& key) override;
    
    // Batch operations for atomicity
    bool beginBatch() override;
    bool commitBatch() override;
    bool abortBatch() override;
    
    // Iteration for backup/recovery
    bool iterate(const std::string& prefix, 
                std::function<bool(const std::string&, const std::string&)> callback) const override;

private:
    std::string db_path_;
    sqlite3* db_;
    bool in_batch_;
    
    // Helper methods
    bool createTables();
    bool prepareStatements();
    void finalizeStatements();
    
    // Prepared statements for performance
    sqlite3_stmt* stmt_put_;
    sqlite3_stmt* stmt_get_;
    sqlite3_stmt* stmt_remove_;
    sqlite3_stmt* stmt_iterate_;
};

} // namespace din
