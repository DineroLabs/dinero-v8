// Phase 5B: Compact Block Relay RPC Methods (BIP152)
// Architecture V3: Context-aware handlers for compact block relay monitoring
//
// Phase Plan-A: Binary Wire Format Migration
// - Removed JSON-based CompactBlockManager dependency
// - Stats now sourced from CompactBlockService (which delegates to BlockRelayManager)

#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/compact_block_service.h"
#include "common/logger.h"
#include "din_json.h"

namespace dinero {
namespace rpc {

using din::Json;
using ::ExecutionContext;

// compactblocks.stats - Get compact block relay statistics
static Json compactblocks_stats_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->compact_blocks) {
        throw std::runtime_error("CompactBlockService not available");
    }

    auto service = daemon_ctx->compact_blocks;

    Json result = din::obj();
    result["blocks_processed"] = static_cast<Json::UInt64>(service->getBlocksProcessed());
    result["reconstruction_rate"] = service->getReconstructionRate();
    result["bandwidth_saved"] = static_cast<Json::UInt64>(service->getBandwidthSaved());

    return result;
}

// compactblocks.status - Get compact block relay status
static Json compactblocks_status_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->compact_blocks) {
        throw std::runtime_error("CompactBlockService not available");
    }

    auto service = daemon_ctx->compact_blocks;

    Json result = din::obj();
    result["enabled"] = true;
    result["protocol"] = "BIP152";
    result["wire_format"] = "binary";  // Plan-A: Binary wire format
    result["short_id_length"] = 6;  // BIP152 uses 6-byte short IDs
    result["blocks_processed"] = static_cast<Json::UInt64>(service->getBlocksProcessed());
    result["reconstruction_rate"] = service->getReconstructionRate();

    return result;
}

// compactblocks.configure - Configure compact block settings
static Json compactblocks_configure_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->compact_blocks) {
        throw std::runtime_error("CompactBlockService not available");
    }

    Json result = din::obj();
    result["success"] = true;
    result["applied"] = false;
    result["mode"] = "static";
    result["message"] = "Compact block relay has no runtime configuration in this build";
    if (!params.isNull() && !params.empty()) {
        result["warning"] = "Parameters were ignored";
    }

    return result;
}

// Registration function called from rpc_context_wiring.cpp
void register_compact_blocks_methods() {
    // Register compact block relay RPC methods in global registry
    g_rpcRegistry.registerHandler("compactblocks.stats",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return compactblocks_stats_impl(ctx, params);
        },
        "compactblocks");

    g_rpcRegistry.registerHandler("compactblocks.status",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return compactblocks_status_impl(ctx, params);
        },
        "compactblocks");

    g_rpcRegistry.registerHandler("compactblocks.configure",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return compactblocks_configure_impl(ctx, params);
        },
        "compactblocks");
}

} // namespace rpc
} // namespace dinero
