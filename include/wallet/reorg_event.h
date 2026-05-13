#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include <optional>

namespace dinero {

/**
 * @brief Phase W.2.4: Reorg Event Tracking
 *
 * Captures critical reorg information for user visibility and trust.
 *
 * Design Principles:
 * - Track what matters: depth, balance impact, affected height
 * - Surface to UI: clear, actionable signals
 * - Enable freeze semantics: pause ETA during reorg
 * - Build user trust: never hide chain reorganizations
 */
struct ReorgEvent {
    /**
     * @brief Height at which reorg was detected
     */
    uint64_t detected_at_height;

    /**
     * @brief Depth of the reorganization (blocks unwound)
     *
     * Examples:
     * - depth=1: Minor (single block replaced)
     * - depth=3: Moderate (requires attention)
     * - depth=6+: Major (red flag for user)
     */
    int depth;

    /**
     * @brief Balance change due to reorg (in una)
     *
     * Can be:
     * - Positive: Gained balance (rare, but possible)
     * - Negative: Lost balance (common - tx became unconfirmed)
     * - Zero: No balance impact
     */
    int64_t balance_change;

    /**
     * @brief Number of transactions affected by reorg
     *
     * Includes:
     * - Transactions that became unconfirmed
     * - Transactions that appeared in new chain
     */
    uint32_t affected_tx_count;

    /**
     * @brief Timestamp when reorg was detected (milliseconds since epoch)
     */
    uint64_t timestamp_ms;

    /**
     * @brief Whether the reorg is still in progress
     *
     * True: Chain is actively reorganizing (freeze ETA)
     * False: Reorg complete, resumed normal sync
     */
    bool is_in_progress;

    /**
     * @brief Get human-readable severity level
     *
     * @return "minor", "moderate", "major"
     */
    std::string GetSeverity() const {
        if (depth <= 1) return "minor";
        if (depth <= 5) return "moderate";
        return "major";
    }

    /**
     * @brief Get user-facing description
     *
     * Examples:
     * - "Reorganization detected (depth: 1, minor)"
     * - "Reorganization detected (depth: 3, moderate) - 2 transactions affected"
     * - "Balance updated: -0.5 DIN due to reorganization"
     */
    std::string GetDescription() const;

    /**
     * @brief Check if reorg had balance impact
     */
    bool HasBalanceImpact() const {
        return balance_change != 0;
    }

    /**
     * @brief Check if reorg is severe enough to warrant user alert
     *
     * Alert if:
     * - Depth ≥ 3 (moderate or major)
     * - Balance changed
     * - Multiple transactions affected
     */
    bool RequiresUserAlert() const {
        return depth >= 3 || HasBalanceImpact() || affected_tx_count > 1;
    }
};

/**
 * @brief Reorg severity levels for UX decisions
 */
enum class ReorgSeverity {
    MINOR = 0,      // depth 1, no balance impact → silent handling
    MODERATE = 1,   // depth 2-5 → show notification
    MAJOR = 2       // depth 6+ → alert user, may indicate issue
};

/**
 * @brief Convert depth to severity level
 */
inline ReorgSeverity GetReorgSeverity(int depth) {
    if (depth <= 1) return ReorgSeverity::MINOR;
    if (depth <= 5) return ReorgSeverity::MODERATE;
    return ReorgSeverity::MAJOR;
}

} // namespace dinero
