#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/headers_sync_service.h"
#include "common/logger.h"
#include "din_json.h"

namespace dinero {
namespace rpc {

using din::Json;
using ::ExecutionContext;

/**
 * sync.status - Get headers-first synchronization status
 *
 * Returns current sync state, headers count, blocks downloaded, and progress.
 */
static Json sync_status_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->headers_sync) {
        throw std::runtime_error("Headers sync service not available");
    }

    return daemon_ctx->headers_sync->getStatus();
}

/**
 * sync.metrics - Get headers-first synchronization metrics
 *
 * Returns detailed metrics including total headers received, blocks requested,
 * blocks downloaded, and sync duration.
 */
static Json sync_metrics_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->headers_sync) {
        throw std::runtime_error("Headers sync service not available");
    }

    return daemon_ctx->headers_sync->getMetrics();
}

/**
 * sync.start - Start headers-first synchronization
 *
 * Parameters:
 *   [0] peer_id (string, required): Peer ID to sync from
 */
static Json sync_start_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->headers_sync) {
        throw std::runtime_error("Headers sync service not available");
    }

    if (params.size() < 1) {
        throw std::invalid_argument("sync.start requires peer_id parameter");
    }

    std::string peer_id = params[0].asString();
    daemon_ctx->headers_sync->startSync(peer_id);

    Json result = din::obj();
    result["success"] = true;
    result["message"] = "Headers-first sync started";
    result["peer_id"] = peer_id;
    return result;
}

/**
 * sync.stop - Stop headers-first synchronization
 */
static Json sync_stop_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->headers_sync) {
        throw std::runtime_error("Headers sync service not available");
    }

    daemon_ctx->headers_sync->stopSync();

    Json result = din::obj();
    result["success"] = true;
    result["message"] = "Headers-first sync stopped";
    return result;
}

/**
 * sync.configure - Configure headers-first sync settings
 *
 * Parameters:
 *   [0] config (object): Configuration options
 *       - max_headers_per_request (uint): Maximum headers per request (default: 2000)
 *       - validation_enabled (bool): Enable header validation (default: true)
 *       - timeout_seconds (uint): Sync timeout in seconds (default: 30)
 */
static Json sync_configure_impl(const ExecutionContext& ctx, const Json& params) {
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->headers_sync) {
        throw std::runtime_error("Headers sync service not available");
    }

    if (params.size() < 1 || !params[0].isObject()) {
        throw std::invalid_argument("sync.configure requires config object parameter");
    }

    const Json& config = params[0];

    if (config.isMember("max_headers_per_request")) {
        uint32_t max_headers = config["max_headers_per_request"].asUInt();
        daemon_ctx->headers_sync->setMaxHeadersPerRequest(max_headers);
    }

    if (config.isMember("validation_enabled")) {
        bool enabled = config["validation_enabled"].asBool();
        daemon_ctx->headers_sync->setValidationEnabled(enabled);
    }

    if (config.isMember("timeout_seconds")) {
        uint32_t timeout = config["timeout_seconds"].asUInt();
        daemon_ctx->headers_sync->setTimeout(std::chrono::seconds(timeout));
    }

    Json result = din::obj();
    result["success"] = true;
    result["message"] = "Headers-first sync configured";
    return result;
}

/**
 * Register headers-first sync methods in the RPC registry
 */
void register_headers_sync_methods() {
    g_rpcRegistry.registerHandler("sync.status",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return sync_status_impl(ctx, params);
        },
        "sync");

    g_rpcRegistry.registerHandler("sync.metrics",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return sync_metrics_impl(ctx, params);
        },
        "sync");

    g_rpcRegistry.registerHandler("sync.start",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return sync_start_impl(ctx, params);
        },
        "sync");

    g_rpcRegistry.registerHandler("sync.stop",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return sync_stop_impl(ctx, params);
        },
        "sync");

    g_rpcRegistry.registerHandler("sync.configure",
        [](const ExecutionContext& ctx, const Json& params) -> Json {
            return sync_configure_impl(ctx, params);
        },
        "sync");
}

} // namespace rpc
} // namespace dinero
