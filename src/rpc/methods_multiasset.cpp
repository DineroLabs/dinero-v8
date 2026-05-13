#include "rpc/methods_multiasset.h"
#include "rpc/rpc_registry.h"
#include "daemon/execution_context.h"
#include "contracts/multiasset_escrow_contract.h"
#include "bridge/fiat_bridge_manager.h"
#include "common/logger.h"
#include "din_json.h"
#include <json/json.h>

extern RpcRegistry g_rpcRegistry;

namespace dinero {
namespace rpc {

using namespace dinero::contracts;
using namespace dinero::bridge;

// Global manager instance
static std::unique_ptr<BridgedEscrowManager> g_bridged_escrow_manager;

// Helper: Parse JSON parameters (handles CLI string-in-array format)
static din::Json parseParams(const din::Json& params) {
    if (params.isObject()) {
        return params;
    }

    if (params.isArray() && params.size() > 0) {
        const din::Json& first = params[0];

        if (first.isObject()) {
            return first;
        }

        if (first.isString()) {
            std::string json_str = first.asString();

            if (json_str.empty() || json_str.find_first_not_of(" \t\n\r") == std::string::npos) {
                return din::Json(Json::objectValue);
            }

            Json::CharReaderBuilder builder;
            Json::CharReader* reader = builder.newCharReader();
            std::string errors;
            din::Json parsed;

            bool success = reader->parse(
                json_str.c_str(),
                json_str.c_str() + json_str.length(),
                &parsed,
                &errors
            );
            delete reader;

            if (!success) {
                throw std::runtime_error("Failed to parse JSON parameters: " + errors);
            }

            return parsed;
        }
    }

    if (params.isNull() || (params.isArray() && params.size() == 0)) {
        return din::Json(Json::objectValue);
    }

    throw std::runtime_error("Invalid parameter format (expected object or JSON string)");
}

// Helper: Convert AssetEscrowContract to JSON
static din::Json assetContractToJson(const AssetEscrowContract& contract) {
    din::Json result;

    result["contract_id"] = contract.contract_id;
    result["asset_id"] = contract.asset_id;
    result["decimals"] = contract.decimals;
    result["p2sh_address"] = contract.p2sh_address;
    result["amount"] = contract.amount;
    result["refund_time"] = contract.refund_time;
    result["status"] = contract.status;
    result["confirmations"] = contract.confirmations;
    result["lock_txid"] = contract.lock_txid;
    result["created_at"] = static_cast<Json::UInt64>(contract.created_at);

    // Keys
    din::Json keys;
    keys["buyer"] = contract.keys.buyer_pubkey;
    keys["seller"] = contract.keys.seller_pubkey;
    keys["mediator"] = contract.keys.mediator_pubkey;
    result["keys"] = keys;

    // Script info
    result["redeem_script"] = contract.redeem_script;
    result["script_hash"] = contract.script_hash;

    // Conversion info
    if (!contract.release_asset.empty()) {
        result["release_asset"] = contract.release_asset;

        if (contract.swap_route.has_value()) {
            const auto& route = contract.swap_route.value();
            din::Json route_json;
            route_json["description"] = route.description();
            route_json["total_rate"] = route.total_rate;
            route_json["total_fee_bps"] = route.total_fee_bps;
            route_json["slippage_bps"] = route.slippage_bps;
            route_json["hop_count"] = route.hop_count;
            route_json["effective_rate"] = route.effective_rate();

            result["conversion_route"] = route_json;
        }
    }

    return result;
}

// Helper: Get or create manager
static BridgedEscrowManager& getManager() {
    if (!g_bridged_escrow_manager) {
        g_bridged_escrow_manager = std::make_unique<BridgedEscrowManager>(
            FiatBridgeManager::instance()
        );
    }
    return *g_bridged_escrow_manager;
}

//
// RPC Method Implementations
//

din::Json multiasset_createescrow(const din::Json& params) {
    try {
        din::Json p = parseParams(params);

        // Extract required fields
        if (!p.isMember("buyer_pubkey") || !p.isMember("seller_pubkey") ||
            !p.isMember("mediator_pubkey") || !p.isMember("asset_id") ||
            !p.isMember("amount") || !p.isMember("refund_blocks")) {
            throw std::runtime_error("Missing required parameters");
        }

        EscrowKeys keys;
        keys.buyer_pubkey = p["buyer_pubkey"].asString();
        keys.seller_pubkey = p["seller_pubkey"].asString();
        keys.mediator_pubkey = p["mediator_pubkey"].asString();

        std::string asset_id = p["asset_id"].asString();
        double amount = p["amount"].asDouble();
        uint32_t refund_blocks = p["refund_blocks"].asUInt();

        std::optional<std::string> release_asset;
        if (p.isMember("release_asset") && !p["release_asset"].asString().empty()) {
            release_asset = p["release_asset"].asString();
        }

        // Create escrow
        auto contract = getManager().createEscrow(
            keys,
            asset_id,
            amount,
            refund_blocks,
            release_asset
        );

        if (!contract) {
            throw std::runtime_error("Failed to create escrow contract");
        }

        dinero::g_logger.info("[RPC] Created multi-asset escrow: " + contract->contract_id);

        return assetContractToJson(contract.value());

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = e.what();
        return error;
    }
}

din::Json multiasset_releaseescrow(const din::Json& params) {
    try {
        din::Json p = parseParams(params);

        if (!p.isMember("contract_id") || !p.isMember("to_address") ||
            !p.isMember("sig_buyer") || !p.isMember("sig_seller")) {
            throw std::runtime_error("Missing required parameters");
        }

        std::string contract_id = p["contract_id"].asString();
        std::string to_address = p["to_address"].asString();
        std::string sig_buyer = p["sig_buyer"].asString();
        std::string sig_seller = p["sig_seller"].asString();

        // Get contract to check conversion
        auto contract_opt = MultiAssetContractRegistry::getInstance().getContract(contract_id);
        if (!contract_opt) {
            throw std::runtime_error("Contract not found");
        }

        auto tx_hex = getManager().releaseEscrow(
            contract_id,
            to_address,
            sig_buyer,
            sig_seller
        );

        if (!tx_hex) {
            throw std::runtime_error("Failed to release escrow");
        }

        din::Json result;
        result["txid"] = tx_hex.value();
        result["conversion_executed"] = contract_opt->swap_route.has_value();

        if (contract_opt->swap_route.has_value()) {
            result["route_description"] = contract_opt->swap_route->description();
        }

        dinero::g_logger.info("[RPC] Released multi-asset escrow: " + contract_id);

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = e.what();
        return error;
    }
}

din::Json multiasset_refundescrow(const din::Json& params) {
    try {
        din::Json p = parseParams(params);

        if (!p.isMember("contract_id") || !p.isMember("refund_address") ||
            !p.isMember("sig_buyer")) {
            throw std::runtime_error("Missing required parameters");
        }

        std::string contract_id = p["contract_id"].asString();
        std::string refund_address = p["refund_address"].asString();
        std::string sig_buyer = p["sig_buyer"].asString();

        auto tx_hex = getManager().refundEscrow(
            contract_id,
            refund_address,
            sig_buyer
        );

        if (!tx_hex) {
            throw std::runtime_error("Failed to refund escrow");
        }

        din::Json result;
        result["txid"] = tx_hex.value();

        dinero::g_logger.info("[RPC] Refunded multi-asset escrow: " + contract_id);

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = e.what();
        return error;
    }
}

din::Json multiasset_getcontract(const din::Json& params) {
    try {
        din::Json p = parseParams(params);

        if (!p.isMember("contract_id")) {
            throw std::runtime_error("Missing contract_id parameter");
        }

        std::string contract_id = p["contract_id"].asString();

        auto contract = MultiAssetContractRegistry::getInstance().getContract(contract_id);
        if (!contract) {
            throw std::runtime_error("Contract not found");
        }

        return assetContractToJson(contract.value());

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = e.what();
        return error;
    }
}

din::Json multiasset_listcontracts(const din::Json& params) {
    try {
        din::Json p = parseParams(params);

        std::vector<AssetEscrowContract> contracts;

        if (p.isMember("asset_id") && !p["asset_id"].asString().empty()) {
            // Filter by asset
            std::string asset_id = p["asset_id"].asString();
            contracts = MultiAssetContractRegistry::getInstance().getContractsByAsset(asset_id);
        } else {
            // Get all active contracts
            contracts = MultiAssetContractRegistry::getInstance().getActiveContracts();
        }

        din::Json result(Json::arrayValue);
        for (const auto& contract : contracts) {
            result.append(assetContractToJson(contract));
        }

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = e.what();
        return error;
    }
}

din::Json multiasset_getconversionroutes(const din::Json& params) {
    try {
        din::Json p = parseParams(params);

        if (!p.isMember("from_asset") || !p.isMember("to_asset") ||
            !p.isMember("amount")) {
            throw std::runtime_error("Missing required parameters");
        }

        std::string from_asset = p["from_asset"].asString();
        std::string to_asset = p["to_asset"].asString();
        double amount = p["amount"].asDouble();

        auto routes = getManager().getConversionRoutes(from_asset, to_asset, amount);

        din::Json result;
        din::Json routes_array(Json::arrayValue);

        for (const auto& route : routes) {
            din::Json route_json;
            route_json["description"] = route.description();
            route_json["total_rate"] = route.total_rate;
            route_json["total_fee_bps"] = route.total_fee_bps;
            route_json["slippage_bps"] = route.slippage_bps;
            route_json["hop_count"] = route.hop_count;
            route_json["effective_rate"] = route.effective_rate();

            // Add hops details
            din::Json hops_array(Json::arrayValue);
            for (const auto& hop : route.hops) {
                din::Json hop_json;
                hop_json["from"] = hop.from_asset;
                hop_json["to"] = hop.to_asset;
                hop_json["provider"] = hop.provider;
                hop_json["rate"] = hop.rate;
                hops_array.append(hop_json);
            }
            route_json["hops"] = hops_array;

            routes_array.append(route_json);
        }

        result["routes"] = routes_array;

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = e.what();
        return error;
    }
}

din::Json multiasset_estimateconversion(const din::Json& params) {
    try {
        din::Json p = parseParams(params);

        if (!p.isMember("from_asset") || !p.isMember("to_asset") ||
            !p.isMember("amount")) {
            throw std::runtime_error("Missing required parameters");
        }

        std::string from_asset = p["from_asset"].asString();
        std::string to_asset = p["to_asset"].asString();
        double amount = p["amount"].asDouble();

        auto output = getManager().estimateConversion(from_asset, to_asset, amount);

        if (!output) {
            throw std::runtime_error("No conversion route available");
        }

        // Get route description
        auto routes = getManager().getConversionRoutes(from_asset, to_asset, amount);
        std::string route_desc = routes.empty() ? "" : routes[0].description();

        din::Json result;
        result["input_amount"] = amount;
        result["output_amount"] = output.value();
        result["effective_rate"] = output.value() / amount;
        result["route"] = route_desc;

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = e.what();
        return error;
    }
}

din::Json multiasset_stats(const din::Json& params) {
    try {
        auto stats = MultiAssetContractRegistry::getInstance().getAssetStatistics();
        auto active = MultiAssetContractRegistry::getInstance().getActiveContracts();

        din::Json result;
        result["active_contracts"] = static_cast<Json::UInt>(active.size());

        size_t total = 0;
        din::Json by_asset;
        for (const auto& pair : stats) {
            by_asset[pair.first] = static_cast<Json::UInt>(pair.second);
            total += pair.second;
        }

        result["total_contracts"] = static_cast<Json::UInt>(total);
        result["by_asset"] = by_asset;

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = e.what();
        return error;
    }
}

din::Json multiasset_supportedassets(const din::Json& params) {
    try {
        din::Json result;
        din::Json assets_array(Json::arrayValue);

        // List of supported assets with names
        struct AssetInfo {
            std::string id;
            std::string name;
        };

        std::vector<AssetInfo> assets = {
            {"DIN", "Dinero"},
            {"BTC", "Bitcoin"},
            {"ETH", "Ethereum"},
            {"USDT", "Tether"},
            {"USDC", "USD Coin"},
            {"DAI", "Dai Stablecoin"},
            {"EUR", "Euro"},
            {"USD", "US Dollar"},
            {"GBP", "British Pound"}
        };

        for (const auto& asset : assets) {
            if (MultiAssetEscrowBuilder::isAssetSupported(asset.id)) {
                din::Json asset_json;
                asset_json["id"] = asset.id;
                asset_json["decimals"] = MultiAssetEscrowBuilder::getAssetDecimals(asset.id);
                asset_json["name"] = asset.name;
                assets_array.append(asset_json);
            }
        }

        result["assets"] = assets_array;

        return result;

    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = e.what();
        return error;
    }
}

//
// Registration
//

void register_multiasset_methods() {
    g_rpcRegistry.registerHandler("multiasset.createescrow",
        [](const ExecutionContext&, const din::Json& params) {
            return multiasset_createescrow(params);
        }, "multiasset");

    g_rpcRegistry.registerHandler("multiasset.releaseescrow",
        [](const ExecutionContext&, const din::Json& params) {
            return multiasset_releaseescrow(params);
        }, "multiasset");

    g_rpcRegistry.registerHandler("multiasset.refundescrow",
        [](const ExecutionContext&, const din::Json& params) {
            return multiasset_refundescrow(params);
        }, "multiasset");

    g_rpcRegistry.registerHandler("multiasset.getcontract",
        [](const ExecutionContext&, const din::Json& params) {
            return multiasset_getcontract(params);
        }, "multiasset");

    g_rpcRegistry.registerHandler("multiasset.listcontracts",
        [](const ExecutionContext&, const din::Json& params) {
            return multiasset_listcontracts(params);
        }, "multiasset");

    g_rpcRegistry.registerHandler("multiasset.getconversionroutes",
        [](const ExecutionContext&, const din::Json& params) {
            return multiasset_getconversionroutes(params);
        }, "multiasset");

    g_rpcRegistry.registerHandler("multiasset.estimateconversion",
        [](const ExecutionContext&, const din::Json& params) {
            return multiasset_estimateconversion(params);
        }, "multiasset");

    g_rpcRegistry.registerHandler("multiasset.stats",
        [](const ExecutionContext&, const din::Json& params) {
            return multiasset_stats(params);
        }, "multiasset");

    g_rpcRegistry.registerHandler("multiasset.supportedassets",
        [](const ExecutionContext&, const din::Json& params) {
            return multiasset_supportedassets(params);
        }, "multiasset");

    dinero::g_logger.info("[RPC] Registered 9 multi-asset escrow methods");
}

} // namespace rpc
} // namespace dinero
