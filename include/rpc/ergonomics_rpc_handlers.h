#pragma once

// SPDX-License-Identifier: MIT
// Phase W.3: RPC Ergonomics - High-level status aggregation

#include <string>
#include <vector>
#include "din_json.h"

// Forward declarations
struct ExecutionContext;

namespace dinero {

/**
 * @brief Phase W.3: RPC Ergonomics
 *
 * High-level status RPCs that aggregate information from multiple sources
 * to provide user-friendly health checks and readiness indicators.
 *
 * Design philosophy:
 * - Single call reveals everything
 * - Traffic light colors (GREEN/YELLOW/RED)
 * - Actionable warnings and blockers
 * - No guesswork required
 */

// ============================================================================
// Node Health Status
// ============================================================================

/**
 * Overall node health grade
 */
enum class NodeHealthGrade {
    GREEN = 0,   ///< Fully operational, no issues
    YELLOW = 1,  ///< Minor issues, still functional
    RED = 2      ///< Critical issues, not operational
};

/**
 * Subsystem health status
 */
struct SubsystemHealth {
    std::string name;          ///< Subsystem name (e.g., "sync", "network", "wallet")
    NodeHealthGrade grade;     ///< Health grade
    std::string status;        ///< Human-readable status
    std::vector<std::string> warnings;  ///< Warnings or issues
};

/**
 * Overall node health assessment
 */
struct NodeHealth {
    NodeHealthGrade overall_grade;           ///< Overall health grade
    std::vector<SubsystemHealth> subsystems; ///< Per-subsystem health
    std::vector<std::string> recommendations; ///< Actionable recommendations
    uint64_t timestamp_ms;                   ///< When health was checked
};

// ============================================================================
// Mining Readiness Status
// ============================================================================

/**
 * Mining readiness check result
 */
struct MiningReadiness {
    bool ready;                              ///< True if ready to mine
    std::vector<std::string> blockers;       ///< What's preventing mining (if not ready)
    std::vector<std::string> warnings;       ///< Non-blocking warnings
    std::string recommendation;              ///< What to do next

    // Detailed precondition checks
    bool node_synced;                        ///< Node is synced (not in IBD)
    bool wallet_synced;                      ///< Wallet is caught up
    bool sufficient_peers;                   ///< Enough network connections
    bool no_active_reorg;                    ///< Not in reorg recovery
    bool wallet_loaded;                      ///< Wallet is loaded

    uint64_t timestamp_ms;                   ///< When readiness was checked
};

// ============================================================================
// RPC Handlers (Phase W.3)
// ============================================================================

/**
 * RPC: node.gethealth
 *
 * Returns comprehensive node health status aggregating:
 * - Sync status (blockchain, headers, wallet)
 * - Network health (peers, connectivity)
 * - Wallet status (loaded, synced, locked)
 * - System warnings (disk space, clock skew, etc.)
 *
 * @param ctx Execution context
 * @return JSON response with node health
 */
std::string RPC_GetNodeHealth(const ExecutionContext& ctx);

/**
 * RPC: mining.getreadiness
 *
 * Checks if the node is ready to mine and returns specific blockers.
 *
 * Mining requires:
 * - Node fully synced (STEADY_STATE phase)
 * - Wallet loaded and synced
 * - Sufficient peer connections (3+)
 * - No active reorg in progress
 *
 * @param ctx Execution context
 * @return JSON response with mining readiness
 */
std::string RPC_GetMiningReadiness(const ExecutionContext& ctx);

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Build NodeHealth from current system state
 *
 * @param ctx Execution context
 * @return NodeHealth assessment
 */
NodeHealth BuildNodeHealth(const ExecutionContext& ctx);

/**
 * Build MiningReadiness from current system state
 *
 * @param ctx Execution context
 * @return MiningReadiness assessment
 */
MiningReadiness BuildMiningReadiness(const ExecutionContext& ctx);

/**
 * Convert NodeHealthGrade to string
 *
 * @param grade Health grade
 * @return "green", "yellow", or "red"
 */
std::string NodeHealthGradeToString(NodeHealthGrade grade);

/**
 * Convert NodeHealth to JSON
 *
 * @param health Node health assessment
 * @return JSON representation
 */
din::Json NodeHealthToJson(const NodeHealth& health);

/**
 * Convert MiningReadiness to JSON
 *
 * @param readiness Mining readiness assessment
 * @return JSON representation
 */
din::Json MiningReadinessToJson(const MiningReadiness& readiness);

// ============================================================================
// RPC Method Registration (Phase W.3)
// ============================================================================

namespace rpc {

/**
 * Register Phase W.3 ergonomics RPC methods
 *
 * Registers:
 * - node.gethealth: Comprehensive node health check
 * - mining.getreadiness: Mining preconditions check
 */
void registerErgonomicsRpcMethods();

} // namespace rpc

} // namespace dinero
