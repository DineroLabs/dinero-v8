/**
 * Market RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file migrates market RPC methods to context-aware pattern.
 * The market methods are already implemented with ExecutionContext signatures
 * in methods_market.cpp, so we simply delegate to them.
 *
 * PATTERN:
 *   These RPC handlers delegate to existing market functions which already
 *   accept ExecutionContext. This is a straightforward wrapper to integrate
 *   them into the context-aware RPC system.
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "common/logger.h"

// Forward declarations from methods_market.cpp
namespace din {
namespace rpc {
    extern din::Json market_createoffer_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json market_canceloffer_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json market_updateoffer_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json market_listoffers_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json market_getoffer_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json market_search_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json market_acceptoffer_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json market_completetrade_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json market_disputetrade_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json market_getreputation_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json market_myoffers_impl(const ExecutionContext& ctx, const din::Json& params);
    extern din::Json market_mytrades_impl(const ExecutionContext& ctx, const din::Json& params);
}
}

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE MARKET RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

/**
 * All market methods delegate to existing implementations.
 * These already use ExecutionContext, so this is a direct passthrough.
 */

din::Json rpc_context_market_createoffer(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::market_createoffer_impl(ctx, params);
}

din::Json rpc_context_market_canceloffer(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::market_canceloffer_impl(ctx, params);
}

din::Json rpc_context_market_updateoffer(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::market_updateoffer_impl(ctx, params);
}

din::Json rpc_context_market_listoffers(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::market_listoffers_impl(ctx, params);
}

din::Json rpc_context_market_getoffer(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::market_getoffer_impl(ctx, params);
}

din::Json rpc_context_market_search(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::market_search_impl(ctx, params);
}

din::Json rpc_context_market_acceptoffer(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::market_acceptoffer_impl(ctx, params);
}

din::Json rpc_context_market_completetrade(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::market_completetrade_impl(ctx, params);
}

din::Json rpc_context_market_disputetrade(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::market_disputetrade_impl(ctx, params);
}

din::Json rpc_context_market_getreputation(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::market_getreputation_impl(ctx, params);
}

din::Json rpc_context_market_myoffers(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::market_myoffers_impl(ctx, params);
}

din::Json rpc_context_market_mytrades(const ExecutionContext& ctx, const din::Json& params) {
    return din::rpc::market_mytrades_impl(ctx, params);
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

void registerMarketMethodsContext() {
    extern RpcRegistry g_rpcRegistry;

    g_rpcRegistry.registerHandler("market.createoffer",
                                 rpc_context_market_createoffer,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("market.canceloffer",
                                 rpc_context_market_canceloffer,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("market.updateoffer",
                                 rpc_context_market_updateoffer,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("market.listoffers",
                                 rpc_context_market_listoffers,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("market.getoffer",
                                 rpc_context_market_getoffer,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("market.search",
                                 rpc_context_market_search,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("market.acceptoffer",
                                 rpc_context_market_acceptoffer,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("market.completetrade",
                                 rpc_context_market_completetrade,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("market.disputetrade",
                                 rpc_context_market_disputetrade,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("market.getreputation",
                                 rpc_context_market_getreputation,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("market.myoffers",
                                 rpc_context_market_myoffers,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("market.mytrades",
                                 rpc_context_market_mytrades,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    dinero::g_logger.info("[RPC Context] Registered 12 market context-aware methods");
}
