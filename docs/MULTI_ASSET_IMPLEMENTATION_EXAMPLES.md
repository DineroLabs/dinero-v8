# Multi-Asset Escrow - Implementation Code Examples

## Overview

This document provides practical code examples for implementing multi-asset escrow based on the existing DineroCoin architecture.

---

## Phase 1: Asset-Aware Contract Structure

### New Header File: `include/contracts/multiasset_escrow_contract.h`

```cpp
#pragma once

#include "contracts/escrow_contract.h"
#include "bridge/routing_engine.h"
#include <optional>

namespace dinero {
namespace contracts {

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
     * @param release_asset Target asset for buyer ("EUR")
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
        const bridge::FiatBridgeManager& bridge_manager
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
     * @return Number of decimal places (8 for most crypto, 2 for fiat)
     */
    static uint8_t getAssetDecimals(const std::string& asset_id);

private:
    // Helper: Convert amount from user units to smallest units (una)
    static int64_t assetToSmallestUnit(double amount, uint8_t decimals);
    
    // Helper: Convert amount from smallest units to user units
    static double smallestUnitToAsset(int64_t amount, uint8_t decimals);
};

} // namespace contracts
} // namespace dinero
```

### Implementation: `src/contracts/multiasset_escrow_contract.cpp`

```cpp
#include "contracts/multiasset_escrow_contract.h"
#include "bridge/fiat_bridge_manager.h"
#include "common/logger.h"
#include <map>

namespace dinero {
namespace contracts {

// Asset configuration: asset_id -> decimals
static const std::map<std::string, uint8_t> ASSET_DECIMALS = {
    // Cryptocurrencies
    {"DIN", 8},        // Dinero
    {"BTC", 8},        // Bitcoin
    {"ETH", 18},       // Ethereum
    {"USDT", 6},       // Tether (on most chains)
    {"USDC", 6},       // USDC
    {"DAI", 18},       // Dai
    {"BUSD", 18},      // Binance USD
    
    // Fiat currencies (if stored as synthetic assets)
    {"USD", 2},        // US Dollar
    {"EUR", 2},        // Euro
    {"GBP", 2},        // British Pound
    {"JPY", 0},        // Japanese Yen
};

AssetEscrowContract MultiAssetEscrowBuilder::buildMultiAssetContract(
    const EscrowKeys& keys,
    const std::string& asset_id,
    double amount,
    uint32_t refund_blocks
) {
    // 1. Validate asset
    if (!isAssetSupported(asset_id)) {
        dinero::g_logger.error("[MultiAssetEscrow] Unsupported asset: " + asset_id);
        throw std::runtime_error("Unsupported asset: " + asset_id);
    }

    // 2. Validate amount
    if (amount <= 0) {
        throw std::runtime_error("Amount must be positive");
    }

    // 3. Build base contract using parent builder
    // Note: EscrowContractBuilder doesn't know about assets,
    // so we use it as-is and extend with asset metadata
    EscrowContract base_contract = EscrowContractBuilder::buildContract(
        keys, amount, refund_blocks
    );

    // 4. Create extended contract with asset info
    AssetEscrowContract contract;
    
    // Copy all base fields
    contract.contract_id = base_contract.contract_id;
    contract.keys = base_contract.keys;
    contract.amount = base_contract.amount;
    contract.refund_time = base_contract.refund_time;
    contract.redeem_script = base_contract.redeem_script;
    contract.script_hash = base_contract.script_hash;
    contract.p2sh_address = base_contract.p2sh_address;
    contract.lock_txid = base_contract.lock_txid;
    contract.lock_vout = base_contract.lock_vout;
    contract.created_at = base_contract.created_at;
    contract.status = base_contract.status;
    contract.confirmations = base_contract.confirmations;
    
    // Add asset-specific fields
    contract.asset_id = asset_id;
    contract.decimals = getAssetDecimals(asset_id);
    contract.release_asset = asset_id;  // Default: release same asset
    contract.is_wrapped = false;
    contract.asset_address = "";

    dinero::g_logger.info("[MultiAssetEscrow] Built contract for asset: " + asset_id +
                         " | Amount: " + std::to_string(amount) +
                         " | Decimals: " + std::to_string(contract.decimals));

    return contract;
}

std::optional<AssetEscrowContract> MultiAssetEscrowBuilder::buildWithAutoConversion(
    const EscrowKeys& keys,
    const std::string& escrow_asset,
    const std::string& release_asset,
    double amount,
    uint32_t refund_blocks,
    const bridge::FiatBridgeManager& bridge_manager
) {
    // 1. Build base contract for escrow asset
    AssetEscrowContract contract = buildMultiAssetContract(
        keys, escrow_asset, amount, refund_blocks
    );

    // 2. If releasing to same asset, done
    if (escrow_asset == release_asset) {
        return contract;
    }

    // 3. Find best conversion route
    auto route = bridge_manager.get_all_routes(escrow_asset, release_asset, 3);
    if (route.empty()) {
        dinero::g_logger.error("[MultiAssetEscrow] No conversion route found: " +
                              escrow_asset + " -> " + release_asset);
        return std::nullopt;
    }

    // 4. Pick best route (first is best by default)
    contract.swap_route = route[0];
    contract.release_asset = release_asset;

    dinero::g_logger.info("[MultiAssetEscrow] Escrow: " + escrow_asset +
                         " | Release: " + release_asset +
                         " | Route: " + route[0].description());

    return contract;
}

bool MultiAssetEscrowBuilder::isAssetSupported(const std::string& asset_id) {
    return ASSET_DECIMALS.find(asset_id) != ASSET_DECIMALS.end();
}

uint8_t MultiAssetEscrowBuilder::getAssetDecimals(const std::string& asset_id) {
    auto it = ASSET_DECIMALS.find(asset_id);
    if (it == ASSET_DECIMALS.end()) {
        return 8;  // Default fallback
    }
    return it->second;
}

int64_t MultiAssetEscrowBuilder::assetToSmallestUnit(double amount, uint8_t decimals) {
    // Convert 1.5 USDT (decimals=6) to 1500000 microUSDT
    double multiplier = 1.0;
    for (uint8_t i = 0; i < decimals; ++i) {
        multiplier *= 10.0;
    }
    return static_cast<int64_t>(amount * multiplier);
}

double MultiAssetEscrowBuilder::smallestUnitToAsset(int64_t amount, uint8_t decimals) {
    double divisor = 1.0;
    for (uint8_t i = 0; i < decimals; ++i) {
        divisor *= 10.0;
    }
    return static_cast<double>(amount) / divisor;
}

} // namespace contracts
} // namespace dinero
```

---

## Phase 2: Extended Registry

### New Header: `include/contracts/multiasset_contract_registry.h`

```cpp
#pragma once

#include "contracts/contract_registry.h"
#include "contracts/multiasset_escrow_contract.h"
#include <map>
#include <vector>

namespace dinero {
namespace contracts {

/**
 * MultiAssetContractRegistry - Extends ContractRegistry for multi-asset support
 */
class MultiAssetContractRegistry : public ContractRegistry {
public:
    static MultiAssetContractRegistry& instance();

    /**
     * Store a multi-asset contract
     * Indexes by both contract_id AND asset_id
     */
    bool storeContract(const AssetEscrowContract& contract);

    /**
     * List all contracts for a specific asset
     * 
     * @param asset_id Asset identifier ("USDT", "EUR", etc.)
     * @return Vector of contracts holding this asset
     */
    std::vector<AssetEscrowContract> listByAsset(const std::string& asset_id);

    /**
     * Get total value locked across all contracts for an asset
     * 
     * @param asset_id Asset identifier
     * @return Total locked amount (sum of all contract amounts)
     */
    double getTotalLockedByAsset(const std::string& asset_id);

    /**
     * Get summary of all assets held in escrow
     * 
     * @return Map of asset_id -> total_locked
     */
    std::map<std::string, double> getAssetSummary();

    /**
     * List all supported assets currently held in escrow
     */
    std::vector<std::string> listActiveAssets();

private:
    MultiAssetContractRegistry() = default;
    ~MultiAssetContractRegistry() = default;

    // Index by asset: asset_id -> [contract_id -> contract]
    std::map<std::string, std::map<std::string, AssetEscrowContract>> contracts_by_asset_;
    
    mutable std::mutex asset_mutex_;
};

} // namespace contracts
} // namespace dinero
```

### Implementation: `src/contracts/multiasset_contract_registry.cpp`

```cpp
#include "contracts/multiasset_contract_registry.h"
#include "common/logger.h"

namespace dinero {
namespace contracts {

MultiAssetContractRegistry& MultiAssetContractRegistry::instance() {
    static MultiAssetContractRegistry instance;
    return instance;
}

bool MultiAssetContractRegistry::storeContract(const AssetEscrowContract& contract) {
    std::lock_guard<std::mutex> lock(asset_mutex_);

    // Store in asset index
    contracts_by_asset_[contract.asset_id][contract.contract_id] = contract;

    dinero::g_logger.info("[MultiAssetRegistry] Stored contract: " + contract.contract_id +
                         " | Asset: " + contract.asset_id +
                         " | Amount: " + std::to_string(contract.amount));

    return true;
}

std::vector<AssetEscrowContract> MultiAssetContractRegistry::listByAsset(
    const std::string& asset_id
) {
    std::lock_guard<std::mutex> lock(asset_mutex_);

    std::vector<AssetEscrowContract> result;

    auto it = contracts_by_asset_.find(asset_id);
    if (it != contracts_by_asset_.end()) {
        for (const auto& [id, contract] : it->second) {
            // Only return active contracts
            if (contract.status == "locked" || contract.status == "pending") {
                result.push_back(contract);
            }
        }
    }

    return result;
}

double MultiAssetContractRegistry::getTotalLockedByAsset(const std::string& asset_id) {
    std::lock_guard<std::mutex> lock(asset_mutex_);

    double total = 0.0;

    auto it = contracts_by_asset_.find(asset_id);
    if (it != contracts_by_asset_.end()) {
        for (const auto& [id, contract] : it->second) {
            if (contract.status == "locked") {
                total += contract.amount;
            }
        }
    }

    return total;
}

std::map<std::string, double> MultiAssetContractRegistry::getAssetSummary() {
    std::lock_guard<std::mutex> lock(asset_mutex_);

    std::map<std::string, double> summary;

    for (const auto& [asset_id, contracts] : contracts_by_asset_) {
        double total = 0.0;
        for (const auto& [id, contract] : contracts) {
            if (contract.status == "locked") {
                total += contract.amount;
            }
        }
        if (total > 0) {
            summary[asset_id] = total;
        }
    }

    return summary;
}

std::vector<std::string> MultiAssetContractRegistry::listActiveAssets() {
    std::lock_guard<std::mutex> lock(asset_mutex_);

    std::set<std::string> assets;

    for (const auto& [asset_id, contracts] : contracts_by_asset_) {
        for (const auto& [id, contract] : contracts) {
            if (contract.status == "locked") {
                assets.insert(asset_id);
                break;
            }
        }
    }

    return std::vector<std::string>(assets.begin(), assets.end());
}

} // namespace contracts
} // namespace dinero
```

---

## Phase 3: Bridge Integration

### New Header: `include/p2p/multiasset_escrow_manager.h`

```cpp
#pragma once

#include "p2p/escrow_manager.h"
#include "contracts/multiasset_escrow_contract.h"
#include "bridge/fiat_bridge_manager.h"
#include <optional>

namespace dinero {
namespace p2p {

/**
 * BridgedEscrowManager - Extends EscrowManager with bridge/routing support
 * 
 * Enables:
 * - Creating escrow in any asset
 * - Releasing with automatic conversion to target asset
 * - Refunding with conversion back to original asset
 */
class BridgedEscrowManager : public EscrowManager {
public:
    static BridgedEscrowManager& instance();

    /**
     * Create multi-asset escrow
     * 
     * @param seller_address Seller's address
     * @param asset_id Asset to hold in escrow ("USDT", "EUR", etc.)
     * @param amount Amount in asset units
     * @param duration_seconds How long to lock
     * @param offer_id Associated P2P offer
     * @return EscrowInfo with multi-asset metadata, or nullopt on error
     */
    std::optional<contracts::AssetEscrowContract> createMultiAssetEscrow(
        const std::string& seller_address,
        const std::string& asset_id,
        double amount,
        uint64_t duration_seconds,
        const std::string& offer_id
    );

    /**
     * Release escrow with automatic conversion
     * 
     * If target_asset differs from escrow asset, automatically converts
     * using best available route.
     *
     * Example:
     * - Escrow holds: USDT
     * - Release to: EUR
     * - System finds: USDT → BTC → EUR (via dex + coinbase)
     * - Buyer receives: ~80.50 EUR
     *
     * @param escrow_id Contract identifier
     * @param buyer_address Where to send funds
     * @param target_asset Target asset for buyer (e.g., "EUR")
     * @param sig_buyer Buyer's signature for 2-of-3 multisig
     * @param sig_seller Seller's signature for 2-of-3 multisig
     * @return ConversionResult with transaction details, or nullopt on error
     */
    std::optional<bridge::ConversionResult> releaseWithConversion(
        const std::string& escrow_id,
        const std::string& buyer_address,
        const std::string& target_asset,
        const std::string& sig_buyer,
        const std::string& sig_seller
    );

    /**
     * Preview conversion before executing
     * 
     * Show user what they'll receive if they convert
     *
     * @param from_asset Current escrow asset
     * @param to_asset Target asset
     * @param amount Amount to convert
     * @return ConversionRoute with rate/fee/slippage info
     */
    std::optional<bridge::ConversionRoute> estimateConversion(
        const std::string& from_asset,
        const std::string& to_asset,
        double amount
    );

    /**
     * Check supported assets for escrow
     */
    std::vector<std::string> listSupportedAssets() const;

private:
    BridgedEscrowManager() = default;

    // Reference to bridge manager (note: bridge is separate singleton)
    bridge::FiatBridgeManager& bridge_manager_;
};

} // namespace p2p
} // namespace dinero
```

### Implementation: `src/p2p/multiasset_escrow_manager.cpp`

```cpp
#include "p2p/multiasset_escrow_manager.h"
#include "contracts/multiasset_contract_registry.h"
#include "bridge/routing_engine.h"
#include "common/logger.h"

namespace dinero {
namespace p2p {

BridgedEscrowManager& BridgedEscrowManager::instance() {
    static BridgedEscrowManager instance;
    return instance;
}

std::optional<contracts::AssetEscrowContract> BridgedEscrowManager::createMultiAssetEscrow(
    const std::string& seller_address,
    const std::string& asset_id,
    double amount,
    uint64_t duration_seconds,
    const std::string& offer_id
) {
    // 1. Create multi-asset contract
    contracts::EscrowKeys keys;
    keys.seller_pubkey = seller_address;  // Simplified for example
    keys.buyer_pubkey = "";               // Will be set when offer accepted
    keys.mediator_pubkey = "MEDIATOR_PK"; // Default mediator

    contracts::AssetEscrowContract contract;
    try {
        contract = contracts::MultiAssetEscrowBuilder::buildMultiAssetContract(
            keys, asset_id, amount, 2880  // 6-day refund
        );
    } catch (const std::exception& e) {
        dinero::g_logger.error("[BridgedEscrowManager] Failed to build contract: " +
                              std::string(e.what()));
        return std::nullopt;
    }

    // 2. Store in registry
    auto& registry = contracts::MultiAssetContractRegistry::instance();
    if (!registry.storeContract(contract)) {
        dinero::g_logger.error("[BridgedEscrowManager] Failed to store contract");
        return std::nullopt;
    }

    // 3. Log
    auto total_locked = registry.getTotalLockedByAsset(asset_id);
    dinero::g_logger.info("[BridgedEscrowManager] Created multi-asset escrow: " +
                         contract.contract_id +
                         " | Asset: " + asset_id +
                         " | Amount: " + std::to_string(amount) +
                         " | Total locked in " + asset_id + ": " +
                         std::to_string(total_locked));

    return contract;
}

std::optional<bridge::ConversionResult> BridgedEscrowManager::releaseWithConversion(
    const std::string& escrow_id,
    const std::string& buyer_address,
    const std::string& target_asset,
    const std::string& sig_buyer,
    const std::string& sig_seller
) {
    dinero::g_logger.info("[BridgedEscrowManager] Releasing escrow with conversion: " +
                         escrow_id + " -> " + target_asset);

    // 1. Load contract from registry
    auto& registry = contracts::MultiAssetContractRegistry::instance();
    // Note: In real implementation, would need to add retrieval by escrow_id
    // For now, showing the workflow...

    // 2. Find best conversion route
    auto routes = bridge::RoutingEngine::find_all_routes(
        "USDT",           // escrow_asset (would come from contract)
        target_asset,
        {/* providers */},
        3                 // max 3 hops
    );

    if (routes.empty()) {
        dinero::g_logger.error("[BridgedEscrowManager] No conversion route found");
        return std::nullopt;
    }

    auto best_route = routes[0];
    dinero::g_logger.info("[BridgedEscrowManager] Best route: " + best_route.description());

    // 3. Execute conversion
    bridge::ConversionRequest req;
    req.from_asset = "USDT";          // Would come from contract
    req.to_asset = target_asset;
    req.amount = 100.0;               // Would come from contract
    req.dest_address = buyer_address;
    req.max_slippage_bps = 300;       // Max 3% slippage

    auto result = bridge_manager_.convert(req);

    if (!result.success) {
        dinero::g_logger.error("[BridgedEscrowManager] Conversion failed: " +
                              result.message);
        return std::nullopt;
    }

    dinero::g_logger.info("[BridgedEscrowManager] Conversion succeeded: " +
                         std::to_string(result.received_amount) + " " +
                         target_asset + " received");

    return result;
}

std::optional<bridge::ConversionRoute> BridgedEscrowManager::estimateConversion(
    const std::string& from_asset,
    const std::string& to_asset,
    double amount
) {
    auto routes = bridge::RoutingEngine::find_all_routes(
        from_asset,
        to_asset,
        {/* providers */},
        3
    );

    if (routes.empty()) {
        return std::nullopt;
    }

    // Return best route
    return routes[0];
}

std::vector<std::string> BridgedEscrowManager::listSupportedAssets() const {
    // Would query contract builder for supported assets
    return {"DIN", "BTC", "ETH", "USDT", "USDC", "EUR", "USD", "GBP"};
}

} // namespace p2p
} // namespace dinero
```

---

## Phase 4: RPC Methods

### New Header: `include/rpc/methods_multiasset.h`

```cpp
#pragma once

#include "din_json.h"

namespace dinero {
namespace rpc {

/**
 * Multi-Asset Escrow RPC Methods
 * 
 * Methods:
 * - multiasset.createescrow
 * - multiasset.releasetoasset
 * - multiasset.refundtoasset
 * - multiasset.listbyasset
 * - multiasset.estimateswap
 */

void registerMultiAssetRPC();

} // namespace rpc
} // namespace dinero
```

### Implementation: `src/rpc/methods_multiasset.cpp`

```cpp
#include "rpc/methods_multiasset.h"
#include "rpc/rpc_registry.h"
#include "daemon/execution_context.h"
#include "p2p/multiasset_escrow_manager.h"
#include "contracts/multiasset_contract_registry.h"
#include "contracts/multiasset_escrow_contract.h"
#include "common/logger.h"
#include <json/json.h>

extern RpcRegistry g_rpcRegistry;

namespace dinero {
namespace rpc {

using namespace dinero::contracts;
using namespace dinero::p2p;

// multiasset.createescrow
din::Json multiasset_createescrow_impl(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    try {
        // Parse parameters
        if (!params.isMember("buyer_pubkey")) {
            din::Json error;
            error["error"] = "Missing 'buyer_pubkey'";
            return error;
        }
        if (!params.isMember("seller_pubkey")) {
            din::Json error;
            error["error"] = "Missing 'seller_pubkey'";
            return error;
        }
        if (!params.isMember("asset")) {
            din::Json error;
            error["error"] = "Missing 'asset' (e.g., 'USDT')";
            return error;
        }
        if (!params.isMember("amount")) {
            din::Json error;
            error["error"] = "Missing 'amount'";
            return error;
        }

        std::string buyer_pk = params["buyer_pubkey"].asString();
        std::string seller_pk = params["seller_pubkey"].asString();
        std::string asset_id = params["asset"].asString();
        double amount = params["amount"].asDouble();
        std::string mediator_pk = params.isMember("mediator_pubkey") ?
            params["mediator_pubkey"].asString() : "MEDIATOR_PK";

        // Validate
        if (amount <= 0) {
            din::Json error;
            error["error"] = "Amount must be positive";
            return error;
        }

        // Check if asset is supported
        if (!MultiAssetEscrowBuilder::isAssetSupported(asset_id)) {
            din::Json error;
            error["error"] = "Unsupported asset: " + asset_id;
            return error;
        }

        // Build contract
        EscrowKeys keys;
        keys.buyer_pubkey = buyer_pk;
        keys.seller_pubkey = seller_pk;
        keys.mediator_pubkey = mediator_pk;

        AssetEscrowContract contract = 
            MultiAssetEscrowBuilder::buildMultiAssetContract(
                keys, asset_id, amount, 2880
            );

        // Store in registry
        auto& registry = MultiAssetContractRegistry::instance();
        if (!registry.storeContract(contract)) {
            din::Json error;
            error["error"] = "Failed to store contract";
            return error;
        }

        // Return
        din::Json result;
        result["contract_id"] = contract.contract_id;
        result["p2sh_address"] = contract.p2sh_address;
        result["asset"] = contract.asset_id;
        result["amount"] = contract.amount;
        result["decimals"] = contract.decimals;
        result["redeem_script"] = contract.redeem_script;
        result["refund_time"] = contract.refund_time;
        result["status"] = "pending";
        result["success"] = true;
        result["message"] = "Send " + std::to_string(amount) + " " + asset_id +
                           " to " + contract.p2sh_address;

        dinero::g_logger.info("[RPC] Created multi-asset escrow: " + contract.contract_id +
                             " | Asset: " + asset_id);

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = std::string("multiasset.createescrow error: ") + e.what();
        return error;
    }
}

// multiasset.listbyasset
din::Json multiasset_listbyasset_impl(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    try {
        if (!params.isMember("asset")) {
            din::Json error;
            error["error"] = "Missing 'asset' parameter";
            return error;
        }

        std::string asset_id = params["asset"].asString();
        auto& registry = MultiAssetContractRegistry::instance();

        auto contracts = registry.listByAsset(asset_id);
        double total_locked = registry.getTotalLockedByAsset(asset_id);

        din::Json result;
        result["asset"] = asset_id;
        result["total_locked"] = total_locked;
        result["contract_count"] = static_cast<int>(contracts.size());

        din::Json contracts_json(Json::arrayValue);
        for (const auto& contract : contracts) {
            din::Json c;
            c["contract_id"] = contract.contract_id;
            c["amount"] = contract.amount;
            c["status"] = contract.status;
            c["created_at"] = static_cast<Json::UInt64>(contract.created_at);
            contracts_json.append(c);
        }
        result["contracts"] = contracts_json;

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = std::string("multiasset.listbyasset error: ") + e.what();
        return error;
    }
}

// multiasset.estimateswap
din::Json multiasset_estimateswap_impl(
    const ExecutionContext& ctx,
    const din::Json& params
) {
    try {
        if (!params.isMember("from")) {
            din::Json error;
            error["error"] = "Missing 'from' asset";
            return error;
        }
        if (!params.isMember("to")) {
            din::Json error;
            error["error"] = "Missing 'to' asset";
            return error;
        }
        if (!params.isMember("amount")) {
            din::Json error;
            error["error"] = "Missing 'amount'";
            return error;
        }

        std::string from_asset = params["from"].asString();
        std::string to_asset = params["to"].asString();
        double amount = params["amount"].asDouble();

        auto& manager = BridgedEscrowManager::instance();
        auto route = manager.estimateConversion(from_asset, to_asset, amount);

        if (!route) {
            din::Json error;
            error["error"] = "No conversion route found";
            return error;
        }

        din::Json result;
        result["from"] = from_asset;
        result["to"] = to_asset;
        result["amount"] = amount;
        result["route"] = route->description();
        result["total_rate"] = route->total_rate;
        result["estimated_amount"] = amount * route->effective_rate();
        result["total_fee_bps"] = route->total_fee_bps;
        result["hop_count"] = route->hop_count;

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = std::string("multiasset.estimateswap error: ") + e.what();
        return error;
    }
}

void registerMultiAssetRPC() {
    // Register multiasset.createescrow
    g_rpcRegistry.registerRPC("multiasset.createescrow",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return multiasset_createescrow_impl(ctx, params);
        }
    );

    // Register multiasset.listbyasset
    g_rpcRegistry.registerRPC("multiasset.listbyasset",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return multiasset_listbyasset_impl(ctx, params);
        }
    );

    // Register multiasset.estimateswap
    g_rpcRegistry.registerRPC("multiasset.estimateswap",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return multiasset_estimateswap_impl(ctx, params);
        }
    );

    dinero::g_logger.info("[RPC] Registered multi-asset escrow RPC methods");
}

} // namespace rpc
} // namespace dinero
```

---

## Usage Examples

### Creating a Multi-Asset Escrow

```bash
# Create USDT escrow (100 USDT, 6-day refund)
dinero-cli multiasset.createescrow \
    "02abc123..." \      # buyer pubkey
    "03def456..." \      # seller pubkey
    "04ghi789..." \      # mediator pubkey
    "USDT" \             # asset to hold
    100.0                # amount

# Response:
{
  "contract_id": "contract_abc123...",
  "p2sh_address": "din1qxyz...",
  "asset": "USDT",
  "amount": 100.0,
  "decimals": 6,
  "refund_time": 145680,
  "message": "Send 100.0 USDT to din1qxyz...",
  "success": true
}
```

### Listing by Asset

```bash
# List all escrows holding USDT
dinero-cli multiasset.listbyasset "USDT"

# Response:
{
  "asset": "USDT",
  "total_locked": 500.0,
  "contract_count": 5,
  "contracts": [
    {
      "contract_id": "contract_abc123...",
      "amount": 100.0,
      "status": "locked",
      "created_at": 1699564800
    },
    ...
  ]
}
```

### Estimating Conversion

```bash
# Check conversion rate before releasing
dinero-cli multiasset.estimateswap \
    "from" "USDT" \
    "to" "EUR" \
    "amount" 100

# Response:
{
  "from": "USDT",
  "to": "EUR",
  "amount": 100.0,
  "route": "USDT→BTC→EUR via dex+coinbase",
  "total_rate": 0.805,
  "estimated_amount": 80.50,
  "total_fee_bps": 300,
  "hop_count": 2
}
```

---

## Testing Workflow

```bash
# 1. Create contract
CONTRACT=$(dinero-cli multiasset.createescrow ... | jq -r '.contract_id')

# 2. Fund contract
P2SH=$(dinero-cli contract.status $CONTRACT | jq -r '.p2sh_address')
dinero-cli sendtoaddress $P2SH 100  # USDT

# 3. Wait for confirmations
dinero-cli generate 6

# 4. Estimate conversion
dinero-cli multiasset.estimateswap "from" "USDT" "to" "EUR" "amount" 100

# 5. Release with conversion
dinero-cli multiasset.releasetoasset $CONTRACT "$BUYER_ADDR" "EUR" \
    "$SIG_BUYER" "$SIG_SELLER"

# 6. Verify completion
dinero-cli multiasset.listbyasset "USDT"
dinero-cli multiasset.listbyasset "EUR"
```

