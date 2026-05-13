/**
 * Bridge RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file migrates bridge RPC methods to context-aware pattern.
 * Bridge methods use FiatBridgeManager and ArpManager singletons, which is
 * acceptable as they're application-wide registries.
 *
 * PATTERN:
 *   These RPC handlers delegate to existing bridge functions which already
 *   accept ExecutionContext. Simple passthrough to integrate with context system.
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "common/logger.h"

// Forward declarations from methods_bridge.cpp
namespace dinero {
namespace rpc {
    extern din::Json bridge_getrate_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json bridge_providers_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json bridge_status_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json bridge_refresh_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json bridge_routes_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json bridge_findroute_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json bridge_convert_impl(const ExecutionContext& ctx, const din::Json& params);
}
}

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE BRIDGE RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

din::Json rpc_context_bridge_getrate(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::bridge_getrate_impl(ctx, params);
}

din::Json rpc_context_bridge_providers(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::bridge_providers_impl(ctx, params);
}

din::Json rpc_context_bridge_status(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::bridge_status_impl(ctx, params);
}

din::Json rpc_context_bridge_refresh(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::bridge_refresh_impl(ctx, params);
}

din::Json rpc_context_bridge_routes(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::bridge_routes_impl(ctx, params);
}

din::Json rpc_context_bridge_findroute(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::bridge_findroute_impl(ctx, params);
}

din::Json rpc_context_bridge_convert(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::bridge_convert_impl(ctx, params);
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

void registerBridgeMethodsContext() {
    extern RpcRegistry g_rpcRegistry;

    g_rpcRegistry.registerHandler("bridge.getrate",
                                 rpc_context_bridge_getrate,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("bridge.providers",
                                 rpc_context_bridge_providers,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("bridge.status",
                                 rpc_context_bridge_status,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("bridge.refresh",
                                 rpc_context_bridge_refresh,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("bridge.routes",
                                 rpc_context_bridge_routes,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("bridge.findroute",
                                 rpc_context_bridge_findroute,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("bridge.convert",
                                 rpc_context_bridge_convert,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    dinero::g_logger.info("[RPC Context] Registered 7 bridge context-aware methods");
}
