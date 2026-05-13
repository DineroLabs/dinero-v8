#include "p2p/network_limits_monitor.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace dinero {
namespace p2p {

//==============================================================================
// Utility Functions
//==============================================================================

const char* NetworkHealthStatusToString(NetworkHealthStatus status) {
    switch (status) {
        case NetworkHealthStatus::OK:       return "OK";
        case NetworkHealthStatus::WARNING:  return "WARNING";
        case NetworkHealthStatus::CRITICAL: return "CRITICAL";
        case NetworkHealthStatus::EXHAUSTED: return "EXHAUSTED";
        case NetworkHealthStatus::ERROR:    return "ERROR";
    }
    return "UNKNOWN";
}

//==============================================================================
// NetworkLimitsMonitor Implementation
//==============================================================================

NetworkLimitsMonitor::NetworkLimitsMonitor(
    std::shared_ptr<ConnectionManager> connection_mgr,
    std::shared_ptr<RateLimiter> rate_limiter,
    std::shared_ptr<PeerScoringManager> scoring_mgr,
    const NetworkLimitsConfig& config
)
    : connection_mgr_(connection_mgr)
    , rate_limiter_(rate_limiter)
    , scoring_mgr_(scoring_mgr)
    , config_(config)
{
}

NetworkUsageInfo NetworkLimitsMonitor::checkNetworkHealth() const {
    NetworkUsageInfo info;

    if (!connection_mgr_ || !rate_limiter_ || !scoring_mgr_) {
        info.status = NetworkHealthStatus::ERROR;
        info.details = "One or more network components not initialized";
        return info;
    }

    // ============================================================================
    // Connection Utilization
    // ============================================================================
    info.total_connections = connection_mgr_->getTotalCount();
    info.max_connections = connection_mgr_->getLimits().max_total;
    info.inbound_connections = connection_mgr_->getInboundCount();
    info.max_inbound = connection_mgr_->getLimits().max_inbound;
    info.outbound_connections = connection_mgr_->getOutboundCount();
    info.max_outbound = connection_mgr_->getLimits().max_outbound;

    if (info.max_connections > 0) {
        info.connection_utilization_percent =
            (static_cast<double>(info.total_connections) / info.max_connections) * 100.0;
    }

    // ============================================================================
    // Rate Limiting Statistics
    // ============================================================================
    auto rate_stats = rate_limiter_->getStats();
    info.active_rate_limiters = rate_stats.total_peers;
    info.total_rate_violations = rate_stats.total_violations;
    info.total_messages_allowed = rate_stats.total_messages_allowed;
    info.total_messages_rejected = rate_stats.total_messages_rejected;
    info.message_rejection_rate = calculateRejectionRate(
        info.total_messages_allowed,
        info.total_messages_rejected
    );

    // ============================================================================
    // Peer Scoring Statistics
    // ============================================================================
    auto scoring_stats = scoring_mgr_->getStats();
    info.total_tracked_peers = scoring_stats.total_peers;
    info.banned_peers = scoring_stats.banned_peers;
    info.misbehaving_peers = scoring_stats.misbehaving_peers;
    info.avg_peer_score = scoring_stats.avg_score;
    info.total_misbehaviors = scoring_stats.total_misbehaviors;

    // ============================================================================
    // Bandwidth Usage (future enhancement)
    // ============================================================================
    // TODO: Aggregate bandwidth stats from DoSProtection if available
    info.total_bytes_sent = 0;
    info.total_bytes_received = 0;
    info.send_rate_mbps = 0.0;
    info.recv_rate_mbps = 0.0;

    // ============================================================================
    // Overall Health Status
    // ============================================================================
    info.status = calculateHealthStatus(info.connection_utilization_percent);

    // Build status details string
    std::ostringstream details;
    details << "Connections: " << info.total_connections << "/" << info.max_connections
            << " (" << std::fixed << std::setprecision(1) << info.connection_utilization_percent << "%)";

    if (info.status == NetworkHealthStatus::EXHAUSTED) {
        details << " - EXHAUSTED";
    } else if (info.status == NetworkHealthStatus::CRITICAL) {
        details << " - CRITICAL";
    } else if (info.status == NetworkHealthStatus::WARNING) {
        details << " - WARNING";
    }

    if (info.banned_peers > 0) {
        details << ", Banned: " << info.banned_peers;
    }

    if (info.total_rate_violations > 0) {
        details << ", Rate violations: " << info.total_rate_violations;
    }

    info.details = details.str();

    return info;
}

bool NetworkLimitsMonitor::canAcceptConnection() const {
    if (!connection_mgr_) {
        return false;
    }

    // Check if we're at or near capacity
    uint32_t total = connection_mgr_->getTotalCount();
    uint32_t max = connection_mgr_->getLimits().max_total;

    // Reject if at 100% capacity
    return total < max;
}

bool NetworkLimitsMonitor::canSendMessage(uint64_t bytes) const {
    // Phase E.2.c: Bandwidth throttling placeholder
    // TODO: Implement actual bandwidth budget tracking
    // For now, always allow (bandwidth limits not yet enforced)
    (void)bytes;  // Unused parameter
    return true;
}

std::string NetworkLimitsMonitor::getNetworkUsageReport() const {
    auto info = checkNetworkHealth();

    std::ostringstream report;
    report << "========================================\\n";
    report << "Network Usage Report\\n";
    report << "========================================\\n\\n";

    // Overall status
    report << "Status: " << NetworkHealthStatusToString(info.status) << "\\n";
    report << "Details: " << info.details << "\\n\\n";

    // Connection breakdown
    report << std::fixed << std::setprecision(1);
    report << "Connections:\\n";
    report << "  Total:      " << info.total_connections << "/" << info.max_connections
           << " (" << info.connection_utilization_percent << "%)\\n";
    report << "  Inbound:    " << info.inbound_connections << "/" << info.max_inbound << "\\n";
    report << "  Outbound:   " << info.outbound_connections << "/" << info.max_outbound << "\\n";
    report << "\\n";

    // Rate limiting statistics
    report << "Rate Limiting:\\n";
    report << "  Active rate limiters:  " << info.active_rate_limiters << "\\n";
    report << "  Messages allowed:      " << info.total_messages_allowed << "\\n";
    report << "  Messages rejected:     " << info.total_messages_rejected << "\\n";
    report << std::setprecision(2);
    report << "  Rejection rate:        " << info.message_rejection_rate << "%\\n";
    report << "  Total violations:      " << info.total_rate_violations << "\\n";
    report << "\\n";

    // Peer scoring summary
    report << "Peer Scoring:\\n";
    report << "  Tracked peers:         " << info.total_tracked_peers << "\\n";
    report << "  Banned peers:          " << info.banned_peers << "\\n";
    report << "  Misbehaving peers:     " << info.misbehaving_peers << " (score > 0)\\n";
    report << "  Average score:         " << info.avg_peer_score << "\\n";
    report << "  Total misbehaviors:    " << info.total_misbehaviors << "\\n";
    report << "\\n";

    // Bandwidth (if available)
    if (info.total_bytes_sent > 0 || info.total_bytes_received > 0) {
        auto to_mb = [](uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
        report << std::setprecision(2);
        report << "Bandwidth:\\n";
        report << "  Sent:     " << to_mb(info.total_bytes_sent) << " MB\\n";
        report << "  Received: " << to_mb(info.total_bytes_received) << " MB\\n";
        report << "  Send rate:    " << info.send_rate_mbps << " Mbps\\n";
        report << "  Receive rate: " << info.recv_rate_mbps << " Mbps\\n";
        report << "\\n";
    }

    // Warnings
    if (info.status == NetworkHealthStatus::EXHAUSTED) {
        report << "⚠️  WARNING: Network resources EXHAUSTED - node may reject connections\\n";
    } else if (info.status == NetworkHealthStatus::CRITICAL) {
        report << "⚠️  WARNING: Network resources CRITICAL - approaching limits\\n";
    } else if (info.status == NetworkHealthStatus::WARNING) {
        report << "⚠️  WARNING: Network resources approaching capacity\\n";
    }

    if (info.banned_peers > 0) {
        report << "⚠️  WARNING: " << info.banned_peers << " peer(s) currently banned for misbehavior\\n";
    }

    report << "========================================\\n";

    return report.str();
}

//==============================================================================
// Private Helper Methods
//==============================================================================

NetworkHealthStatus NetworkLimitsMonitor::calculateHealthStatus(double utilization_percent) const {
    if (utilization_percent >= 100.0) {
        return NetworkHealthStatus::EXHAUSTED;
    } else if (utilization_percent >= config_.critical_threshold_percent) {
        return NetworkHealthStatus::CRITICAL;
    } else if (utilization_percent >= config_.warning_threshold_percent) {
        return NetworkHealthStatus::WARNING;
    } else {
        return NetworkHealthStatus::OK;
    }
}

double NetworkLimitsMonitor::calculateRejectionRate(uint32_t allowed, uint32_t rejected) const {
    uint64_t total = static_cast<uint64_t>(allowed) + static_cast<uint64_t>(rejected);
    if (total == 0) {
        return 0.0;
    }
    return (static_cast<double>(rejected) / total) * 100.0;
}

} // namespace p2p
} // namespace dinero
