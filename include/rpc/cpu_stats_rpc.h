/**
 * CPU Stats RPC Methods - Phase E.3.1
 *
 * Read-only diagnostic endpoints for CPU budget monitoring.
 *
 * Design Principles:
 * - Read-only (no control or tuning)
 * - Non-consensus (diagnostic only)
 * - No changes to validation paths
 * - Pulls from existing atomic counters
 */

#pragma once

#include "din_json.h"
#include "rpc/rpc_registry.h"

// Forward declarations
struct ExecutionContext;

// ═══════════════════════════════════════════════════════════════
// PHASE E.3.1: CPU STATS RPC HANDLERS
// ═══════════════════════════════════════════════════════════════

/**
 * node.getcpustats - Get detailed CPU budget statistics
 *
 * Returns:
 * - Script validation: budget, total validated, timeouts, timeout rate
 * - Block validation: budget, total validated, timeouts, timeout rate
 * - Signature verification: budget, total verified, timeouts, timeout rate
 * - CPU load percentage
 * - Overall status (OK, WARNING, CRITICAL, EXHAUSTED, ERROR)
 */
din::Json rpc_getcpustats(const ExecutionContext& ctx, const din::Json& params);

/**
 * node.getresourcepressure - Get aggregate resource health status
 *
 * Returns simple health status for:
 * - CPU (from CPUBudgetMonitor)
 * - Memory (from MemoryMonitor)
 * - Disk (from DiskSpaceMonitor)
 * - Network (from NetworkLimitsMonitor)
 * - Overall (worst of all resources)
 */
din::Json rpc_getresourcepressure(const ExecutionContext& ctx, const din::Json& params);

/**
 * Register CPU stats RPC methods with registry
 *
 * Call during daemon initialization to register:
 * - node.getcpustats
 * - node.getresourcepressure
 */
void register_cpu_stats_rpc_methods(RpcRegistry& registry);
