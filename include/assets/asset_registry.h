/**
 * Phase 30: Taproot Asset Layer - Asset Registry
 *
 * Global registry for tracking all assets in the system:
 * - Asset genesis records (creation events)
 * - UTXO index for asset-bearing outputs
 * - Supply tracking per asset type
 * - SQLite persistence for durability
 */

#pragma once

#include "assets/asset_id.h"
#include "assets/asset_state.h"
#include "assets/asset_proof.h"
#include <vector>
#include <string>
#include <optional>
#include <functional>
#include <memory>
#include <mutex>

// Forward declarations
struct sqlite3;
struct sqlite3_stmt;

namespace dinero {
namespace assets {

// ============================================================================
// Registry Query Results
// ============================================================================

/**
 * @brief Summary of an asset type
 */
struct AssetSummary {
    AssetID asset_id;
    AssetMetadata metadata;
    AssetSupplyConfig supply_config;

    // Supply statistics
    uint64_t total_minted;              // Total ever minted
    uint64_t total_burned;              // Total ever burned
    uint64_t circulating_supply;        // Currently in circulation

    // UTXO statistics
    uint32_t utxo_count;                // Number of UTXOs
    uint32_t holder_count;              // Unique addresses holding

    // Creation info
    std::string creation_txid;
    uint32_t creation_height;
    uint64_t created_at;
};

/**
 * @brief Filter options for UTXO queries
 */
struct AssetUTXOFilter {
    std::optional<AssetID> asset_id;    // Filter by asset type
    std::optional<std::string> address; // Filter by owner address
    std::optional<uint64_t> min_amount; // Minimum amount
    std::optional<uint64_t> max_amount; // Maximum amount
    std::optional<uint32_t> min_height; // Minimum block height
    std::optional<uint32_t> max_height; // Maximum block height
    bool include_spent = false;         // Include spent UTXOs
    uint32_t limit = 1000;              // Max results
    uint32_t offset = 0;                // Pagination offset
};

/**
 * @brief Registry statistics
 */
struct RegistryStats {
    uint64_t total_assets;              // Number of registered assets
    uint64_t total_utxos;               // Total asset UTXOs
    uint64_t spent_utxos;               // Spent asset UTXOs
    uint64_t total_transfers;           // Total transfers processed
    uint64_t total_mints;               // Total mint operations
    uint64_t total_burns;               // Total burn operations
    uint32_t last_indexed_height;       // Last indexed block
};

// ============================================================================
// Asset Registry
// ============================================================================

/**
 * @brief Central registry for all assets
 */
class AssetRegistry {
public:
    /**
     * @brief Open or create registry database
     * @param db_path Path to SQLite database file
     */
    explicit AssetRegistry(const std::string& db_path);
    ~AssetRegistry();

    // Non-copyable
    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;

    // ========================================================================
    // Asset Registration
    // ========================================================================

    /**
     * @brief Register a new asset from genesis transaction
     * @param genesis Asset genesis information
     * @return true if registered, false if already exists
     */
    bool registerAsset(const AssetGenesis& genesis);

    /**
     * @brief Get asset genesis by ID
     * @param asset_id Asset to lookup
     * @return Genesis info or nullopt if not found
     */
    std::optional<AssetGenesis> getAssetGenesis(const AssetID& asset_id);

    /**
     * @brief Get asset summary with statistics
     * @param asset_id Asset to lookup
     * @return Summary or nullopt if not found
     */
    std::optional<AssetSummary> getAssetSummary(const AssetID& asset_id);

    /**
     * @brief List all registered assets
     * @param limit Maximum results
     * @param offset Pagination offset
     * @return List of asset summaries
     */
    std::vector<AssetSummary> listAssets(uint32_t limit = 100, uint32_t offset = 0);

    /**
     * @brief Search assets by name/ticker
     * @param query Search query
     * @return Matching assets
     */
    std::vector<AssetSummary> searchAssets(const std::string& query);

    // ========================================================================
    // UTXO Management
    // ========================================================================

    /**
     * @brief Add asset UTXO to index
     * @param utxo UTXO to add
     * @return true if added
     */
    bool addUTXO(const AssetUTXO& utxo);

    /**
     * @brief Mark UTXO as spent
     * @param txid Transaction containing output
     * @param vout Output index
     * @param spending_txid Transaction spending this output
     * @param spending_input Input index in spending tx
     * @return true if marked
     */
    bool markUTXOSpent(
        const std::string& txid,
        uint32_t vout,
        const std::string& spending_txid,
        uint32_t spending_input);

    /**
     * @brief Get UTXO by outpoint
     * @param txid Transaction ID
     * @param vout Output index
     * @return UTXO or nullopt if not found
     */
    std::optional<AssetUTXO> getUTXO(const std::string& txid, uint32_t vout);

    /**
     * @brief Query UTXOs with filters
     * @param filter Query filters
     * @return Matching UTXOs
     */
    std::vector<AssetUTXO> queryUTXOs(const AssetUTXOFilter& filter);

    /**
     * @brief Get all unspent UTXOs for an address
     * @param address Owner address
     * @param asset_id Optional: filter by asset type
     * @return Unspent UTXOs
     */
    std::vector<AssetUTXO> getAddressUTXOs(
        const std::string& address,
        const std::optional<AssetID>& asset_id = std::nullopt);

    /**
     * @brief Get balance for an address
     * @param address Owner address
     * @return Map of asset_id -> balance
     */
    std::vector<AssetBalance> getAddressBalances(const std::string& address);

    // ========================================================================
    // Supply Tracking
    // ========================================================================

    /**
     * @brief Record a mint operation
     * @param asset_id Asset minted
     * @param amount Amount minted
     * @param mint_txid Transaction containing mint
     * @param block_height Block height
     * @return true if recorded
     */
    bool recordMint(
        const AssetID& asset_id,
        uint64_t amount,
        const std::string& mint_txid,
        uint32_t block_height);

    /**
     * @brief Record a burn operation
     * @param asset_id Asset burned
     * @param amount Amount burned
     * @param burn_txid Transaction containing burn
     * @param block_height Block height
     * @return true if recorded
     */
    bool recordBurn(
        const AssetID& asset_id,
        uint64_t amount,
        const std::string& burn_txid,
        uint32_t block_height);

    /**
     * @brief Get current circulating supply
     * @param asset_id Asset to check
     * @return Circulating supply
     */
    uint64_t getCirculatingSupply(const AssetID& asset_id);

    /**
     * @brief Check if mint would exceed supply cap
     * @param asset_id Asset to mint
     * @param amount Amount to mint
     * @return true if mint is allowed
     */
    bool canMint(const AssetID& asset_id, uint64_t amount);

    // ========================================================================
    // Transfer Indexing
    // ========================================================================

    /**
     * @brief Index a transfer transaction
     * @param transition State transition
     * @param txid Transaction ID
     * @param block_height Block height (0 = mempool)
     * @return true if indexed
     */
    bool indexTransfer(
        const AssetStateTransition& transition,
        const std::string& txid,
        uint32_t block_height);

    /**
     * @brief Get transfer history for an address
     * @param address Address to lookup
     * @param asset_id Optional: filter by asset
     * @param limit Max results
     * @param offset Pagination offset
     * @return Transfer history
     */
    struct TransferRecord {
        std::string txid;
        AssetID asset_id;
        uint64_t amount;
        std::string from_address;
        std::string to_address;
        uint32_t block_height;
        uint64_t timestamp;
    };
    std::vector<TransferRecord> getTransferHistory(
        const std::string& address,
        const std::optional<AssetID>& asset_id = std::nullopt,
        uint32_t limit = 100,
        uint32_t offset = 0);

    // ========================================================================
    // Block Processing
    // ========================================================================

    /**
     * @brief Process a new block for asset operations
     * @param block_height Block height
     * @param block_hash Block hash
     * @param get_tx Callback to get transaction by txid
     * @return Number of operations indexed
     */
    uint32_t processBlock(
        uint32_t block_height,
        const std::string& block_hash,
        std::function<std::optional<std::vector<uint8_t>>(const std::string&)> get_tx);

    /**
     * @brief Revert a block (for reorg handling)
     * @param block_height Block height to revert
     * @return Number of operations reverted
     */
    uint32_t revertBlock(uint32_t block_height);

    /**
     * @brief Get last indexed block height
     */
    uint32_t getLastIndexedHeight() const;

    // ========================================================================
    // Validation
    // ========================================================================

    /**
     * @brief Validate asset transfer (for mempool/consensus)
     * @param transition State transition to validate
     * @return Validation result
     */
    bool validateTransfer(const AssetStateTransition& transition);

    /**
     * @brief Validate mint authorization
     * @param asset_id Asset being minted
     * @param amount Amount
     * @param authorization CSFS signature
     * @return true if authorized
     */
    bool validateMintAuth(
        const AssetID& asset_id,
        uint64_t amount,
        const std::vector<uint8_t>& authorization);

    /**
     * @brief Validate burn authorization
     * @param asset_id Asset being burned
     * @param amount Amount
     * @param authorization Optional CSFS signature
     * @return true if authorized
     */
    bool validateBurnAuth(
        const AssetID& asset_id,
        uint64_t amount,
        const std::vector<uint8_t>& authorization);

    // ========================================================================
    // Statistics
    // ========================================================================

    /**
     * @brief Get registry statistics
     */
    RegistryStats getStats() const;

    /**
     * @brief Get asset holders (addresses with balance)
     * @param asset_id Asset to check
     * @param limit Max results
     * @return Addresses sorted by balance descending
     */
    struct HolderInfo {
        std::string address;
        uint64_t balance;
        uint32_t utxo_count;
    };
    std::vector<HolderInfo> getTopHolders(const AssetID& asset_id, uint32_t limit = 100);

private:
    // Database handle
    sqlite3* db_ = nullptr;
    mutable std::mutex db_mutex_;

    // Prepared statements
    sqlite3_stmt* stmt_register_asset_ = nullptr;
    sqlite3_stmt* stmt_get_genesis_ = nullptr;
    sqlite3_stmt* stmt_add_utxo_ = nullptr;
    sqlite3_stmt* stmt_mark_spent_ = nullptr;
    sqlite3_stmt* stmt_get_utxo_ = nullptr;
    sqlite3_stmt* stmt_record_mint_ = nullptr;
    sqlite3_stmt* stmt_record_burn_ = nullptr;
    sqlite3_stmt* stmt_record_transfer_ = nullptr;

    // Initialize database schema
    void initSchema();

    // Prepare statements
    void prepareStatements();

    // Finalize statements
    void finalizeStatements();
};

// ============================================================================
// Asset Index (In-Memory Cache)
// ============================================================================

/**
 * @brief In-memory cache for fast asset lookups
 */
class AssetIndexCache {
public:
    explicit AssetIndexCache(size_t max_entries = 10000);

    // Cache asset metadata
    void cacheAsset(const AssetID& id, const AssetMetadata& metadata);
    std::optional<AssetMetadata> getCachedMetadata(const AssetID& id);

    // Cache UTXO existence
    void cacheUTXO(const std::string& outpoint, const AssetID& asset_id);
    std::optional<AssetID> getCachedUTXOAsset(const std::string& outpoint);
    void invalidateUTXO(const std::string& outpoint);

    // Cache supply
    void cacheSupply(const AssetID& id, uint64_t supply);
    std::optional<uint64_t> getCachedSupply(const AssetID& id);
    void invalidateSupply(const AssetID& id);

    // Clear cache
    void clear();

    // Stats
    size_t size() const;
    double hitRate() const;

private:
    struct CacheEntry {
        std::vector<uint8_t> data;
        uint64_t last_access;
    };

    std::map<std::string, CacheEntry> cache_;
    mutable std::mutex cache_mutex_;
    size_t max_entries_;

    // Stats
    mutable uint64_t hits_ = 0;
    mutable uint64_t misses_ = 0;

    void evictIfNeeded();
};

// ============================================================================
// Global Registry Accessor
// ============================================================================

/**
 * @brief Get the global asset registry instance
 */
AssetRegistry& GetAssetRegistry();

/**
 * @brief Initialize the global asset registry
 * @param db_path Database path
 */
void InitAssetRegistry(const std::string& db_path);

/**
 * @brief Shutdown the global asset registry
 */
void ShutdownAssetRegistry();

} // namespace assets
} // namespace dinero
