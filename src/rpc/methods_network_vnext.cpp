/**
 * Network RPC Methods - vNext Architecture
 *
 * Full migration to RPC_METHOD DSL with complete metadata.
 * Network status, peer management, and connection information.
 */

#include "rpc/rpc_method_builder.h"
#include "rpc/methods_network.h"
#include "common/logger.h"
#include <iostream>

namespace din {
namespace rpc {

// Implementation functions from methods_network.cpp (with P2PManager* parameter)
// These need to be wrapped in lambdas that capture P2PManager

void registerNetworkMethodsVNext(
    P2PManager* p2p_manager,
    const dinero::rpc::NetworkConfig& config,
    dinero::WsServer* ws_server,
    std::function<int64_t()> get_uptime_callback
) {
    // Declare implementation functions (defined in methods_network.cpp)
    extern din::Json rpc_getnetworkinfo(
        const ExecutionContext& ctx,
        const din::Json& params,
        P2PManager* p2p_manager
    );
    extern din::Json rpc_getserverinfo(
        const ExecutionContext& ctx,
        const din::Json& params,
        const dinero::rpc::NetworkConfig& config,
        dinero::WsServer* ws_server,
        P2PManager* p2p_manager,
        std::function<int64_t()> get_uptime_callback
    );
    extern din::Json rpc_getpeerinfo(
        const ExecutionContext& ctx,
        const din::Json& params,
        P2PManager* p2p_manager
    );
    extern din::Json rpc_addnode(
        const ExecutionContext& ctx,
        const din::Json& params,
        P2PManager* p2p_manager
    );
    extern din::Json rpc_getconnectioncount(
        const ExecutionContext& ctx,
        const din::Json& params,
        P2PManager* p2p_manager
    );

    // ═══════════════════════════════════════════════════════════════
    // NETWORK STATUS & MANAGEMENT
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("network.getinfo", "network")
        .description("Returns network status and connection information")
        .params({})
        .result("object", "Network information including connections, protocol version, and network activity")
        .handler([p2p_manager](const ExecutionContext& ctx, const din::Json& params) {
            return rpc_getnetworkinfo(ctx, params, p2p_manager);
        })
        .examples({
            "getnetworkinfo"
        });

    RPC_METHOD("server.getinfo", "network")
        .description("Returns server runtime information and configuration")
        .params({})
        .result("object", "Server info including uptime, version, and WebSocket status")
        .handler([config, ws_server, p2p_manager, get_uptime_callback](const ExecutionContext& ctx, const din::Json& params) {
            return rpc_getserverinfo(ctx, params, config, ws_server, p2p_manager, get_uptime_callback);
        })
        .examples({
            "getserverinfo"
        });

    RPC_METHOD("network.getpeerinfo", "network")
        .description("Returns data about each connected network peer")
        .params({})
        .result("array", "Array of peer objects with connection details and statistics")
        .handler([p2p_manager](const ExecutionContext& ctx, const din::Json& params) {
            return rpc_getpeerinfo(ctx, params, p2p_manager);
        })
        .examples({
            "getpeerinfo"
        });

    RPC_METHOD("network.addnode", "network")
        .description("Add, remove, or attempt connection to a specific node")
        .param("node", "string", "Node address (host:port)", true)
        .param("command", "string", "Command: add, remove, or onetry", true)
        .result("object", "Operation result")
        .handler([p2p_manager](const ExecutionContext& ctx, const din::Json& params) {
            return rpc_addnode(ctx, params, p2p_manager);
        })
        .examples({
            "addnode \"192.168.1.100:20999\" \"add\"",
            "addnode \"seed.dinero-coin.com:20999\" \"onetry\""
        });

    RPC_METHOD("network.getconnectioncount", "network")
        .description("Returns the number of active peer connections")
        .params({})
        .result("number", "Number of active connections")
        .handler([p2p_manager](const ExecutionContext& ctx, const din::Json& params) {
            return rpc_getconnectioncount(ctx, params, p2p_manager);
        })
        .examples({
            "getconnectioncount"
        });

    dinero::g_logger.info("✅ Registered 5 network methods (vNext DSL)");
}

// Auto-register at program startup (requires calling from main with parameters)
// Note: Cannot use static initializer here due to required parameters

} // namespace rpc
} // namespace din
