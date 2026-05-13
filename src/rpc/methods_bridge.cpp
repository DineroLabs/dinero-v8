#include "rpc/methods_bridge.h"
#include "rpc/rpc_registry.h"
#include "daemon/execution_context.h"
#include "bridge/fiat_bridge_manager.h"
#include "daemon/arp_manager.h"
#include "common/logger.h"
#include "din_json.h"
#include <json/json.h>
#include <iostream>

extern RpcRegistry g_rpcRegistry;

namespace dinero {
namespace rpc {

// ═══════════════════════════════════════════════════════════════
// Bridge RPC Implementation Functions
// ═══════════════════════════════════════════════════════════════

din::Json bridge_getrate_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        std::string from, to;

        // Parse parameters (positional or named)
        if (params.isArray() && params.size() >= 2) {
            from = params[0].asString();
            to = params[1].asString();
        } else if (params.isObject() && params.isMember("from") && params.isMember("to")) {
            from = params["from"].asString();
            to = params["to"].asString();
        } else {
            result["error"] = "Missing 'from' or 'to' parameter";
            result["code"] = -32602;
            return result;
        }

        auto rate = bridge::FiatBridgeManager::instance().get_rate(from, to);
        double confidence = rate.has_value() ? 0.5 : 0.0;  // Simple heuristic
        bool used_arp = false;

        // If no market rate and querying DIN→USD, use ARP as fallback
        if (!rate && (from == "DIN" || from == "din") && (to == "USD" || to == "usd")) {
            auto arp = ArpManager::instance().getCurrent();
            if (arp) {
                rate = arp->price_usd;
                confidence = 0.0;  // Pure ARP
                used_arp = true;
            }
        }

        // If both market rate and ARP available for DIN→USD, blend them
        if (rate && !used_arp && (from == "DIN" || from == "din") && (to == "USD" || to == "usd")) {
            auto arp = ArpManager::instance().getCurrent();
            if (arp && arp->confidence < 1.0) {
                auto blended = ArpManager::instance().getBlended(*rate, confidence);
                if (blended) {
                    rate = blended->price_usd;
                    confidence = blended->confidence;
                    used_arp = true;
                }
            }
        }

        if (rate) {
            result["from"] = from;
            result["to"] = to;
            result["rate"] = *rate;
            result["confidence"] = confidence;
            if (used_arp) {
                result["source"] = (confidence > 0.1) ? "blended_arp_market" : "arp_only";
            } else {
                result["source"] = "market";
            }
            result["rpc_schema"] = "din.bridge.v1";
        } else {
            result["error"] = "Rate not available for " + from + " -> " + to;
            result["code"] = -32000;
        }

    } catch (const std::exception& e) {
        result["error"] = std::string("bridge.getrate error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

din::Json bridge_convert_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        bridge::ConversionRequest req;

        // Parse parameters
        if (params.isArray() && params.size() >= 3) {
            // Positional: bridge.convert "DIN" "USDT" 100 ["dex"]
            req.from_asset = params[0].asString();
            req.to_asset = params[1].asString();
            req.amount = params[2].asDouble();

            if (params.size() > 3) {
                req.provider_hint = params[3].asString();
            }
        } else if (params.isArray() && params.size() > 0 && params[0].isObject()) {
            // JSON object in array
            const din::Json& obj = params[0];
            req.from_asset = obj.get("from", "DIN").asString();
            req.to_asset = obj.get("to", "USDT").asString();
            req.amount = obj.get("amount", 0.0).asDouble();
            req.provider_hint = obj.get("provider", "").asString();

            if (obj.isMember("address")) {
                req.dest_address = obj["address"].asString();
            }
        } else if (params.isObject()) {
            // Direct object
            req.from_asset = params.get("from", "DIN").asString();
            req.to_asset = params.get("to", "USDT").asString();
            req.amount = params.get("amount", 0.0).asDouble();
            req.provider_hint = params.get("provider", "").asString();

            if (params.isMember("address")) {
                req.dest_address = params["address"].asString();
            }
        } else {
            result["error"] = "Invalid parameters for bridge.convert";
            result["code"] = -32602;
            return result;
        }

        // Validate
        if (req.amount <= 0) {
            result["error"] = "Amount must be positive";
            result["code"] = -32602;
            return result;
        }

        // Execute conversion
        auto conv_result = bridge::FiatBridgeManager::instance().convert(req);

        result["success"] = conv_result.success;
        result["provider"] = conv_result.provider;
        result["rate"] = conv_result.rate;
        result["received_amount"] = conv_result.received_amount;
        result["txid"] = conv_result.txid;
        result["message"] = conv_result.message;
        result["fee_amount"] = conv_result.fee_amount;
        result["slippage_bps"] = conv_result.slippage_bps;
        result["timestamp"] = static_cast<Json::UInt64>(conv_result.timestamp);
        result["rpc_schema"] = "din.bridge.v1";

        dinero::g_logger.info("[Bridge RPC] Conversion: " + std::to_string(req.amount) +
                              " " + req.from_asset + " -> " + conv_result.provider +
                              " (" + conv_result.txid + ")");

    } catch (const std::exception& e) {
        result["error"] = std::string("bridge.convert error: ") + e.what();
        result["code"] = -1;
        result["success"] = false;
    }

    return result;
}

din::Json bridge_providers_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        auto providers = bridge::FiatBridgeManager::instance().list_providers();

        din::Json providers_array(Json::arrayValue);
        for (const auto& name : providers) {
            providers_array.append(name);
        }

        result["providers"] = providers_array;
        result["count"] = static_cast<int>(providers.size());
        result["rpc_schema"] = "din.bridge.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("bridge.providers error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

din::Json bridge_status_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        auto providers = bridge::FiatBridgeManager::instance().list_providers();
        auto rates = bridge::FiatBridgeManager::instance().get_cached_rates();

        result["active_providers"] = static_cast<int>(providers.size());

        // Providers array
        din::Json providers_array(Json::arrayValue);
        for (const auto& name : providers) {
            providers_array.append(name);
        }
        result["providers"] = providers_array;

        // Cached rates
        din::Json rates_obj;
        for (const auto& [pair, rate] : rates) {
            rates_obj[pair] = rate;
        }
        result["cached_rates"] = rates_obj;
        result["cached_rate_count"] = static_cast<int>(rates.size());

        result["rpc_schema"] = "din.bridge.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("bridge.status error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

din::Json bridge_refresh_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        bridge::FiatBridgeManager::instance().refresh_rates();

        auto rates = bridge::FiatBridgeManager::instance().get_cached_rates();

        result["success"] = true;
        result["refreshed_rates"] = static_cast<int>(rates.size());
        result["message"] = "Exchange rates refreshed successfully";
        result["rpc_schema"] = "din.bridge.v1";

        dinero::g_logger.info("[Bridge RPC] Refreshed " + std::to_string(rates.size()) + " rates");

    } catch (const std::exception& e) {
        result["error"] = std::string("bridge.refresh error: ") + e.what();
        result["code"] = -1;
        result["success"] = false;
    }

    return result;
}

din::Json bridge_findroute_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        std::string from, to;

        // Parse parameters
        if (params.isArray() && params.size() >= 2) {
            from = params[0].asString();
            to = params[1].asString();
        } else if (params.isObject() && params.isMember("from") && params.isMember("to")) {
            from = params["from"].asString();
            to = params["to"].asString();
        } else {
            result["error"] = "Missing 'from' or 'to' parameter";
            result["code"] = -32602;
            return result;
        }

        // Use automatic routing
        bridge::ConversionRoute route_info;
        auto rate = bridge::FiatBridgeManager::instance().get_rate_auto(from, to, &route_info);

        if (!rate) {
            result["error"] = "No route found from " + from + " to " + to;
            result["code"] = -32000;
            return result;
        }

        // Build response with route details
        result["from"] = from;
        result["to"] = to;
        result["rate"] = *rate;
        result["effective_rate"] = route_info.effective_rate();
        result["hop_count"] = route_info.hop_count;
        result["total_fee_bps"] = route_info.total_fee_bps;
        result["slippage_bps"] = route_info.slippage_bps;
        result["route"] = route_info.description();

        // Add hop details
        din::Json hops_array(Json::arrayValue);
        for (const auto& hop : route_info.hops) {
            din::Json hop_obj;
            hop_obj["from"] = hop.from_asset;
            hop_obj["to"] = hop.to_asset;
            hop_obj["rate"] = hop.rate;
            hop_obj["fee_bps"] = hop.fee_bps;
            hop_obj["provider"] = hop.provider;
            hops_array.append(hop_obj);
        }
        result["hops"] = hops_array;
        result["rpc_schema"] = "din.bridge.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("bridge.findroute error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

din::Json bridge_routes_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        std::string from, to;
        int max_hops = 3;

        // Parse parameters
        if (params.isArray() && params.size() >= 2) {
            from = params[0].asString();
            to = params[1].asString();
            if (params.size() > 2) {
                max_hops = params[2].asInt();
            }
        } else if (params.isObject()) {
            from = params.get("from", "DIN").asString();
            to = params.get("to", "USD").asString();
            max_hops = params.get("max_hops", 3).asInt();
        } else {
            result["error"] = "Missing 'from' or 'to' parameter";
            result["code"] = -32602;
            return result;
        }

        // Get all possible routes
        auto routes = bridge::FiatBridgeManager::instance().get_all_routes(from, to, max_hops);

        // Build response
        din::Json routes_array(Json::arrayValue);
        for (const auto& route : routes) {
            din::Json route_obj;
            route_obj["route"] = route.description();
            route_obj["rate"] = route.total_rate;
            route_obj["effective_rate"] = route.effective_rate();
            route_obj["hop_count"] = route.hop_count;
            route_obj["total_fee_bps"] = route.total_fee_bps;
            route_obj["slippage_bps"] = route.slippage_bps;

            din::Json hops_array(Json::arrayValue);
            for (const auto& hop : route.hops) {
                din::Json hop_obj;
                hop_obj["from"] = hop.from_asset;
                hop_obj["to"] = hop.to_asset;
                hop_obj["rate"] = hop.rate;
                hop_obj["provider"] = hop.provider;
                hops_array.append(hop_obj);
            }
            route_obj["hops"] = hops_array;

            routes_array.append(route_obj);
        }

        result["from"] = from;
        result["to"] = to;
        result["routes"] = routes_array;
        result["count"] = static_cast<int>(routes.size());
        result["rpc_schema"] = "din.bridge.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("bridge.routes error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

din::Json getarp_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        // Optional parameters: mode ("static" | "blended"), market_rate, confidence
        std::string mode = "static";
        double market_rate = 0.0;
        double confidence = 0.0;

        if (params.isObject()) {
            mode = params.get("mode", "static").asString();
            market_rate = params.get("market_rate", 0.0).asDouble();
            confidence = params.get("confidence", 0.0).asDouble();
        } else if (params.isArray() && params.size() > 0) {
            mode = params[0].asString();
            if (params.size() > 1) market_rate = params[1].asDouble();
            if (params.size() > 2) confidence = params[2].asDouble();
        }

        auto& arp = ArpManager::instance();
        std::optional<ArpInfo> info;

        if (mode == "blended" && market_rate > 0) {
            // Blended mode: ARP + market data
            info = arp.getBlended(market_rate, confidence);
        } else {
            // Static mode: pure ARP
            info = arp.getCurrent();
        }

        if (!info) {
            result["error"] = "ARP not initialized";
            result["code"] = -32000;
            return result;
        }

        result["price_usd"] = info->price_usd;
        result["timestamp"] = info->timestamp;
        result["source"] = info->source;
        result["confidence"] = info->confidence;
        result["mode"] = mode;

        if (mode == "blended") {
            result["market_rate"] = market_rate;
            result["arp_weight"] = 1.0 - confidence;
            result["market_weight"] = confidence;
        }

        result["rpc_schema"] = "din.arp.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("getarp error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

din::Json setarp_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        double price_usd = 0.0;
        std::string source = "manual";

        if (params.isArray() && params.size() > 0) {
            price_usd = params[0].asDouble();
            if (params.size() > 1) source = params[1].asString();
        } else if (params.isObject()) {
            price_usd = params.get("price_usd", 0.0).asDouble();
            source = params.get("source", "manual").asString();
        } else {
            result["error"] = "Missing 'price_usd' parameter";
            result["code"] = -32602;
            return result;
        }

        if (price_usd <= 0) {
            result["error"] = "Price must be positive";
            result["code"] = -32602;
            return result;
        }

        ArpManager::instance().setPrice(price_usd, source);
        ArpManager::instance().saveToConfig("config/arp.json");

        auto info = ArpManager::instance().getCurrent();
        if (info) {
            result["success"] = true;
            result["price_usd"] = info->price_usd;
            result["timestamp"] = info->timestamp;
            result["source"] = info->source;
            result["message"] = "ARP updated successfully";
        }

        result["rpc_schema"] = "din.arp.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("setarp error: ") + e.what();
        result["code"] = -1;
        result["success"] = false;
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// Registration Function
// ═══════════════════════════════════════════════════════════════

void registerBridgeRPC() {
    dinero::g_logger.info("[Bridge RPC] Registering fiat bridge RPC methods...");

    // bridge.getrate
    g_rpcRegistry.registerHandler("bridge.getrate",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return bridge_getrate_impl(ctx, params);
        },
        "bridge");

    // bridge.convert
    g_rpcRegistry.registerHandler("bridge.convert",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return bridge_convert_impl(ctx, params);
        },
        "bridge");

    // bridge.providers
    g_rpcRegistry.registerHandler("bridge.providers",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return bridge_providers_impl(ctx, params);
        },
        "bridge");

    // bridge.status
    g_rpcRegistry.registerHandler("bridge.status",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return bridge_status_impl(ctx, params);
        },
        "bridge");

    // bridge.refresh
    g_rpcRegistry.registerHandler("bridge.refresh",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return bridge_refresh_impl(ctx, params);
        },
        "bridge");

    // bridge.findroute - Automatic multi-hop routing
    g_rpcRegistry.registerHandler("bridge.findroute",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return bridge_findroute_impl(ctx, params);
        },
        "bridge");

    // bridge.routes - Get all possible routes
    g_rpcRegistry.registerHandler("bridge.routes",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return bridge_routes_impl(ctx, params);
        },
        "bridge");

    // bridge.getarp - Get Anchor Reference Price (ARP)
    g_rpcRegistry.registerHandler("bridge.getarp",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return getarp_impl(ctx, params);
        },
        "bridge");

    // bridge.setarp - Set Anchor Reference Price (admin only)
    g_rpcRegistry.registerHandler("bridge.setarp",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return setarp_impl(ctx, params);
        },
        "bridge");

    dinero::g_logger.info("✅ Bridge RPC methods registered (9 methods)");

    std::cout << "[Bridge RPC] Registered methods:" << std::endl;
    std::cout << "[Bridge RPC]   - bridge.getrate    (Get exchange rate)" << std::endl;
    std::cout << "[Bridge RPC]   - bridge.convert    (Execute conversion)" << std::endl;
    std::cout << "[Bridge RPC]   - bridge.providers  (List providers)" << std::endl;
    std::cout << "[Bridge RPC]   - bridge.status     (Bridge system status)" << std::endl;
    std::cout << "[Bridge RPC]   - bridge.refresh    (Refresh cached rates)" << std::endl;
    std::cout << "[Bridge RPC]   - bridge.findroute  (Find best route with multi-hop)" << std::endl;
    std::cout << "[Bridge RPC]   - bridge.getarp     (Get Anchor Reference Price)" << std::endl;
    std::cout << "[Bridge RPC]   - bridge.setarp     (Set Anchor Reference Price)" << std::endl;
    std::cout << "[Bridge RPC]   - bridge.routes     (Get all possible routes)" << std::endl;
}

} // namespace rpc
} // namespace dinero
