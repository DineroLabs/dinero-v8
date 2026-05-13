#pragma once

#include "contracts/escrow_contract.h"
#include "bridge/routing_engine.h"
#include "bridge/fiat_bridge_manager.h"
#include <optional>
#include <memory>

namespace dinero {
namespace contracts {

// Forward declaration
class MultiAssetPersistence;

/**
 * AssetEscrowContract - Extends EscrowContract to support multiple assets
 *
 * The Bitcoin Script itself doesn't care about asset type,
 * but the manager needs to track which asset is locked.
 */
struct AssetEscrowContract : public EscrowContract {
    // Asset information
    std::string asset_id;           // "DIN", "BTC", "USDT", "EUR", etc.
    uint8_t decimals;               // Asset decimal places (8 for most crypto)

    // For conversions on release
    std::string release_asset;      // If releasing to different asset, track here
    std::optional<bridge::ConversionRoute> swap_route;  // For automatic conversion

    // Additional metadata
    std::string asset_address;      // Address where asset is held (if wrapped)
    bool is_wrapped;                // If asset is wrapped version (e.g., USDT on Ethereum)

    // Constructor
    AssetEscrowContract() : is_wrapped(false), decimals(8) {}
};

/**
 * MultiAssetEscrowBuilder - Extends EscrowContractBuilder for multiple assets
 */
class MultiAssetEscrowBuilder {
public:
    /**
     * Build escrow contract for any supported asset
     *
     * @param keys Buyer, seller, mediator public keys
     * @param asset_id Asset identifier ("DIN", "USDT", "EUR", etc.)
     * @param amount Amount in asset_id units
     * @param refund_blocks Number of blocks until refund
     * @return AssetEscrowContract with asset metadata
     */
    static AssetEscrowContract buildMultiAssetContract(
        const EscrowKeys& keys,
        const std::string& asset_id,
        double amount,
        uint32_t refund_blocks
    );

    /**
     * Build with automatic conversion on release
     *
     * Example: Seller wants to receive EUR, but escrow will hold USDT
     * This method finds the best conversion route automatically
     *
     * @param keys Buyer, seller, mediator keys
     * @param escrow_asset Asset to lock in escrow ("USDT")
     * @param release_asset Target asset for seller ("EUR")
     * @param amount Amount in escrow_asset units
     * @param refund_blocks Refund timelock
     * @param bridge_manager To query available conversion routes
     * @return Contract with conversion route populated
     */
    static std::optional<AssetEscrowContract> buildWithAutoConversion(
        const EscrowKeys& keys,
        const std::string& escrow_asset,
        const std::string& release_asset,
        double amount,
        uint32_t refund_blocks,
        bridge::FiatBridgeManager& bridge_manager
    );

    /**
     * Validate that an asset is supported
     *
     * @param asset_id Asset to validate
     * @return true if asset can be held in escrow
     */
    static bool isAssetSupported(const std::string& asset_id);

    /**
     * Get decimal places for an asset
     *
     * @param asset_id Asset identifier
     * @return Number of decimal places (8 by default)
     */
    static uint8_t getAssetDecimals(const std::string& asset_id);

    /**
     * Convert amount from one asset denomination to another
     * Uses the routing engine to find exchange rates
     *
     * @param from_asset Source asset
     * @param to_asset Target asset
     * @param amount Amount in from_asset units
     * @param bridge_manager For rate lookup
     * @return Converted amount in to_asset units (nullopt if conversion not available)
     */
    static std::optional<double> convertAmount(
        const std::string& from_asset,
        const std::string& to_asset,
        double amount,
        bridge::FiatBridgeManager& bridge_manager
    );

private:
    // Map of supported assets to decimal places
    static const std::map<std::string, uint8_t> ASSET_DECIMALS;
};

/**
 * MultiAssetContractRegistry - Thread-safe registry for multi-asset contracts
 *
 * Extends the existing registry pattern to index by asset type
 */
class MultiAssetContractRegistry {
public:
    // Singleton access
    static MultiAssetContractRegistry& getInstance();

    /**
     * Initialize persistence layer
     *
     * @param db_path Path to SQLite database file
     * @return true if successfully initialized
     */
    bool initialize(const std::string& db_path);

    /**
     * Register a new contract
     *
     * @param contract Contract to register
     * @return true if successfully registered
     */
    bool registerContract(const AssetEscrowContract& contract);

    /**
     * Get contract by ID
     *
     * @param contract_id Unique contract identifier
     * @return Contract if found
     */
    std::optional<AssetEscrowContract> getContract(const std::string& contract_id) const;

    /**
     * Get all contracts for a specific asset
     *
     * @param asset_id Asset identifier ("DIN", "USDT", etc.)
     * @return Vector of all contracts using that asset
     */
    std::vector<AssetEscrowContract> getContractsByAsset(const std::string& asset_id) const;

    /**
     * Get all active contracts (pending or locked)
     *
     * @return Vector of active contracts
     */
    std::vector<AssetEscrowContract> getActiveContracts() const;

    /**
     * Update contract status
     *
     * @param contract_id Contract to update
     * @param new_status New status ("released", "refunded", etc.)
     * @return true if successfully updated
     */
    bool updateContractStatus(const std::string& contract_id, const std::string& new_status);

    /**
     * Remove contract from registry
     *
     * @param contract_id Contract to remove
     * @return true if successfully removed
     */
    bool removeContract(const std::string& contract_id);

    /**
     * Get statistics for asset usage
     *
     * @return Map of asset_id -> number of contracts
     */
    std::map<std::string, size_t> getAssetStatistics() const;

private:
    MultiAssetContractRegistry() = default;
    ~MultiAssetContractRegistry() = default;
    MultiAssetContractRegistry(const MultiAssetContractRegistry&) = delete;
    MultiAssetContractRegistry& operator=(const MultiAssetContractRegistry&) = delete;

    // Thread-safe storage
    mutable std::mutex mutex_;
    std::map<std::string, AssetEscrowContract> contracts_;          // contract_id -> contract (in-memory cache)
    std::multimap<std::string, std::string> asset_index_;           // asset_id -> contract_id (in-memory index)

    // Persistent storage
    std::unique_ptr<MultiAssetPersistence> persistence_;
};

/**
 * BridgedEscrowManager - Manages escrow contracts with automatic conversions
 *
 * Coordinates between:
 * - MultiAssetContractRegistry (contract storage)
 * - FiatBridgeManager (routing/conversion)
 * - EscrowContractBuilder (script generation)
 */
class BridgedEscrowManager {
public:
    /**
     * Constructor
     *
     * @param bridge_manager Reference to the bridge manager for conversions
     */
    explicit BridgedEscrowManager(bridge::FiatBridgeManager& bridge_manager);

    /**
     * Create escrow with optional automatic conversion
     *
     * @param keys Buyer, seller, mediator keys
     * @param escrow_asset Asset to lock
     * @param amount Amount to lock
     * @param refund_blocks Blocks until refund
     * @param release_asset Optional: asset to release (triggers conversion)
     * @return Created contract
     */
    std::optional<AssetEscrowContract> createEscrow(
        const EscrowKeys& keys,
        const std::string& escrow_asset,
        double amount,
        uint32_t refund_blocks,
        const std::optional<std::string>& release_asset = std::nullopt
    );

    /**
     * Release escrow with automatic conversion if needed
     *
     * @param contract_id Contract to release
     * @param to_address Destination address
     * @param sig_buyer Buyer's signature
     * @param sig_seller Seller's signature
     * @return Transaction hex if successful
     */
    std::optional<std::string> releaseEscrow(
        const std::string& contract_id,
        const std::string& to_address,
        const std::string& sig_buyer,
        const std::string& sig_seller
    );

    /**
     * Refund escrow (no conversion, original asset returned)
     *
     * @param contract_id Contract to refund
     * @param refund_address Buyer's refund address
     * @param sig_buyer Buyer's signature
     * @return Transaction hex if successful
     */
    std::optional<std::string> refundEscrow(
        const std::string& contract_id,
        const std::string& refund_address,
        const std::string& sig_buyer
    );

    /**
     * Get available conversion routes for an escrow
     *
     * @param escrow_asset Current escrow asset
     * @param target_asset Desired target asset
     * @param amount Amount to convert
     * @return Vector of possible routes, sorted by best rate
     */
    std::vector<bridge::ConversionRoute> getConversionRoutes(
        const std::string& escrow_asset,
        const std::string& target_asset,
        double amount
    ) const;

    /**
     * Estimate conversion output
     *
     * @param escrow_asset Source asset
     * @param target_asset Target asset
     * @param amount Input amount
     * @return Expected output amount (after fees/slippage)
     */
    std::optional<double> estimateConversion(
        const std::string& escrow_asset,
        const std::string& target_asset,
        double amount
    ) const;

private:
    bridge::FiatBridgeManager& bridge_manager_;
    MultiAssetContractRegistry& registry_;

    // Helper: Execute conversion route
    bool executeConversion(
        const AssetEscrowContract& contract,
        const std::string& to_address
    );
};

} // namespace contracts
} // namespace dinero
