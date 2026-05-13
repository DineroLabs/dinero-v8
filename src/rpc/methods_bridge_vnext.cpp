/**
 * Bridge RPC Methods - vNext Architecture
 *
 * Fiat bridge and cross-chain conversion methods with full metadata.
 */

#include "rpc/rpc_method_builder.h"
#include "bridge/fiat_bridge_manager.h"
#include "common/logger.h"
#include <iostream>

namespace din {
namespace rpc {

// Implementation functions from methods_bridge.cpp
extern din::Json bridge_getrate_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json bridge_convert_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json bridge_providers_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json bridge_status_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json bridge_refresh_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json bridge_findroute_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json bridge_routes_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json getarp_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json setarp_impl(const ExecutionContext& ctx, const din::Json& params);

void registerBridgeMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // FIAT BRIDGE CORE METHODS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("bridge.getrate", "bridge")
        .description("Gets the current exchange rate between DIN and a fiat/crypto currency")
        .param("from", "string", "Source currency (e.g., DIN)", true)
        .param("to", "string", "Target currency (e.g., USD, BTC)", true)
        .param("provider", "string", "Bridge provider (optional, uses best if not specified)", false)
        .result("object", "Exchange rate data with price, provider, and timestamp")
        .handler(bridge_getrate_impl)
        .examples({
            "bridge.getrate DIN USD",
            "bridge.getrate DIN BTC",
            "bridge.getrate DIN EUR custodial"
        });

    RPC_METHOD("bridge.convert", "bridge")
        .description("Converts DIN to fiat/crypto or vice versa through bridge providers")
        .param("amount", "number", "Amount to convert", true)
        .param("from", "string", "Source currency", true)
        .param("to", "string", "Target currency", true)
        .param("provider", "string", "Bridge provider (optional)", false)
        .result("object", "Conversion result with amount, rate, and transaction details")
        .handler(bridge_convert_impl)
        .examples({
            "bridge.convert 100 DIN USD",
            "bridge.convert 50 USD DIN dex",
            "bridge.convert 1.5 DIN BTC"
        });

    RPC_METHOD("bridge.providers", "bridge")
        .description("Lists all available bridge providers and their capabilities")
        .params({})
        .result("object", "Array of provider info with names, supported currencies, and fees")
        .handler(bridge_providers_impl)
        .examples({
            "bridge.providers"
        });

    RPC_METHOD("bridge.status", "bridge")
        .description("Returns bridge system status and health")
        .params({})
        .result("object", "Bridge status including active providers and recent activity")
        .handler(bridge_status_impl)
        .examples({
            "bridge.status"
        });

    RPC_METHOD("bridge.refresh", "bridge")
        .description("Refreshes exchange rates from all bridge providers")
        .params({})
        .result("object", "Refresh status with updated rates")
        .handler(bridge_refresh_impl)
        .examples({
            "bridge.refresh"
        });

    RPC_METHOD("bridge.findroute", "bridge")
        .description("Finds optimal conversion route between currencies")
        .param("from", "string", "Source currency", true)
        .param("to", "string", "Target currency", true)
        .param("amount", "number", "Amount to convert (optional)", false)
        .result("object", "Optimal route with providers, rates, and estimated fees")
        .handler(bridge_findroute_impl)
        .examples({
            "bridge.findroute DIN USD",
            "bridge.findroute DIN BTC 100"
        });

    RPC_METHOD("bridge.routes", "bridge")
        .description("Lists all available conversion routes")
        .params({})
        .result("array", "Array of available routes with currency pairs")
        .handler(bridge_routes_impl)
        .examples({
            "bridge.routes"
        });

    // ═══════════════════════════════════════════════════════════════
    // ARP (ALGORITHMIC REFERENCE PRICE) METHODS
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("bridge.getarp", "bridge")
        .description("Gets Algorithmic Reference Price for DIN")
        .param("mode", "string", "Calculation mode: market, arp, or blended (default: blended)", false)
        .param("confidence", "number", "Market confidence factor 0.0-1.0 (optional)", false)
        .result("object", "ARP data with price, components, and confidence metrics")
        .handler(getarp_impl)
        .examples({
            "bridge.getarp",
            "bridge.getarp market",
            "bridge.getarp blended 0.8"
        });

    RPC_METHOD("bridge.setarp", "bridge")
        .description("Sets ARP calculation parameters (admin only)")
        .param("config", "object", "ARP configuration parameters", true)
        .result("object", "Updated ARP configuration")
        .handler(setarp_impl)
        .examples({
            "bridge.setarp '{\"mode\":\"blended\",\"weights\":{\"market\":0.7,\"arp\":0.3}}'"
        });

    std::cout << "[Bridge RPC vNext] ✅ Registered 9 bridge methods with full metadata" << std::endl;
}

} // namespace rpc
} // namespace din

// Auto-register at startup
static auto _bridge_vnext_init = (din::rpc::registerBridgeMethodsVNext(), 0);
