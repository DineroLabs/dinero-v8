#pragma once

#include "p2p/block_download_scheduler.h"  // For SyncPhase enum
#include <cstdint>
#include <memory>

// Forward declarations
namespace dinero {
    class Mempool;
    class BlockRelayManager;
}

namespace dinero {

/**
 * @brief Block assembly context - network-aware signals for intelligent mining
 *
 * Phase W.1.1: This struct aggregates signals from Phase G (Networking & Relay)
 * to enable context-aware block assembly. It is derived/computed, not stored.
 *
 * Design Principles:
 * - Lightweight (computed on-demand, not persisted)
 * - Read-only signals from Phase G components
 * - No consensus impact (mining optimization only)
 * - Testable in isolation
 *
 * Integration Points:
 * - G.12: SyncPhase (IBD vs catching_up vs steady_state)
 * - G.16: Compact block reconstruction success rate
 * - G.17: Mempool pressure and fee-aware propagation
 */
struct BlockAssemblyContext {
    // ========================================================================
    // Phase G.12: Sync State (IBD vs Catching Up vs Steady State)
    // ========================================================================
    /**
     * Current sync phase of the node
     *
     * IBD:          Don't mine (chain not synced)
     * CATCHING_UP:  Conservative template (focus on propagation speed)
     * STEADY_STATE: Optimize for revenue (use all available intelligence)
     */
    SyncPhase sync_phase;

    // ========================================================================
    // Phase G.17: Mempool Pressure (Transaction Congestion)
    // ========================================================================
    /**
     * Mempool pressure: ratio of pending transactions to block capacity
     *
     * Range: [0.0, +inf)
     * - 0.0:   Empty mempool
     * - 1.0:   Exactly 1 block worth of transactions
     * - 5.0:   5 blocks worth of backlog
     *
     * Used for:
     * - Fee estimation
     * - Template refresh frequency (high pressure = refresh more often)
     * - Mining probability scoring (high pressure = new txs less likely)
     */
    double mempool_pressure;

    // ========================================================================
    // Phase G.16: Compact Block Success Rate (Reconstruction Quality)
    // ========================================================================
    /**
     * Recent compact block reconstruction success rate
     *
     * Range: [0.0, 1.0]
     * - 1.0:  Perfect reconstruction (peers have same mempool)
     * - 0.5:  50% success (moderate mempool divergence)
     * - 0.0:  Complete failure (full block fallback)
     *
     * Used for:
     * - Compact-friendly template bias (W.1.3)
     * - Transaction ordering (prefer widely-propagated txs)
     * - Template refresh strategy
     */
    double compact_success_rate;

    // ========================================================================
    // Template Metadata (Internal Tracking)
    // ========================================================================
    /**
     * Timestamp of last template generation (milliseconds)
     *
     * Used for:
     * - Template staleness detection
     * - Refresh frequency calculation
     * - Performance metrics
     */
    uint64_t last_template_time_ms;

    /**
     * Number of mempool changes since last template
     *
     * Used for:
     * - Incremental refresh triggering (W.1.4)
     * - Template staleness scoring
     * - Mempool delta tracking
     */
    uint64_t mempool_delta_count;

    // ========================================================================
    // Constructor: Default (Safe Fallback Values)
    // ========================================================================
    BlockAssemblyContext();

    // ========================================================================
    // Factory: Create from Live System State
    // ========================================================================
    /**
     * @brief Create context from current network state
     *
     * Aggregates signals from:
     * - Mempool (pressure, delta count)
     * - BlockRelayManager (compact success rate, sync phase)
     *
     * @param mempool Mempool instance (required)
     * @param relay_manager BlockRelayManager instance (optional - uses defaults if null)
     * @param last_template_time Previous template timestamp (0 = first template)
     * @return BlockAssemblyContext with current network signals
     */
    static BlockAssemblyContext CreateFromNetworkState(
        const Mempool* mempool,
        const BlockRelayManager* relay_manager = nullptr,
        uint64_t last_template_time_ms = 0
    );

    // ========================================================================
    // Derived Metrics (Computed Properties)
    // ========================================================================

    /**
     * @brief Should template be refreshed?
     *
     * Criteria:
     * - Time since last template > 30 seconds (stale)
     * - Mempool delta > 10 transactions (significant change)
     * - Mempool pressure > 2.0 (high congestion = refresh more often)
     *
     * @param current_time_ms Current timestamp (milliseconds)
     * @return True if template should be refreshed
     */
    bool ShouldRefreshTemplate(uint64_t current_time_ms) const;

    /**
     * @brief Get template refresh urgency score
     *
     * Range: [0.0, 1.0]
     * - 0.0: No urgency (template is fresh)
     * - 1.0: Maximum urgency (immediate refresh required)
     *
     * Factors:
     * - Template age (30s = moderate, 60s = high)
     * - Mempool delta count (10 txs = moderate, 50 txs = high)
     * - Mempool pressure (2.0 = moderate, 5.0 = high)
     *
     * @param current_time_ms Current timestamp (milliseconds)
     * @return Urgency score [0.0, 1.0]
     */
    double GetRefreshUrgency(uint64_t current_time_ms) const;

    /**
     * @brief Get compact-friendly bias weight
     *
     * Range: [0.0, 1.0]
     * - 0.0: Ignore compact success (fee-only optimization)
     * - 1.0: Maximize compact success (propagation-first optimization)
     *
     * Formula:
     * - compact_success_rate > 0.9 → bias = 0.3 (high success = moderate bias)
     * - compact_success_rate > 0.7 → bias = 0.2 (medium success = low bias)
     * - compact_success_rate < 0.7 → bias = 0.0 (low success = ignore)
     *
     * @return Bias weight for compact-friendly transaction ordering
     */
    double GetCompactFriendlyBias() const;

    /**
     * @brief Should use aggressive fee optimization?
     *
     * True when:
     * - sync_phase == STEADY_STATE (node is synced)
     * - mempool_pressure > 1.5 (sufficient transaction backlog)
     *
     * False when:
     * - sync_phase != STEADY_STATE (still syncing)
     * - mempool_pressure < 1.5 (low backlog, propagation matters more)
     *
     * @return True if aggressive fee optimization should be used
     */
    bool ShouldOptimizeForFees() const;
};

} // namespace dinero
