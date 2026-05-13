#pragma once

#include "contracts/multiasset_escrow_contract.h"
#include <sqlite3.h>
#include <string>
#include <optional>
#include <vector>
#include <mutex>

namespace dinero {
namespace contracts {

/**
 * MultiAssetPersistence - SQLite persistence layer for multi-asset contracts
 *
 * Provides durable storage for escrow contracts with:
 * - Automatic schema migration
 * - Transaction support
 * - Index optimization for asset queries
 * - JSON serialization for complex fields
 */
class MultiAssetPersistence {
public:
    /**
     * Initialize persistence with database file
     *
     * @param db_path Path to SQLite database file
     * @return true if successfully initialized
     */
    bool initialize(const std::string& db_path);

    /**
     * Close database connection
     */
    void close();

    /**
     * Save or update contract
     *
     * @param contract Contract to persist
     * @return true if successfully saved
     */
    bool saveContract(const AssetEscrowContract& contract);

    /**
     * Load contract by ID
     *
     * @param contract_id Unique contract identifier
     * @return Contract if found
     */
    std::optional<AssetEscrowContract> loadContract(const std::string& contract_id);

    /**
     * Load all contracts for a specific asset
     *
     * @param asset_id Asset identifier (e.g., "USDT")
     * @return Vector of contracts
     */
    std::vector<AssetEscrowContract> loadContractsByAsset(const std::string& asset_id);

    /**
     * Load all active contracts (pending or locked)
     *
     * @return Vector of active contracts
     */
    std::vector<AssetEscrowContract> loadActiveContracts();

    /**
     * Load all contracts
     *
     * @return Vector of all contracts
     */
    std::vector<AssetEscrowContract> loadAllContracts();

    /**
     * Update contract status
     *
     * @param contract_id Contract to update
     * @param new_status New status string
     * @return true if successfully updated
     */
    bool updateContractStatus(const std::string& contract_id, const std::string& new_status);

    /**
     * Delete contract from database
     *
     * @param contract_id Contract to delete
     * @return true if successfully deleted
     */
    bool deleteContract(const std::string& contract_id);

    /**
     * Get statistics by asset
     *
     * @return Map of asset_id -> count
     */
    std::map<std::string, size_t> getAssetStatistics();

    /**
     * Check if database is initialized
     */
    bool isInitialized() const { return db_ != nullptr; }

private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;

    // Initialize database schema
    bool createSchema();

    // Serialize/deserialize helpers
    std::string serializeKeys(const EscrowKeys& keys);
    EscrowKeys deserializeKeys(const std::string& json);
    std::string serializeRoute(const std::optional<bridge::ConversionRoute>& route);
    std::optional<bridge::ConversionRoute> deserializeRoute(const std::string& json);

    // Row to contract conversion
    AssetEscrowContract rowToContract(sqlite3_stmt* stmt);

    // Execute SQL with error handling
    bool executeSql(const std::string& sql);
};

} // namespace contracts
} // namespace dinero
