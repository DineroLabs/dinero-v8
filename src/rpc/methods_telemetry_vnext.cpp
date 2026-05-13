/**
 * Telemetry RPC Methods - vNext Architecture
 *
 * Full migration to RPC_METHOD DSL with complete metadata.
 * Node telemetry, performance metrics, and health monitoring.
 */

#include "rpc/rpc_method_builder.h"
#include "rpc/methods_telemetry.h"
#include "common/logger.h"

namespace din {
namespace rpc {

// Implementation functions from methods_telemetry.cpp
extern din::Json rpc_gethealth(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_getmetrics(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_getnodeidentity(const ExecutionContext& ctx, const din::Json& params);

void registerTelemetryMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // NODE TELEMETRY & HEALTH
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("server.health", "telemetry")
        .description("Returns health check status of the node and its subsystems")
        .params({})
        .result("object", "Health status including blockchain, network, wallet, and mempool states")
        .handler(rpc_gethealth)
        .examples({
            "gethealth"
        });

    RPC_METHOD("telemetry.getmetrics", "telemetry")
        .description("Returns performance metrics for the node")
        .params({})
        .result("object", "Metrics including uptime, memory usage, CPU, blocks processed, and tx counts")
        .handler(rpc_getmetrics)
        .examples({
            "getmetrics"
        });

    RPC_METHOD("server.getnodeidentity", "telemetry")
        .description("Returns the node's identity information")
        .params({})
        .result("object", "Node identity including version, network, and unique identifier")
        .handler(rpc_getnodeidentity)
        .examples({
            "getnodeidentity"
        });

    dinero::g_logger.info("✅ Registered 3 telemetry methods (vNext DSL)");
}

// Auto-register at program startup
static auto _telemetry_vnext_init = (din::rpc::registerTelemetryMethodsVNext(), 0);

} // namespace rpc
} // namespace din
