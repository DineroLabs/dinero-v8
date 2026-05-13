/**
 * Discovery RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file migrates discovery RPC methods to context-aware pattern.
 * Discovery methods (rpc.discover, rpc.info) introspect the RPC registry.
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "common/logger.h"

// Forward declarations from methods_discovery.cpp
namespace din {
namespace rpc {
    extern din::Json rpc_discover_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json rpc_info_impl(const ExecutionContext& ctx, const din::Json& params);
}
}

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE DISCOVERY RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

din::Json rpc_context_rpc_discover(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::rpc_discover_impl(ctx, params);
}

din::Json rpc_context_rpc_info(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::rpc_info_impl(ctx, params);
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

void registerDiscoveryMethodsContext() {
    extern RpcRegistry g_rpcRegistry;

    g_rpcRegistry.registerHandler("rpc.discover",
                                 rpc_context_rpc_discover,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("rpc.info",
                                 rpc_context_rpc_info,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    dinero::g_logger.info("[RPC Context] Registered 2 discovery context-aware methods");
}
