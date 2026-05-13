#pragma once

/**
 * Phase E.2.c: Network Limits Monitor
 *
 * PRODUCTION HARDENING: Prevent network resource exhaustion from causing DoS.
 *
 * Philosophy:
 * - The node may reject connections/messages, but must never exhaust network resources
 * - Network limits are HARD, not heuristic
 * - Fail early and loudly, not silently
 *
 * What this prevents:
 * - Connection exhaustion (too many peers → memory/fd exhaustion)
 * - Bandwidth exhaustion (flood → network saturation)
 * - Message spam (rapid-fire messages → CPU exhaustion)
 * - Eclipse attacks (all peers malicious → chain split)
 *
 * SPDX-License-Identifier: MIT
 */

#include "p2p/connection_manager.h"
#include "p2p/rate_limiter.h"
#include "p2p/peer_scoring.h"
#include <filesystem>
#include <cstdint>
#include <string>
#include <memory>

namespace dinero {
namespace p2p {

/**
 * Network health status
 */
enum class NetworkHealthStatus {
    OK = 0,              // All limits within safe thresholds
    WARNING,             // Approaching limits (>80% utilization)
    CRITICAL,            // Near limits (>95% utilization)
    EXHAUSTED,           // At hard limits (100% utilization)
    ERROR                // Monitoring error (cannot determine status)
};

const char* NetworkHealthStatusToString(NetworkHealthStatus status);

/**
 * Network resource usage information
 */
struct NetworkUsageInfo {
    // Connection utilization
    uint32_t total_connections{0};
    uint32_t max_connections{0};
    uint32_t inbound_connections{0};
    uint32_t max_inbound{0};
    uint32_t outbound_connections{0};
    uint32_t max_outbound{0};
    double connection_utilization_percent{0.0};

    // Rate limiting status
    uint32_t active_rate_limiters{0};
    uint32_t total_rate_violations{0};
    uint32_t total_messages_allowed{0};
    uint32_t total_messages_rejected{0};
    double message_rejection_rate{0.0};  // Percentage of messages rejected

    // Peer scoring status
    uint32_t total_tracked_peers{0};
    uint32_t banned_peers{0};
    uint32_t misbehaving_peers{0};     // Score > 0
    int32_t avg_peer_score{0};
    uint64_t total_misbehaviors{0};

    // Bandwidth usage (if available)
    uint64_t total_bytes_sent{0};
    uint64_t total_bytes_received{0};
    double send_rate_mbps{0.0};        // Megabits per second
    double recv_rate_mbps{0.0};        // Megabits per second

    NetworkHealthStatus status{NetworkHealthStatus::ERROR};
    std::string details;               // Human-readable status details
};

/**
 * Network limits configuration
 * Aggregates all network resource limits
 */
struct NetworkLimitsConfig {
    // Connection limits (from ConnectionLimits)
    uint32_t max_total_connections{125};
    uint32_t max_inbound{115};
    uint32_t max_outbound{10};
    uint32_t max_blocks_only{8};

    // Rate limiting (from RateLimiterConfig)
    uint32_t rate_limit_max_tokens{100};
    double rate_limit_refill_rate{10.0};
    uint32_t rate_limit_ban_threshold{5};
    bool rate_limiting_enabled{true};

    // Peer scoring (from PeerScoringManager)
    int32_t peer_ban_threshold{100};
    double peer_score_decay_rate{0.1};  // 10% per hour

    // Bandwidth limits (Phase E.2.c enhancement)
    uint64_t max_send_rate_mbps{100};      // Max send rate in Mbps (0 = unlimited)
    uint64_t max_recv_rate_mbps{100};      // Max recv rate in Mbps (0 = unlimited)
    uint64_t max_total_bandwidth_mbps{150}; // Max combined bandwidth (0 = unlimited)

    // Warning thresholds
    double warning_threshold_percent{80.0};    // Warn at 80% utilization
    double critical_threshold_percent{95.0};   // Critical at 95% utilization

    NetworkLimitsConfig() = default;
};

/**
 * Network Limits Monitor
 *
 * Aggregates and monitors all network resource limits:
 * - Connection limits (ConnectionManager)
 * - Message rate limits (RateLimiter)
 * - Peer ban scores (PeerScoringManager)
 * - Bandwidth usage (optional)
 *
 * Provides unified visibility into network health and resource utilization.
 *
 * Usage:
 *   NetworkLimitsMonitor monitor(connection_mgr, rate_limiter, scoring_mgr);
 *   auto info = monitor.checkNetworkHealth();
 *
 *   if (info.status == NetworkHealthStatus::EXHAUSTED) {
 *       std::cerr << "CRITICAL: Network resources exhausted\\n";
 *       // Take action (reject new connections, throttle, etc.)
 *   }
 */
class NetworkLimitsMonitor {
public:
    /**
     * Constructor
     * @param connection_mgr Connection manager instance (required)
     * @param rate_limiter Rate limiter instance (required)
     * @param scoring_mgr Peer scoring manager instance (required)
     * @param config Network limits configuration (optional)
     */
    explicit NetworkLimitsMonitor(
        std::shared_ptr<ConnectionManager> connection_mgr,
        std::shared_ptr<RateLimiter> rate_limiter,
        std::shared_ptr<PeerScoringManager> scoring_mgr,
        const NetworkLimitsConfig& config = NetworkLimitsConfig()
    );

    /**
     * Check current network health status
     *
     * Aggregates data from:
     * - ConnectionManager (connection counts)
     * - RateLimiter (rate limit violations)
     * - PeerScoringManager (ban scores, misbehavior)
     *
     * @return NetworkUsageInfo with current status
     */
    NetworkUsageInfo checkNetworkHealth() const;

    /**
     * Check if safe to accept a new inbound connection
     *
     * @return true if network has capacity, false if exhausted
     */
    bool canAcceptConnection() const;

    /**
     * Check if safe to send a message
     * (placeholder for future bandwidth throttling)
     *
     * @param bytes Size of message in bytes
     * @return true if bandwidth available, false if throttled
     */
    bool canSendMessage(uint64_t bytes) const;

    /**
     * Get detailed network usage report (for logging/RPC)
     *
     * Returns multi-line report with:
     * - Connection utilization
     * - Rate limiting statistics
     * - Peer scoring summary
     * - Bandwidth usage (if available)
     *
     * @return Human-readable report
     */
    std::string getNetworkUsageReport() const;

    /**
     * Get configuration
     */
    const NetworkLimitsConfig& getConfig() const { return config_; }

private:
    // Component instances
    std::shared_ptr<ConnectionManager> connection_mgr_;
    std::shared_ptr<RateLimiter> rate_limiter_;
    std::shared_ptr<PeerScoringManager> scoring_mgr_;

    // Configuration
    NetworkLimitsConfig config_;

    /**
     * Helper: Determine health status from utilization percentage
     */
    NetworkHealthStatus calculateHealthStatus(double utilization_percent) const;

    /**
     * Helper: Calculate message rejection rate
     */
    double calculateRejectionRate(uint32_t allowed, uint32_t rejected) const;
};

} // namespace p2p
} // namespace dinero
