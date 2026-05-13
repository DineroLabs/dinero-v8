// Phase 5C: Address Manager RPC Methods
// Architecture V3: Context-aware handlers for peer address management

#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/address_manager_service.h"
#include "p2p/addrman.h"
#include "common/logger.h"
#include "din_json.h"

namespace dinero {
namespace rpc {

using din::Json;
using ::ExecutionContext;

// addrman.stats - Get address manager statistics
static Json addrman_stats_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->address_manager) {
        throw std::runtime_error("AddressManagerService not available");
    }

    auto manager = daemon_ctx->address_manager->getManager();
    if (!manager) {
        throw std::runtime_error("AddressManager not initialized");
    }

    auto stats = manager->getStats();
    Json result = din::obj();
    result["total_addresses"] = static_cast<Json::UInt64>(stats.total_addresses);
    result["new_addresses"] = static_cast<Json::UInt64>(stats.new_addresses);
    result["tried_addresses"] = static_cast<Json::UInt64>(stats.tried_addresses);
    result["terrible_addresses"] = static_cast<Json::UInt64>(stats.terrible_addresses);
    result["banned_addresses"] = static_cast<Json::UInt64>(stats.banned_addresses);
    result["avg_success_rate"] = stats.avg_success_rate;

    return result;
}

// addrman.status - Get address manager status
static Json addrman_status_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->address_manager) {
        throw std::runtime_error("AddressManagerService not available");
    }

    Json result = din::obj();
    result["enabled"] = true;
    result["algorithm"] = "Bitcoin addrman";
    result["description"] = "Peer discovery and intelligent selection";

    return result;
}

// addrman.maintenance - Trigger maintenance operations
static Json addrman_maintenance_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->address_manager) {
        throw std::runtime_error("AddressManagerService not available");
    }

    auto manager = daemon_ctx->address_manager->getManager();
    if (!manager) {
        throw std::runtime_error("AddressManager not initialized");
    }

    manager->performMaintenance();

    Json result = din::obj();
    result["success"] = true;
    result["message"] = "Address manager maintenance completed";

    return result;
}

// Registration function called from rpc_context_wiring.cpp
void register_address_manager_methods() {
    // Register address manager RPC methods in global registry
    g_rpcRegistry.registerHandler("addrman.stats",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return addrman_stats_impl(ctx, params);
        },
        "addrman");

    g_rpcRegistry.registerHandler("addrman.status",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return addrman_status_impl(ctx, params);
        },
        "addrman");

    g_rpcRegistry.registerHandler("addrman.maintenance",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return addrman_maintenance_impl(ctx, params);
        },
        "addrman");
}

} // namespace rpc
} // namespace dinero
