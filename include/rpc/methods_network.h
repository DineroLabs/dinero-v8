#pragma once

#include "http_rpc_server.h"
#include "p2p_manager.h"
#include "ws/ws_server.h"
#include <json/json.h>
#include <memory>
#include <functional>

namespace dinero {
namespace rpc {

// ═══════════════════════════════════════════════════════════
// Network Configuration Helper Struct
// ═══════════════════════════════════════════════════════════

struct NetworkConfig {
    std::string rpc_bind;
    int rpc_port;
    int p2p_port;
    bool regtest;
    bool testnet;
};

// ═══════════════════════════════════════════════════════════
// Network RPC Method Registrations
// ═══════════════════════════════════════════════════════════

/**
 * Register all network/P2P-related RPC methods with the HTTP RPC server.
 *
 * Methods registered:
 * - getnetworkinfo: Get network status and connection information
 * - getserverinfo: Get server configuration and endpoint information
 * - getpeerinfo: Get detailed information about connected peers
 * - addnode: Add/remove/connect to a specific node
 * - getconnectioncount: Get number of connections to other nodes
 *
 * @param server RPC server to register methods with
 * @param p2p_manager P2P network manager
 * @param config Network configuration (ports, network type)
 * @param ws_server WebSocket server (for getserverinfo endpoint discovery)
 * @param get_uptime_callback Callback to get daemon uptime in seconds
 */
void registerNetworkMethods(
    HttpRpcServer* server,
    P2PManager* p2p_manager,
    const NetworkConfig& config,
    WsServer* ws_server,
    std::function<int64_t()> get_uptime_callback
);

/**
 * Register all network/P2P-related RPC methods with vNext architecture (global g_rpcRegistry).
 *
 * Methods registered:
 * - getnetworkinfo: Get network status and connection information
 * - getserverinfo: Get server configuration and endpoint information
 * - getpeerinfo: Get detailed information about connected peers
 * - addnode: Add/remove/connect to a specific node
 * - getconnectioncount: Get number of connections to other nodes
 *
 * @param p2p_manager P2P network manager
 * @param config Network configuration (ports, network type)
 * @param ws_server WebSocket server (for getserverinfo endpoint discovery)
 * @param get_uptime_callback Callback to get daemon uptime in seconds
 */
void registerNetworkMethodsVNext(
    P2PManager* p2p_manager,
    const NetworkConfig& config,
    WsServer* ws_server,
    std::function<int64_t()> get_uptime_callback
);

} // namespace rpc
} // namespace dinero

// vNext registration in din namespace (shorter alias)
namespace din {
namespace rpc {
    void registerNetworkMethodsVNext(
        P2PManager* p2p_manager,
        const dinero::rpc::NetworkConfig& config,
        dinero::WsServer* ws_server,
        std::function<int64_t()> get_uptime_callback
    );
} // namespace rpc
} // namespace din
