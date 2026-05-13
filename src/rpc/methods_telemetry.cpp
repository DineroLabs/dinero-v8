// Telemetry RPC methods migrated to vNext architecture
#include "rpc/methods_telemetry.h"
#include "rpc/rpc_registry.h"
#include "din_json.h"
#include "daemon/http_rpc_server.h"
#include "daemon/rpc/telemetry_rpc_handlers.h"
#include "p2p_manager.h"
#include "storage/chain_db.h"
#include "common/logger.h"
#include <json/json.h>

// External globals
extern RpcRegistry g_rpcRegistry;

namespace dinero {
namespace rpc {

// Wrapper functions to adapt legacy handlers to vNext

static din::Json rpc_gethealth(
    const ExecutionContext& ctx,
    const din::Json& params,
    P2PManager* p2p_manager,
    dinero::ChainDB* chain_db
) {
    // Convert din::Json to Json::Value for legacy handler
    Json::Value legacy_params;
    Json::CharReaderBuilder builder;
    std::string params_str = din::dump(params);
    std::stringstream ss(params_str);
    std::string errs;
    Json::parseFromStream(builder, ss, &legacy_params, &errs);

    auto result = dinero::daemon::rpc::gethealth_handler(legacy_params, p2p_manager, chain_db);

    // Convert Json::Value back to din::Json
    Json::StreamWriterBuilder writer;
    std::string json_str = Json::writeString(writer, result);
    return din::parse(json_str);
}

static din::Json rpc_getnodeidentity(
    const ExecutionContext& ctx,
    const din::Json& params,
    P2PManager* p2p_manager,
    uint16_t p2p_port
) {
    // Convert din::Json to Json::Value for legacy handler
    Json::Value legacy_params;
    Json::CharReaderBuilder builder;
    std::string params_str = din::dump(params);
    std::stringstream ss(params_str);
    std::string errs;
    Json::parseFromStream(builder, ss, &legacy_params, &errs);

    auto result = dinero::daemon::rpc::getnodeidentity_handler(legacy_params, p2p_manager, p2p_port);

    // Convert Json::Value back to din::Json
    Json::StreamWriterBuilder writer;
    std::string json_str = Json::writeString(writer, result);
    return din::parse(json_str);
}

static din::Json rpc_getmetrics(
    const ExecutionContext& ctx,
    const din::Json& params,
    P2PManager* p2p_manager,
    dinero::ChainDB* chain_db
) {
    // Convert din::Json to Json::Value for legacy handler
    Json::Value legacy_params;
    Json::CharReaderBuilder builder;
    std::string params_str = din::dump(params);
    std::stringstream ss(params_str);
    std::string errs;
    Json::parseFromStream(builder, ss, &legacy_params, &errs);

    auto result = dinero::daemon::rpc::getmetrics_handler(legacy_params, p2p_manager, chain_db);

    // Convert Json::Value back to din::Json
    Json::StreamWriterBuilder writer;
    std::string json_str = Json::writeString(writer, result);
    return din::parse(json_str);
}

void registerTelemetryMethods(
    HttpRpcServer* rpc_server,
    P2PManager* p2p_manager,
    dinero::ChainDB* chain_db,
    uint16_t p2p_port
) {
    RpcMethodMeta meta;

    // gethealth
    meta.name = "gethealth";
    meta.description = "Returns node health status for monitoring (Prometheus, Grafana, etc.)";
    meta.result.type = "object";
    meta.result.desc = "Health status: ok (healthy), degraded (issues), error (critical)";
    g_rpcRegistry.registerHandler("server.health",
        [p2p_manager, chain_db](const ExecutionContext& ctx, const din::Json& params) {
            return rpc_gethealth(ctx, params, p2p_manager, chain_db);
        }, meta, "telemetry");

    // getnodeidentity
    meta = RpcMethodMeta();
    meta.name = "getnodeidentity";
    meta.description = "Returns node identity info for cross-region debugging";
    meta.result.type = "object";
    meta.result.desc = "Node identity including region, version, and P2P info";
    g_rpcRegistry.registerHandler("server.getnodeidentity",
        [p2p_manager, p2p_port](const ExecutionContext& ctx, const din::Json& params) {
            return rpc_getnodeidentity(ctx, params, p2p_manager, p2p_port);
        }, meta, "telemetry");

    // getmetrics
    meta = RpcMethodMeta();
    meta.name = "getmetrics";
    meta.description = "Returns metrics in Prometheus text format for scraping";
    meta.result.type = "object";
    meta.result.desc = "Prometheus/OpenMetrics compatible metrics";
    g_rpcRegistry.registerHandler("telemetry.getmetrics",
        [p2p_manager, chain_db](const ExecutionContext& ctx, const din::Json& params) {
            return rpc_getmetrics(ctx, params, p2p_manager, chain_db);
        }, meta, "telemetry");

    dinero::g_logger.info("Registered 3 telemetry RPC methods in vNext");
}

} // namespace rpc
} // namespace dinero
