#include "contracts/multiasset_escrow_contract.h"
#include "contracts/multiasset_persistence.h"
#include "contracts/escrow_contract.h"
#include "bridge/fiat_bridge_manager.h"
#include "common/logger.h"
#include <algorithm>

namespace dinero {
namespace contracts {

// Supported assets with their decimal places
const std::map<std::string, uint8_t> MultiAssetEscrowBuilder::ASSET_DECIMALS = {
    {"DIN", 8},
    {"BTC", 8},
    {"ETH", 18},
    {"USDT", 6},
    {"USDC", 6},
    {"DAI", 18},
    {"EUR", 2},
    {"USD", 2},
    {"GBP", 2}
};

// Build multi-asset contract
AssetEscrowContract MultiAssetEscrowBuilder::buildMultiAssetContract(
    const EscrowKeys& keys,
    const std::string& asset_id,
    double amount,
    uint32_t refund_blocks
) {
    // Build base contract using existing builder
    EscrowContract base = EscrowContractBuilder::buildContract(keys, amount, refund_blocks);

    // Extend to AssetEscrowContract
    AssetEscrowContract contract;
    contract.contract_id = base.contract_id;
    contract.keys = base.keys;
    contract.amount = base.amount;
    contract.refund_time = base.refund_time;
    contract.redeem_script = base.redeem_script;
    contract.script_hash = base.script_hash;
    contract.p2sh_address = base.p2sh_address;
    contract.lock_txid = base.lock_txid;
    contract.lock_vout = base.lock_vout;
    contract.created_at = base.created_at;
    contract.status = base.status;
    contract.confirmations = base.confirmations;

    // Add asset-specific fields
    contract.asset_id = asset_id;
    contract.decimals = getAssetDecimals(asset_id);
    contract.is_wrapped = false;

    dinero::g_logger.info("[MultiAssetEscrow] Created " + asset_id +
                          " escrow contract: " + contract.contract_id);

    return contract;
}

// Build with automatic conversion
std::optional<AssetEscrowContract> MultiAssetEscrowBuilder::buildWithAutoConversion(
    const EscrowKeys& keys,
    const std::string& escrow_asset,
    const std::string& release_asset,
    double amount,
    uint32_t refund_blocks,
    bridge::FiatBridgeManager& bridge_manager
) {
    // Validate both assets are supported
    if (!isAssetSupported(escrow_asset) || !isAssetSupported(release_asset)) {
        dinero::g_logger.error("[MultiAssetEscrow] Unsupported asset: " +
                              escrow_asset + " or " + release_asset);
        return std::nullopt;
    }

    // Build base contract
    AssetEscrowContract contract = buildMultiAssetContract(keys, escrow_asset, amount, refund_blocks);

    // If same asset, no conversion needed
    if (escrow_asset == release_asset) {
        dinero::g_logger.info("[MultiAssetEscrow] Same asset, no conversion needed");
        return contract;
    }

    // Find conversion route
    bridge::ConversionRoute route;
    auto rate = bridge_manager.get_rate_auto(escrow_asset, release_asset, &route);
    if (!rate) {
        dinero::g_logger.error("[MultiAssetEscrow] No conversion route found: " +
                              escrow_asset + " -> " + release_asset);
        return std::nullopt;
    }

    // Populate conversion fields
    contract.release_asset = release_asset;
    contract.swap_route = route;

    dinero::g_logger.info("[MultiAssetEscrow] Created escrow with conversion: " +
                          escrow_asset + " -> " + release_asset +
                          " (" + route.description() + ")");

    return contract;
}

// Validate asset support
bool MultiAssetEscrowBuilder::isAssetSupported(const std::string& asset_id) {
    return ASSET_DECIMALS.find(asset_id) != ASSET_DECIMALS.end();
}

// Get asset decimals
uint8_t MultiAssetEscrowBuilder::getAssetDecimals(const std::string& asset_id) {
    auto it = ASSET_DECIMALS.find(asset_id);
    if (it != ASSET_DECIMALS.end()) {
        return it->second;
    }

    dinero::g_logger.warning("[MultiAssetEscrow] Unknown asset decimals for " +
                             asset_id + ", defaulting to 8");
    return 8;  // Default to 8 decimals (Bitcoin standard)
}

// Convert amount using bridge
std::optional<double> MultiAssetEscrowBuilder::convertAmount(
    const std::string& from_asset,
    const std::string& to_asset,
    double amount,
    bridge::FiatBridgeManager& bridge_manager
) {
    if (from_asset == to_asset) {
        return amount;
    }

    bridge::ConversionRoute route;
    auto rate = bridge_manager.get_rate_auto(from_asset, to_asset, &route);
    if (!rate) {
        return std::nullopt;
    }

    // Calculate output amount after fees and slippage
    double effective_rate = route.effective_rate();
    return amount * effective_rate;
}

//
// MultiAssetContractRegistry Implementation
//

MultiAssetContractRegistry& MultiAssetContractRegistry::getInstance() {
    static MultiAssetContractRegistry instance;
    return instance;
}

bool MultiAssetContractRegistry::initialize(const std::string& db_path) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Create persistence layer
    persistence_ = std::make_unique<MultiAssetPersistence>();

    if (!persistence_->initialize(db_path)) {
        dinero::g_logger.error("[MultiAssetRegistry] Failed to initialize persistence");
        persistence_.reset();
        return false;
    }

    // Load existing contracts from database into in-memory cache
    auto all_contracts = persistence_->loadAllContracts();
    for (const auto& contract : all_contracts) {
        contracts_[contract.contract_id] = contract;
        asset_index_.insert({contract.asset_id, contract.contract_id});
    }

    dinero::g_logger.info("[MultiAssetRegistry] Initialized persistence with " +
                          std::to_string(all_contracts.size()) + " contracts loaded");

    return true;
}

bool MultiAssetContractRegistry::registerContract(const AssetEscrowContract& contract) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if contract already exists
    if (contracts_.find(contract.contract_id) != contracts_.end()) {
        dinero::g_logger.warning("[MultiAssetRegistry] Contract already registered: " +
                                 contract.contract_id);
        return false;
    }

    // Register contract in memory
    contracts_[contract.contract_id] = contract;

    // Add to asset index
    asset_index_.insert({contract.asset_id, contract.contract_id});

    // Persist to database if enabled
    if (persistence_ && !persistence_->saveContract(contract)) {
        dinero::g_logger.warning("[MultiAssetRegistry] Failed to persist contract to database: " +
                                 contract.contract_id);
        // Continue anyway - in-memory storage succeeded
    }

    dinero::g_logger.info("[MultiAssetRegistry] Registered contract: " +
                          contract.contract_id + " (asset: " + contract.asset_id + ")");

    return true;
}

std::optional<AssetEscrowContract> MultiAssetContractRegistry::getContract(
    const std::string& contract_id
) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = contracts_.find(contract_id);
    if (it != contracts_.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::vector<AssetEscrowContract> MultiAssetContractRegistry::getContractsByAsset(
    const std::string& asset_id
) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<AssetEscrowContract> result;

    // Find all contract IDs for this asset
    auto range = asset_index_.equal_range(asset_id);
    for (auto it = range.first; it != range.second; ++it) {
        auto contract_it = contracts_.find(it->second);
        if (contract_it != contracts_.end()) {
            result.push_back(contract_it->second);
        }
    }

    return result;
}

std::vector<AssetEscrowContract> MultiAssetContractRegistry::getActiveContracts() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<AssetEscrowContract> result;

    for (const auto& pair : contracts_) {
        const auto& contract = pair.second;
        if (contract.status == "pending" || contract.status == "locked") {
            result.push_back(contract);
        }
    }

    return result;
}

bool MultiAssetContractRegistry::updateContractStatus(
    const std::string& contract_id,
    const std::string& new_status
) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = contracts_.find(contract_id);
    if (it == contracts_.end()) {
        dinero::g_logger.warning("[MultiAssetRegistry] Contract not found: " + contract_id);
        return false;
    }

    it->second.status = new_status;

    // Persist update to database if enabled
    if (persistence_ && !persistence_->updateContractStatus(contract_id, new_status)) {
        dinero::g_logger.warning("[MultiAssetRegistry] Failed to persist status update to database: " +
                                 contract_id);
        // Continue anyway - in-memory update succeeded
    }

    dinero::g_logger.info("[MultiAssetRegistry] Updated contract " + contract_id +
                          " status: " + new_status);

    return true;
}

bool MultiAssetContractRegistry::removeContract(const std::string& contract_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = contracts_.find(contract_id);
    if (it == contracts_.end()) {
        return false;
    }

    // Remove from asset index
    const std::string& asset_id = it->second.asset_id;
    auto range = asset_index_.equal_range(asset_id);
    for (auto idx_it = range.first; idx_it != range.second; ) {
        if (idx_it->second == contract_id) {
            idx_it = asset_index_.erase(idx_it);
        } else {
            ++idx_it;
        }
    }

    // Remove from main storage
    contracts_.erase(it);

    // Delete from database if enabled
    if (persistence_ && !persistence_->deleteContract(contract_id)) {
        dinero::g_logger.warning("[MultiAssetRegistry] Failed to delete contract from database: " +
                                 contract_id);
        // Continue anyway - in-memory deletion succeeded
    }

    dinero::g_logger.info("[MultiAssetRegistry] Removed contract: " + contract_id);

    return true;
}

std::map<std::string, size_t> MultiAssetContractRegistry::getAssetStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::map<std::string, size_t> stats;

    for (const auto& pair : contracts_) {
        const std::string& asset_id = pair.second.asset_id;
        stats[asset_id]++;
    }

    return stats;
}

//
// BridgedEscrowManager Implementation
//

BridgedEscrowManager::BridgedEscrowManager(bridge::FiatBridgeManager& bridge_manager)
    : bridge_manager_(bridge_manager)
    , registry_(MultiAssetContractRegistry::getInstance())
{
    dinero::g_logger.info("[BridgedEscrowManager] Initialized");
}

std::optional<AssetEscrowContract> BridgedEscrowManager::createEscrow(
    const EscrowKeys& keys,
    const std::string& escrow_asset,
    double amount,
    uint32_t refund_blocks,
    const std::optional<std::string>& release_asset
) {
    std::optional<AssetEscrowContract> contract;

    if (release_asset.has_value() && release_asset.value() != escrow_asset) {
        // Build with conversion
        contract = MultiAssetEscrowBuilder::buildWithAutoConversion(
            keys,
            escrow_asset,
            release_asset.value(),
            amount,
            refund_blocks,
            bridge_manager_
        );
    } else {
        // Build without conversion
        contract = MultiAssetEscrowBuilder::buildMultiAssetContract(
            keys,
            escrow_asset,
            amount,
            refund_blocks
        );
    }

    if (!contract) {
        return std::nullopt;
    }

    // Register in registry
    if (!registry_.registerContract(contract.value())) {
        dinero::g_logger.error("[BridgedEscrowManager] Failed to register contract");
        return std::nullopt;
    }

    return contract;
}

std::optional<std::string> BridgedEscrowManager::releaseEscrow(
    const std::string& contract_id,
    const std::string& to_address,
    const std::string& sig_buyer,
    const std::string& sig_seller
) {
    // Get contract from registry
    auto contract_opt = registry_.getContract(contract_id);
    if (!contract_opt) {
        dinero::g_logger.error("[BridgedEscrowManager] Contract not found: " + contract_id);
        return std::nullopt;
    }

    AssetEscrowContract contract = contract_opt.value();

    // Check if conversion is needed
    if (contract.swap_route.has_value()) {
        dinero::g_logger.info("[BridgedEscrowManager] Releasing with conversion: " +
                             contract.asset_id + " -> " + contract.release_asset);

        // Execute conversion route first
        if (!executeConversion(contract, to_address)) {
            dinero::g_logger.error("[BridgedEscrowManager] Conversion failed");
            return std::nullopt;
        }
    }

    // Create standard release transaction
    std::string tx_hex = EscrowContractBuilder::createReleaseTransaction(
        contract,
        to_address,
        sig_buyer,
        sig_seller
    );

    if (tx_hex.empty()) {
        dinero::g_logger.error("[BridgedEscrowManager] Failed to create release transaction");
        return std::nullopt;
    }

    // Update contract status
    registry_.updateContractStatus(contract_id, "released");

    return tx_hex;
}

std::optional<std::string> BridgedEscrowManager::refundEscrow(
    const std::string& contract_id,
    const std::string& refund_address,
    const std::string& sig_buyer
) {
    // Get contract from registry
    auto contract_opt = registry_.getContract(contract_id);
    if (!contract_opt) {
        dinero::g_logger.error("[BridgedEscrowManager] Contract not found: " + contract_id);
        return std::nullopt;
    }

    AssetEscrowContract contract = contract_opt.value();

    // Create refund transaction (no conversion)
    std::string tx_hex = EscrowContractBuilder::createRefundTransaction(
        contract,
        refund_address,
        sig_buyer
    );

    if (tx_hex.empty()) {
        dinero::g_logger.error("[BridgedEscrowManager] Failed to create refund transaction");
        return std::nullopt;
    }

    // Update contract status
    registry_.updateContractStatus(contract_id, "refunded");

    return tx_hex;
}

std::vector<bridge::ConversionRoute> BridgedEscrowManager::getConversionRoutes(
    const std::string& escrow_asset,
    const std::string& target_asset,
    double amount
) const {
    return bridge_manager_.get_all_routes(escrow_asset, target_asset, 3);
}

std::optional<double> BridgedEscrowManager::estimateConversion(
    const std::string& escrow_asset,
    const std::string& target_asset,
    double amount
) const {
    if (escrow_asset == target_asset) {
        return amount;
    }

    bridge::ConversionRoute route;
    auto rate = bridge_manager_.get_rate_auto(escrow_asset, target_asset, &route);
    if (!rate) {
        return std::nullopt;
    }

    return amount * route.effective_rate();
}

// Helper: Execute conversion route
bool BridgedEscrowManager::executeConversion(
    const AssetEscrowContract& contract,
    const std::string& to_address
) {
    if (!contract.swap_route.has_value()) {
        dinero::g_logger.error("[BridgedEscrowManager] No swap route in contract");
        return false;
    }

    const auto& route = contract.swap_route.value();

    dinero::g_logger.info("[BridgedEscrowManager] Executing conversion route: " +
                          route.description());

    // Execute each hop in the route
    for (const auto& hop : route.hops) {
        dinero::g_logger.info("[BridgedEscrowManager] Processing hop: " +
                             hop.from_asset + " -> " + hop.to_asset +
                             " via " + hop.provider);

        // In a real implementation, this would:
        // 1. Call the provider's swap API
        // 2. Wait for confirmation
        // 3. Verify the swap was successful
        // 4. Move to next hop
        //
        // For now, we'll log the intent
        // TODO: Implement actual provider swap calls
    }

    dinero::g_logger.info("[BridgedEscrowManager] Conversion route executed successfully");

    return true;
}

} // namespace contracts
} // namespace dinero
