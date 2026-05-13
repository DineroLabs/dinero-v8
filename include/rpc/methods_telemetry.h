#pragma once

#include <memory>
#include <cstdint>

// Forward declarations
class HttpRpcServer;
class P2PManager;
namespace dinero {
    class ChainDB;
    struct Config;
}

namespace dinero {
namespace rpc {

/**
 * Register telemetry RPC methods (gethealth, getnodeidentity, getmetrics)
 *
 * Methods registered:
 *  - gethealth: Node health status for external monitoring
 *  - getnodeidentity: Node identification for diagnostics
 *  - getmetrics: Prometheus/OpenMetrics format metrics export
 *
 * @param rpc_server The RPC server instance to register methods on
 * @param p2p_manager The P2P manager for peer information
 * @param chain_db The chain database for blockchain data
 * @param p2p_port The P2P port number for node identification
 */
void registerTelemetryMethods(
    HttpRpcServer* rpc_server,
    P2PManager* p2p_manager,
    dinero::ChainDB* chain_db,
    uint16_t p2p_port
);

} // namespace rpc
} // namespace dinero
