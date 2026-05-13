/**
 * Discovery RPC Methods - vNext Architecture
 *
 * Full migration to RPC_METHOD DSL with complete metadata.
 * Runtime RPC method discovery and introspection.
 */

#include "rpc/rpc_method_builder.h"
#include "rpc/methods_discovery.h"
#include "common/logger.h"

namespace din {
namespace rpc {

// Implementation functions from methods_discovery.cpp
extern din::Json rpc_discover_impl(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_info_impl(const ExecutionContext& ctx, const din::Json& params);

void registerDiscoveryMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // RPC DISCOVERY & INTROSPECTION
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("rpc.discover", "rpc")
        .description("Lists all available RPC methods with metadata and parameters")
        .params({})
        .result("object", "Complete RPC method registry with descriptions, parameters, and return types")
        .handler(rpc_discover_impl)
        .examples({
            "rpc.discover"
        });

    RPC_METHOD("rpc.info", "rpc")
        .description("Returns RPC server information and capabilities")
        .params({})
        .result("object", "RPC server info including version, method count, and transport protocols")
        .handler(rpc_info_impl)
        .examples({
            "rpc.info"
        });

    dinero::g_logger.info("✅ Registered 2 discovery methods (vNext DSL)");
}

// Auto-register at program startup
static auto _discovery_vnext_init = (din::rpc::registerDiscoveryMethodsVNext(), 0);

} // namespace rpc
} // namespace din
