/**
 * Multiasset RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file migrates multiasset RPC methods to context-aware pattern.
 * Note: Multiasset uses global singletons (BridgedEscrowManager, MultiAssetContractRegistry)
 * which is acceptable for now as they're designed as application-wide registries.
 *
 * PATTERN:
 *   These RPC handlers delegate to the existing multiasset functions which use
 *   global registries. This is a minimal migration to make them "context-aware"
 *   in the sense that they receive ExecutionContext, even though they don't
 *   currently use services from it.
 *
 * Future enhancement: If multiasset becomes a DaemonContext service, update these handlers.
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "rpc/methods_multiasset.h"  // Include the existing implementations
#include "common/logger.h"

// Forward declarations from methods_multiasset.cpp (in dinero::rpc namespace)
namespace dinero {
namespace rpc {
    extern din::Json multiasset_createescrow(const din::Json& params);
    extern din::Json multiasset_releaseescrow(const din::Json& params);
    extern din::Json multiasset_refundescrow(const din::Json& params);
    extern din::Json multiasset_getcontract(const din::Json& params);
    extern din::Json multiasset_listcontracts(const din::Json& params);
    extern din::Json multiasset_getconversionroutes(const din::Json& params);
    extern din::Json multiasset_estimateconversion(const din::Json& params);
    extern din::Json multiasset_stats(const din::Json& params);
    extern din::Json multiasset_supportedassets(const din::Json& params);
}
}

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE MULTIASSET RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

/**
 * All multiasset methods delegate to existing implementations.
 * This makes them "context-aware" (receive ExecutionContext) while
 * still using the global registries internally.
 */

din::Json rpc_context_multiasset_createescrow(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::multiasset_createescrow(params);
}

din::Json rpc_context_multiasset_releaseescrow(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::multiasset_releaseescrow(params);
}

din::Json rpc_context_multiasset_refundescrow(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::multiasset_refundescrow(params);
}

din::Json rpc_context_multiasset_getcontract(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::multiasset_getcontract(params);
}

din::Json rpc_context_multiasset_listcontracts(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::multiasset_listcontracts(params);
}

din::Json rpc_context_multiasset_getconversionroutes(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::multiasset_getconversionroutes(params);
}

din::Json rpc_context_multiasset_estimateconversion(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::multiasset_estimateconversion(params);
}

din::Json rpc_context_multiasset_stats(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::multiasset_stats(params);
}

din::Json rpc_context_multiasset_supportedassets(const ExecutionContext& ctx, const din::Json& params) {
    return dinero::rpc::multiasset_supportedassets(params);
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

void registerMultiassetMethodsContext() {
    extern RpcRegistry g_rpcRegistry;

    g_rpcRegistry.registerHandler("multiasset.createescrow",
                                 rpc_context_multiasset_createescrow,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("multiasset.releaseescrow",
                                 rpc_context_multiasset_releaseescrow,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("multiasset.refundescrow",
                                 rpc_context_multiasset_refundescrow,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("multiasset.getcontract",
                                 rpc_context_multiasset_getcontract,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("multiasset.listcontracts",
                                 rpc_context_multiasset_listcontracts,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("multiasset.getconversionroutes",
                                 rpc_context_multiasset_getconversionroutes,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("multiasset.estimateconversion",
                                 rpc_context_multiasset_estimateconversion,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("multiasset.stats",
                                 rpc_context_multiasset_stats,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("multiasset.supportedassets",
                                 rpc_context_multiasset_supportedassets,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    dinero::g_logger.info("[RPC Context] Registered 9 multiasset context-aware methods");
}
