#pragma once

#include <json/json.h>
#include <memory>

// Forward declarations
class P2PManager;
namespace dinero { class ChainDB; }

// ============================================================================
// PHASE D: Telemetry RPC Handlers
// ============================================================================
// Production-ready monitoring and diagnostics RPCs:
// - gethealth: Node health status for external monitoring
// - getminerstats: Mining performance telemetry
// - getnodeidentity: Node identification for cross-region debugging
//
// All handlers return real, live data (no stubs/fake values)
// ============================================================================

// ============================================================================
// PHASE E: Metrics Export (Prometheus/OpenMetrics)
// ============================================================================
// - getmetrics: Returns metrics in Prometheus text format for scraping
// ============================================================================

namespace dinero {
namespace daemon {
namespace rpc {

// Health check RPC - Returns node status for monitoring systems
// Example: curl http://localhost:20998/json_rpc -d '{"method":"gethealth"}'
Json::Value gethealth_handler(
    const Json::Value& params,
    P2PManager* p2p_manager,
    dinero::ChainDB* chain_db
);

// Mining statistics RPC - Returns mining performance metrics
// Example: ./dinero-cli getminerstats
Json::Value getminerstats_handler(
    const Json::Value& params
);

// Node identity RPC - Returns node identification for diagnostics
// Example: ./dinero-cli getnodeidentity
Json::Value getnodeidentity_handler(
    const Json::Value& params,
    P2PManager* p2p_manager,
    uint16_t p2p_port
);

// Prometheus/OpenMetrics exporter - Returns metrics in text format
// Example: ./dinero-cli getmetrics
Json::Value getmetrics_handler(
    const Json::Value& params,
    P2PManager* p2p_manager,
    dinero::ChainDB* chain_db
);

// Genesis verification summary - Returns structured JSON
// Example: ./dinero-cli getverificationsummary
Json::Value getverificationsummary_handler(
    const Json::Value& params
);

} // namespace rpc
} // namespace daemon
} // namespace dinero
