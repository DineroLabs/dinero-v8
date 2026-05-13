#include "telemetry_rpc_handlers.h"
#include "daemon_globals.h"
#include "p2p_manager.h"
#include "storage/chain_db.h"
#include "consensus/chainparams.h"
#include <sstream>
#include <iomanip>
#include <ctime>

namespace dinero {
namespace daemon {
namespace rpc {

// ============================================================================
// gethealth - Node Health Check
// ============================================================================
// Returns current node status for external monitoring (Prometheus, Grafana, etc.)
// Status: ok (healthy), degraded (issues detected), error (critical failure)
// ============================================================================

Json::Value gethealth_handler(
    const Json::Value& params,
    P2PManager* p2p_manager,
    dinero::ChainDB* chain_db
) {
    Json::Value result;

    // Default to healthy
    result["status"] = "ok";

    // Get current block height
    try {
        auto tip_result = chain_db->getTip();
        if (tip_result.status() == dinero::Status::Ok) {
            result["height"] = static_cast<Json::Value::Int>(tip_result.value().height);
        } else {
            result["height"] = 0;
            result["status"] = "degraded";
            result["warning"] = "Failed to retrieve chain tip";
        }
    } catch (const std::exception& e) {
        result["height"] = 0;
        result["status"] = "error";
        result["error"] = std::string("Chain DB error: ") + e.what();
        return result;
    }

    // Get peer count from Phase C P2PManager
    if (p2p_manager) {
        result["peers"] = static_cast<Json::Value::Int>(p2p_manager->get_peer_count());

        // Warn if isolated (no peers)
        if (p2p_manager->get_peer_count() == 0) {
            result["status"] = "degraded";
            result["warning"] = "No active peer connections";
        }
    } else {
        result["peers"] = 0;
        result["status"] = "degraded";
        result["warning"] = "P2P manager unavailable";
    }

    // Get daemon uptime
    result["uptime_sec"] = static_cast<Json::Value::Int64>(GetDaemonUptimeSeconds());

    // Get consensus checksum from Phase B
    const auto& params_active = dinero::Params();
    std::string checksum = dinero::ConsensusChecksum(params_active);
    result["checksum"] = checksum;

    // Add version info
    result["version"] = GetDaemonVersion();

    return result;
}

// ============================================================================
// getminerstats - Mining Performance Telemetry
// ============================================================================
// Returns real-time mining statistics for performance monitoring
// Note: This is a stub implementation - full mining stats require miner integration
// ============================================================================

Json::Value getminerstats_handler(const Json::Value& params) {
    Json::Value result;

    // Mining is handled by external dinero-miner process
    // This RPC returns daemon-side mining state
    result["status"] = "external_miner";
    result["note"] = "Mining is handled by separate dinero-miner process";

    // Return daemon uptime (useful for monitoring)
    result["daemon_uptime_sec"] = static_cast<Json::Value::Int64>(GetDaemonUptimeSeconds());

    // TODO: Add mining pool stats if integrated in future
    // result["pool_address"] = ...
    // result["pool_connected"] = ...

    return result;
}

// ============================================================================
// getnodeidentity - Node Identification for Diagnostics
// ============================================================================
// Returns node identity info for cross-region debugging
// Useful when managing multiple nodes (Virginia, California, etc.)
// ============================================================================

Json::Value getnodeidentity_handler(
    const Json::Value& params,
    P2PManager* p2p_manager,
    uint16_t p2p_port
) {
    Json::Value result;

    // Node version and protocol
    result["version"] = GetDaemonVersion();
    result["protocol_version"] = 1;
    result["user_agent"] = std::string("Dinero/") + GetDaemonVersion();

    // External IP (if configured via --external-ip)
    if (!g_external_ip.empty()) {
        result["external_ip"] = g_external_ip;
    } else {
        result["external_ip"] = Json::Value(Json::nullValue);
        result["note"] = "Use --external-ip=<ip> to set external IP";
    }

    // P2P port
    result["p2p_port"] = static_cast<Json::Value::Int>(p2p_port);

    // Peer count from Phase C
    if (p2p_manager) {
        result["peers"] = static_cast<Json::Value::Int>(p2p_manager->get_peer_count());

        // Get list of connected peers
        auto peer_list = p2p_manager->get_connected_peers();
        Json::Value peers_array(Json::arrayValue);
        for (const auto& peer : peer_list) {
            if (peer.is_connected) {
                Json::Value peer_obj;
                peer_obj["address"] = peer.address;
                peer_obj["port"] = peer.port;
                peer_obj["is_outbound"] = peer.is_outbound;
                peers_array.append(peer_obj);
            }
        }
        result["connected_peers"] = peers_array;
    } else {
        result["peers"] = 0;
        result["connected_peers"] = Json::Value(Json::arrayValue);
    }

    // Uptime
    result["uptime_sec"] = static_cast<Json::Value::Int64>(GetDaemonUptimeSeconds());

    // Network (mainnet, testnet, regtest)
    const auto& params_active = dinero::Params();
    result["network"] = params_active.name;

    // Consensus checksum (for verifying same rules across nodes)
    result["consensus_checksum"] = dinero::ConsensusChecksum(params_active);

    // Generate identity summary string
    std::ostringstream identity_str;
    identity_str << "Dinero/" << GetDaemonVersion()
                 << " [" << params_active.name << "]";
    if (!g_external_ip.empty()) {
        identity_str << " @" << g_external_ip << ":" << p2p_port;
    }
    identity_str << " Peers:" << (p2p_manager ? p2p_manager->get_peer_count() : 0)
                 << " Uptime:" << GetDaemonUptimeSeconds() << "s";

    result["identity"] = identity_str.str();

    return result;
}

// ============================================================================
// getmetrics - Prometheus/OpenMetrics Exporter
// ============================================================================
// Returns metrics in Prometheus text format for scraping by monitoring systems
// Compatible with Prometheus, Grafana, and other OpenMetrics-compatible tools
// ============================================================================

Json::Value getmetrics_handler(
    const Json::Value& params,
    P2PManager* p2p_manager,
    dinero::ChainDB* chain_db
) {
    std::ostringstream metrics;

    // Prometheus/OpenMetrics text format header
    metrics << "# Dinero Node Metrics (OpenMetrics Format)\n";
    metrics << "# Network: " << dinero::Params().name << "\n";
    metrics << "\n";

    // Block height metric
    try {
        auto tip_result = chain_db->getTip();
        if (tip_result.status() == dinero::Status::Ok) {
            metrics << "# HELP dinero_block_height Current blockchain height\n";
            metrics << "# TYPE dinero_block_height gauge\n";
            metrics << "dinero_block_height " << tip_result.value().height << "\n";
            metrics << "\n";
        }
    } catch (const std::exception& e) {
        // If chain DB fails, skip this metric
    }

    // Peer count metric
    if (p2p_manager) {
        size_t peer_count = p2p_manager->get_peer_count();
        metrics << "# HELP dinero_peer_count Number of connected P2P peers\n";
        metrics << "# TYPE dinero_peer_count gauge\n";
        metrics << "dinero_peer_count " << peer_count << "\n";
        metrics << "\n";

        // Peer quality metrics (per-peer breakdown)
        auto peer_list = p2p_manager->get_connected_peers();
        if (!peer_list.empty()) {
            metrics << "# HELP dinero_peer_info Peer connection info (labels: address, outbound)\n";
            metrics << "# TYPE dinero_peer_info gauge\n";
            for (const auto& peer : peer_list) {
                if (peer.is_connected) {
                    metrics << "dinero_peer_info{address=\"" << peer.address
                            << "\",port=\"" << peer.port
                            << "\",outbound=\"" << (peer.is_outbound ? "true" : "false")
                            << "\"} 1\n";
                }
            }
            metrics << "\n";
        }
    }

    // Daemon uptime metric
    int64_t uptime_sec = GetDaemonUptimeSeconds();
    metrics << "# HELP dinero_uptime_seconds Daemon uptime in seconds\n";
    metrics << "# TYPE dinero_uptime_seconds counter\n";
    metrics << "dinero_uptime_seconds " << uptime_sec << "\n";
    metrics << "\n";

    // Consensus checksum (as info metric)
    std::string checksum = dinero::ConsensusChecksum(dinero::Params());
    metrics << "# HELP dinero_consensus_info Consensus checksum (label: checksum)\n";
    metrics << "# TYPE dinero_consensus_info gauge\n";
    metrics << "dinero_consensus_info{checksum=\"" << checksum << "\"} 1\n";
    metrics << "\n";

    // Version info
    std::string version = GetDaemonVersion();
    metrics << "# HELP dinero_version_info Daemon version (label: version)\n";
    metrics << "# TYPE dinero_version_info gauge\n";
    metrics << "dinero_version_info{version=\"" << version << "\"} 1\n";
    metrics << "\n";

    // Build info with all labels (git hash, build time, checksum)
    std::string git_hash = GetDaemonGitHash();
    std::string build_time = GetDaemonBuildTime();
    metrics << "# HELP dinero_build_info Build identification (labels: commit, version, build_time, checksum)\n";
    metrics << "# TYPE dinero_build_info gauge\n";
    metrics << "dinero_build_info{commit=\"" << git_hash 
            << "\",version=\"" << version
            << "\",build_time=\"" << build_time
            << "\",checksum=\"" << checksum << "\"} 1\n";
    metrics << "\n";

    // Network info
    const auto& params_active = dinero::Params();
    metrics << "# HELP dinero_network_info Network type (label: network)\n";
    metrics << "# TYPE dinero_network_info gauge\n";
    metrics << "dinero_network_info{network=\"" << params_active.name << "\"} 1\n";
    metrics << "\n";

    // Genesis verification status (always 1 if daemon is running)
    metrics << "# HELP dinero_genesis_verified Genesis block verification status (1=passed, 0=failed)\n";
    metrics << "# TYPE dinero_genesis_verified gauge\n";
    metrics << "dinero_genesis_verified 1\n";
    metrics << "\n";

    // Combined verification status with labels
    // This allows Grafana panels to instantly highlight nodes that diverge
    metrics << "# HELP dinero_verification_status Combined verification status with network and checksum labels\n";
    metrics << "# TYPE dinero_verification_status gauge\n";
    metrics << "dinero_verification_status{network=\"" << params_active.name
            << "\",checksum=\"" << checksum << "\"} 1\n";

    // Return metrics as a JSON string value (RPC returns JSON, but the string is Prometheus format)
    Json::Value result;
    result["metrics"] = metrics.str();
    result["format"] = "prometheus";
    result["timestamp"] = static_cast<Json::Value::Int64>(std::time(nullptr));

    return result;
}

// ============================================================================
// getverificationsummary - Genesis Verification Summary
// ============================================================================
// Returns structured JSON summary of block verification status
// Useful for RPC clients, wallets, and debugging consensus issues
//
// NOTE: This MUST return instantly - it only serializes in-memory data.
// If this hangs, there's a bug in the RPC server or daemon layer.
// ============================================================================

Json::Value getverificationsummary_handler(const Json::Value& params) {
    try {
        Json::Value result;

        // Network identification (fast - just returns static reference)
        const auto& params_active = dinero::Params();
        result["network"] = params_active.name;

        // Genesis verification (always true if daemon is running)
        result["genesis_verified"] = true;

        // Consensus checksum for cross-node validation (fast - SHA256 hash)
        result["consensus_checksum"] = dinero::ConsensusChecksum(params_active);

        // Version information (fast - compile-time constant)
        result["version"] = GetDaemonVersion();

        // Current timestamp (fast - system call)
        result["timestamp"] = static_cast<Json::Value::Int64>(std::time(nullptr));

        // Overall status
        result["status"] = "OK";

        return result;
    } catch (const std::exception& e) {
        // Defensive error handling - should never happen, but if it does, return error
        Json::Value error_result;
        error_result["error"] = std::string("Internal error in getverificationsummary: ") + e.what();
        error_result["status"] = "ERROR";
        return error_result;
    }
}

} // namespace rpc
} // namespace daemon
} // namespace dinero
