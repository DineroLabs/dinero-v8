#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace dinero {

/**
 * @brief Phase W.2.5: Slow Reason Categories
 *
 * Answers the question: "Why is my wallet syncing slowly?"
 *
 * Design: Each reason maps to observable metrics from the node.
 * Never speculate - only report what we can measure.
 */
enum class SlowReason {
    /**
     * @brief Network-wide Initial Block Download in progress
     *
     * Detection:
     * - Multiple peers also syncing (not at chain tip)
     * - Low overall network hash rate
     * - Many peers at similar heights
     *
     * User message: "Initial blockchain download - network is syncing"
     * Severity: Normal (expected during IBD)
     */
    NETWORK_IBD,

    /**
     * @brief Poor peer quality (low bandwidth, high latency)
     *
     * Detection:
     * - Low download rates from peers (< 100 KB/s)
     * - High ping times (> 500ms)
     * - Frequent peer disconnections
     * - Many failed compact block reconstructions
     *
     * User message: "Slow peer connections - try restarting or adding peers"
     * Severity: Moderate (user can fix)
     */
    LOW_PEER_QUALITY,

    /**
     * @brief Disk I/O bottleneck
     *
     * Detection:
     * - Block validation rate < 10 blocks/sec
     * - RocksDB write stalls detected
     * - High disk queue depth
     *
     * User message: "Disk I/O bottleneck - consider faster storage"
     * Severity: High (impacts performance significantly)
     */
    DISK_BOUND,

    /**
     * @brief High mempool pressure (many pending transactions)
     *
     * Detection:
     * - Mempool size > 50 MB
     * - Mempool evictions occurring
     * - Validation times increasing
     *
     * User message: "High mempool activity - network is busy"
     * Severity: Low (temporary, network condition)
     */
    HIGH_MEMPOOL_PRESSURE,

    /**
     * @brief Recovering from chain reorganization
     *
     * Detection:
     * - Recent reorg detected (< 5 min ago)
     * - Blocks being disconnected/reconnected
     *
     * User message: "Recovering from chain reorganization"
     * Severity: Moderate (temporary, self-resolving)
     */
    REORG_RECOVERY,

    /**
     * @brief Wallet scanning old blocks (e.g., after restore)
     *
     * Detection:
     * - wallet_scan_height significantly behind chain_height
     * - Scan rate < 100 blocks/sec
     *
     * User message: "Scanning wallet history - may take time"
     * Severity: Normal (expected after restore)
     */
    WALLET_RESCAN,

    /**
     * @brief No obvious slowness detected
     *
     * Sync is proceeding normally for current conditions.
     */
    NONE
};

/**
 * @brief Severity of slowness reason
 */
enum class SlowSeverity {
    NONE = 0,      // No slowness
    LOW = 1,       // Minor, temporary (e.g., mempool pressure)
    MODERATE = 2,  // Noticeable, user can help (e.g., peer quality)
    HIGH = 3       // Significant, requires attention (e.g., disk bottleneck)
};

/**
 * @brief Detailed slow reason information
 */
struct SlowReasonInfo {
    SlowReason reason;
    SlowSeverity severity;
    std::string description;     // User-facing message
    std::string suggestion;      // What user can do about it
    double impact_factor;        // 0.0-1.0, how much this is slowing sync

    /**
     * @brief Additional context (optional)
     *
     * Examples:
     * - "Download rate: 45 KB/s (< 100 KB/s threshold)"
     * - "Mempool size: 78 MB (> 50 MB threshold)"
     * - "Disk writes: 2.3 MB/s (slow)"
     */
    std::vector<std::string> context;
};

/**
 * @brief Get user-facing description for slow reason
 */
inline std::string GetSlowReasonDescription(SlowReason reason) {
    switch (reason) {
        case SlowReason::NETWORK_IBD:
            return "Initial blockchain download - network is syncing";
        case SlowReason::LOW_PEER_QUALITY:
            return "Slow peer connections detected";
        case SlowReason::DISK_BOUND:
            return "Disk I/O bottleneck detected";
        case SlowReason::HIGH_MEMPOOL_PRESSURE:
            return "High mempool activity - network is busy";
        case SlowReason::REORG_RECOVERY:
            return "Recovering from chain reorganization";
        case SlowReason::WALLET_RESCAN:
            return "Scanning wallet history";
        case SlowReason::NONE:
            return "Syncing normally";
        default:
            return "Unknown reason";
    }
}

/**
 * @brief Get user-actionable suggestion for slow reason
 */
inline std::string GetSlowReasonSuggestion(SlowReason reason) {
    switch (reason) {
        case SlowReason::NETWORK_IBD:
            return "This is normal during initial sync. Please wait.";
        case SlowReason::LOW_PEER_QUALITY:
            return "Try restarting the node or adding high-quality peers.";
        case SlowReason::DISK_BOUND:
            return "Consider using faster storage (SSD recommended).";
        case SlowReason::HIGH_MEMPOOL_PRESSURE:
            return "Network congestion is temporary. Sync will speed up soon.";
        case SlowReason::REORG_RECOVERY:
            return "Node is reorganizing blocks. This should complete soon.";
        case SlowReason::WALLET_RESCAN:
            return "Wallet scan is in progress. This is expected after restore.";
        case SlowReason::NONE:
            return "";
        default:
            return "";
    }
}

/**
 * @brief Get severity for slow reason
 */
inline SlowSeverity GetSlowReasonSeverity(SlowReason reason) {
    switch (reason) {
        case SlowReason::NETWORK_IBD:
            return SlowSeverity::NONE;  // Expected
        case SlowReason::LOW_PEER_QUALITY:
            return SlowSeverity::MODERATE;
        case SlowReason::DISK_BOUND:
            return SlowSeverity::HIGH;
        case SlowReason::HIGH_MEMPOOL_PRESSURE:
            return SlowSeverity::LOW;
        case SlowReason::REORG_RECOVERY:
            return SlowSeverity::MODERATE;
        case SlowReason::WALLET_RESCAN:
            return SlowSeverity::NONE;  // Expected
        case SlowReason::NONE:
            return SlowSeverity::NONE;
        default:
            return SlowSeverity::NONE;
    }
}

} // namespace dinero
