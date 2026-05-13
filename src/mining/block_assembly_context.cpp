#include "mining/block_assembly_context.h"
#include "daemon/mempool.h"
#include "daemon/block_relay_manager.h"
#include "common/logger.h"
#include <chrono>
#include <algorithm>

namespace dinero {

// ============================================================================
// Constructor: Default (Safe Fallback Values)
// ============================================================================

BlockAssemblyContext::BlockAssemblyContext()
    : sync_phase(SyncPhase::STEADY_STATE)  // Assume synced by default
    , mempool_pressure(0.0)                 // Empty mempool
    , compact_success_rate(0.5)             // Conservative default
    , last_template_time_ms(0)              // No previous template
    , mempool_delta_count(0)                // No changes
{
}

// ============================================================================
// Factory: Create from Live System State
// ============================================================================

BlockAssemblyContext BlockAssemblyContext::CreateFromNetworkState(
    const Mempool* mempool,
    const BlockRelayManager* relay_manager,
    uint64_t last_template_time_ms
) {
    BlockAssemblyContext ctx;
    ctx.last_template_time_ms = last_template_time_ms;

    // ========================================================================
    // 1. Mempool Pressure (Phase G.17)
    // ========================================================================
    if (mempool) {
        size_t mempool_size = mempool->size();
        size_t mempool_bytes = mempool->getTotalSize();

        // Calculate mempool pressure: ratio of pending data to block capacity
        // Standard block size limit: ~1 MB (1,000,000 bytes)
        // Dinero uses 4 MB weight limit (similar to Bitcoin post-SegWit)
        const uint64_t BLOCK_CAPACITY_BYTES = 1000000;  // 1 MB conservative

        if (BLOCK_CAPACITY_BYTES > 0) {
            ctx.mempool_pressure = static_cast<double>(mempool_bytes) / BLOCK_CAPACITY_BYTES;
        } else {
            ctx.mempool_pressure = 0.0;
        }

        // Mempool delta count: how many transactions added since last template?
        // This is a simplified implementation - in production, track actual deltas
        // For now, use mempool size as a proxy (will be refined in W.1.4)
        ctx.mempool_delta_count = mempool_size;

        dinero::g_logger.debug("BlockAssemblyContext: mempool_pressure=" +
                              std::to_string(ctx.mempool_pressure) +
                              " (size=" + std::to_string(mempool_size) +
                              ", bytes=" + std::to_string(mempool_bytes) + ")");
    } else {
        ctx.mempool_pressure = 0.0;
        ctx.mempool_delta_count = 0;
        dinero::g_logger.warning("BlockAssemblyContext: Mempool not provided, using defaults");
    }

    // ========================================================================
    // 2. Sync Phase (Phase G.12)
    // ========================================================================
    if (relay_manager) {
        // Get sync phase from BlockRelayManager
        // BlockRelayManager tracks SyncPhase via SyncPhaseTracker
        ctx.sync_phase = relay_manager->GetCurrentSyncPhase();

        dinero::g_logger.debug("BlockAssemblyContext: sync_phase=" +
                              std::to_string(static_cast<int>(ctx.sync_phase)));
    } else {
        // Default: assume STEADY_STATE (safe for mining)
        ctx.sync_phase = SyncPhase::STEADY_STATE;
        dinero::g_logger.debug("BlockAssemblyContext: BlockRelayManager not provided, assuming STEADY_STATE");
    }

    // ========================================================================
    // 3. Compact Block Success Rate (Phase G.16)
    // ========================================================================
    if (relay_manager) {
        // Get compact block reconstruction success rate from AdaptiveCompactBlockStrategy
        // This is the percentage of successful compact block reconstructions
        ctx.compact_success_rate = relay_manager->GetCompactBlockSuccessRate();

        dinero::g_logger.debug("BlockAssemblyContext: compact_success_rate=" +
                              std::to_string(ctx.compact_success_rate));
    } else {
        // Default: 0.5 (conservative - assume moderate success)
        ctx.compact_success_rate = 0.5;
        dinero::g_logger.debug("BlockAssemblyContext: BlockRelayManager not provided, assuming 50% compact success");
    }

    // ========================================================================
    // Log Final Context
    // ========================================================================
    dinero::g_logger.info("BlockAssemblyContext created: " +
                         std::string("sync_phase=") + std::to_string(static_cast<int>(ctx.sync_phase)) +
                         ", pressure=" + std::to_string(ctx.mempool_pressure) +
                         ", compact_success=" + std::to_string(ctx.compact_success_rate) +
                         ", delta=" + std::to_string(ctx.mempool_delta_count));

    return ctx;
}

// ============================================================================
// Derived Metrics (Computed Properties)
// ============================================================================

bool BlockAssemblyContext::ShouldRefreshTemplate(uint64_t current_time_ms) const {
    // Criteria 1: Template age > 30 seconds
    if (last_template_time_ms > 0) {
        uint64_t age_ms = current_time_ms - last_template_time_ms;
        const uint64_t STALE_THRESHOLD_MS = 30000;  // 30 seconds

        if (age_ms > STALE_THRESHOLD_MS) {
            dinero::g_logger.debug("ShouldRefreshTemplate: YES (age=" +
                                  std::to_string(age_ms / 1000) + "s > 30s)");
            return true;
        }
    }

    // Criteria 2: Mempool delta > 10 transactions
    const uint64_t DELTA_THRESHOLD = 10;
    if (mempool_delta_count > DELTA_THRESHOLD) {
        dinero::g_logger.debug("ShouldRefreshTemplate: YES (delta=" +
                              std::to_string(mempool_delta_count) + " > 10)");
        return true;
    }

    // Criteria 3: High mempool pressure (>2.0) = refresh more often
    // Even if template is recent, high congestion means new high-fee txs arriving
    const double HIGH_PRESSURE_THRESHOLD = 2.0;
    if (mempool_pressure > HIGH_PRESSURE_THRESHOLD) {
        if (last_template_time_ms > 0) {
            uint64_t age_ms = current_time_ms - last_template_time_ms;
            const uint64_t PRESSURE_REFRESH_MS = 15000;  // 15 seconds under pressure

            if (age_ms > PRESSURE_REFRESH_MS) {
                dinero::g_logger.debug("ShouldRefreshTemplate: YES (high pressure, age=" +
                                      std::to_string(age_ms / 1000) + "s > 15s)");
                return true;
            }
        }
    }

    return false;
}

double BlockAssemblyContext::GetRefreshUrgency(uint64_t current_time_ms) const {
    double urgency = 0.0;

    // Factor 1: Template age (30s = 0.5, 60s = 1.0)
    if (last_template_time_ms > 0) {
        uint64_t age_ms = current_time_ms - last_template_time_ms;
        double age_seconds = static_cast<double>(age_ms) / 1000.0;
        double age_factor = std::min(1.0, age_seconds / 60.0);  // 60s = max urgency
        urgency += age_factor * 0.4;  // Age contributes 40%
    }

    // Factor 2: Mempool delta count (10 txs = 0.2, 50 txs = 1.0)
    double delta_factor = std::min(1.0, static_cast<double>(mempool_delta_count) / 50.0);
    urgency += delta_factor * 0.3;  // Delta contributes 30%

    // Factor 3: Mempool pressure (2.0 = 0.4, 5.0 = 1.0)
    double pressure_factor = std::min(1.0, mempool_pressure / 5.0);
    urgency += pressure_factor * 0.3;  // Pressure contributes 30%

    return std::min(1.0, urgency);  // Clamp to [0.0, 1.0]
}

double BlockAssemblyContext::GetCompactFriendlyBias() const {
    // High success (>90%) → moderate bias (0.3)
    // We want compact-friendly templates, but not at the expense of fees
    if (compact_success_rate > 0.9) {
        return 0.3;
    }

    // Medium success (70-90%) → low bias (0.2)
    // Still worth considering, but fees dominate
    if (compact_success_rate > 0.7) {
        return 0.2;
    }

    // Low success (<70%) → no bias (0.0)
    // Mempool divergence too high, focus purely on fees
    return 0.0;
}

bool BlockAssemblyContext::ShouldOptimizeForFees() const {
    // Only optimize aggressively when:
    // 1. Node is fully synced (STEADY_STATE)
    // 2. Mempool has sufficient backlog (pressure > 1.5)

    bool is_synced = (sync_phase == SyncPhase::STEADY_STATE);
    bool has_backlog = (mempool_pressure > 1.5);

    return is_synced && has_backlog;
}

} // namespace dinero
