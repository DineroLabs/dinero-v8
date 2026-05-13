/**
 * Mining Control RPC Methods - Phase Y (CPU Miner Rebuild)
 *
 * Provides user-facing RPC methods for controlling the integrated CPU miner:
 * - mining.start    : Start CPU mining with N threads
 * - mining.stop     : Stop CPU mining
 * - mining.getstatus: Get mining status (hashrate, threads, blocks found)
 * - mining.setthreads: Adjust thread count while mining
 *
 * Design Principles:
 * - Context-aware (accesses MiningManager via ctx.mining)
 * - Thread-safe (MiningManager handles locking internally)
 * - Production-ready error handling
 * - Compatible with MiningManager v2 API
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/mining_service.h"  // For MiningService
#include "mining/mining_manager_v2.h"
#include "mining/address_validator.h"  // Taproot address validation
#include "common/logger.h"
#include <memory>
#include <sstream>

// ═══════════════════════════════════════════════════════════════
// PHASE Y: CPU MINER CONTROL RPC HANDLERS
// ═══════════════════════════════════════════════════════════════

/**
 * mining.start - Start CPU mining
 *
 * Starts the integrated CPU miner with the specified number of threads.
 * Requires a mining address to be set (via mining.setaddress or config).
 *
 * Parameters:
 * - threads (optional, default=0): Number of threads (0 = auto-detect optimal)
 *
 * Example:
 * > mining.start 4
 * {
 *   "status": "started",
 *   "threads": 4,
 *   "address": "din1q..."
 * }
 */
din::Json rpc_mining_start(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Phase Y: Get MiningManager from DaemonContext
    if (!ctx.daemon || !ctx.daemon->mining) {
        result["error"] = "Mining service not available";
        return result;
    }

    auto mining_service = std::dynamic_pointer_cast<dinero::MiningService>(ctx.daemon->mining);
    if (!mining_service) {
        result["error"] = "Mining service not initialized";
        return result;
    }

    auto& mining_mgr = mining_service->getMiningManager();

    // Check if already mining
    if (mining_mgr.isMining()) {
        result["error"] = "Mining is already active";
        result["status"] = "already_mining";
        return result;
    }

    // Parse thread count (optional parameter)
    int threads = 0;  // 0 = auto-detect
    if (params.isArray() && params.size() > 0) {
        threads = params[0].asInt();
        if (threads < 0 || threads > 256) {
            result["error"] = "Invalid thread count (must be 0-256)";
            return result;
        }
    }

    // Check if mining address is set
    std::string address = mining_mgr.getMiningAddress();
    if (address.empty()) {
        result["error"] = "Mining address not set. Use mining.setaddress first.";
        return result;
    }

    // Start mining
    bool success = mining_mgr.startMining(threads);
    if (!success) {
        result["error"] = "Failed to start mining";
        result["status"] = "error";
        return result;
    }

    // Success
    result["status"] = "started";
    result["threads"] = mining_mgr.getThreadCount();
    result["address"] = address;

    return result;
}

/**
 * mining.stop - Stop CPU mining
 *
 * Stops the integrated CPU miner gracefully.
 * Waits for current hash attempts to complete before stopping.
 *
 * Example:
 * > mining.stop
 * {
 *   "status": "stopped",
 *   "blocks_found": 42,
 *   "total_hashes": 1234567890
 * }
 */
din::Json rpc_mining_stop(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Phase Y: Get MiningManager from DaemonContext
    if (!ctx.daemon || !ctx.daemon->mining) {
        result["error"] = "Mining service not available";
        return result;
    }

    auto mining_service = std::dynamic_pointer_cast<dinero::MiningService>(ctx.daemon->mining);
    if (!mining_service) {
        result["error"] = "Mining service not initialized";
        return result;
    }

    auto& mining_mgr = mining_service->getMiningManager();

    // Check if mining is active
    if (!mining_mgr.isMining()) {
        result["error"] = "Mining is not active";
        result["status"] = "not_mining";
        return result;
    }

    // Stop mining (blocks until threads join)
    mining_mgr.stopMining();

    // Return final stats
    result["status"] = "stopped";
    const std::string metrics_json = mining_mgr.GetMetrics();
    Json::CharReaderBuilder reader;
    Json::Value parsed_metrics;
    std::string parse_errors;
    std::istringstream metrics_stream(metrics_json);
    if (Json::parseFromStream(reader, metrics_stream, &parsed_metrics, &parse_errors) &&
        parsed_metrics.isObject()) {
        result["metrics"] = parsed_metrics;
    } else {
        result["metrics_raw"] = metrics_json;
    }

    return result;
}

/**
 * mining.getstatus - Get mining status and statistics
 *
 * Returns current mining status including hashrate, threads, and performance metrics.
 *
 * Example:
 * > mining.getstatus
 * {
 *   "is_mining": true,
 *   "threads": 4,
 *   "hashrate": 125000.5,
 *   "blocks_found": 42,
 *   "address": "din1q...",
 *   "current_height": 100523,
 *   "uptime_seconds": 3600
 * }
 */
din::Json rpc_mining_getstatus(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Phase Y: Get MiningManager from DaemonContext
    if (!ctx.daemon || !ctx.daemon->mining) {
        result["error"] = "Mining service not available";
        return result;
    }

    auto mining_service = std::dynamic_pointer_cast<dinero::MiningService>(ctx.daemon->mining);
    if (!mining_service) {
        result["error"] = "Mining service not initialized";
        return result;
    }

    auto& mining_mgr = mining_service->getMiningManager();

    // Get mining status
    result["is_mining"] = mining_mgr.isMining();
    result["threads"] = mining_mgr.getThreadCount();
    result["address"] = mining_mgr.getMiningAddress();

    // Get metrics from MiningManager
    // MiningManager::GetMetrics() returns a JSON string
    std::string metrics_json = mining_mgr.GetMetrics();

    // Parse metrics JSON and expose structured fields when possible.
    Json::CharReaderBuilder reader;
    Json::Value parsed_metrics;
    std::string parse_errors;
    std::istringstream metrics_stream(metrics_json);
    if (Json::parseFromStream(reader, metrics_stream, &parsed_metrics, &parse_errors) &&
        parsed_metrics.isObject()) {
        result["metrics"] = parsed_metrics;
    } else {
        result["metrics_raw"] = metrics_json;
    }

    return result;
}

/**
 * mining.setthreads - Adjust thread count
 *
 * Changes the number of mining threads.
 * If mining is active, restarts mining with new thread count.
 *
 * Parameters:
 * - threads (required): Number of threads (0 = auto-detect optimal)
 *
 * Example:
 * > mining.setthreads 8
 * {
 *   "status": "updated",
 *   "threads": 8,
 *   "optimal_threads": 12
 * }
 */
din::Json rpc_mining_setthreads(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Phase Y: Get MiningManager from DaemonContext
    if (!ctx.daemon || !ctx.daemon->mining) {
        result["error"] = "Mining service not available";
        return result;
    }

    auto mining_service = std::dynamic_pointer_cast<dinero::MiningService>(ctx.daemon->mining);
    if (!mining_service) {
        result["error"] = "Mining service not initialized";
        return result;
    }

    auto& mining_mgr = mining_service->getMiningManager();

    // Parse thread count (required parameter)
    if (!params.isArray() || params.size() < 1) {
        result["error"] = "Missing thread count parameter";
        return result;
    }

    int threads = params[0].asInt();
    if (threads < 0 || threads > 256) {
        result["error"] = "Invalid thread count (must be 0-256)";
        return result;
    }

    // Set thread count (MiningManager handles restart if mining is active)
    mining_mgr.setThreadCount(threads);

    // Success
    result["status"] = "updated";
    result["threads"] = mining_mgr.getThreadCount();
    result["optimal_threads"] = mining_mgr.getOptimalThreadCount();

    return result;
}

/**
 * mining.setaddress - Set mining address for coinbase rewards
 *
 * Sets the address where mining rewards will be sent.
 * Must be called before starting mining.
 *
 * Parameters:
 * - address (required): Bech32 Dinero address (din1...)
 *
 * Example:
 * > mining.setaddress "din1p..."
 * {
 *   "status": "updated",
 *   "address": "din1p..."
 * }
 *
 * NOTE: Only Taproot (P2TR) addresses are accepted (din1p...).
 * P2WPKH (din1q...) addresses are rejected to ensure coinbase
 * outputs are always spendable with modern Taproot signing.
 */
din::Json rpc_mining_setaddress(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Phase Y: Get MiningManager from DaemonContext
    if (!ctx.daemon || !ctx.daemon->mining) {
        result["error"] = "Mining service not available";
        return result;
    }

    auto mining_service = std::dynamic_pointer_cast<dinero::MiningService>(ctx.daemon->mining);
    if (!mining_service) {
        result["error"] = "Mining service not initialized";
        return result;
    }

    auto& mining_mgr = mining_service->getMiningManager();

    // Parse address (required parameter)
    if (!params.isArray() || params.size() < 1) {
        result["error"] = "Missing address parameter";
        return result;
    }

    std::string address = params[0].asString();
    if (address.empty()) {
        result["error"] = "Address cannot be empty";
        return result;
    }

    // CRITICAL: Enforce Taproot-only mining addresses (BIP341 P2TR)
    // This matches the policy in generatetoaddress and ensures coinbase
    // outputs are always spendable with modern Taproot signing.
    if (!dinero::mining::IsCoinbaseEligibleAddress(address)) {
        result["error"] = dinero::mining::GetTaprootRequiredMessage(address);
        return result;
    }

    // Set mining address
    mining_mgr.setMiningAddress(address);

    // Success
    result["status"] = "updated";
    result["address"] = address;

    return result;
}

// ═══════════════════════════════════════════════════════════════
// RPC REGISTRATION
// ═══════════════════════════════════════════════════════════════

/**
 * Register mining control RPC methods
 *
 * Called during daemon initialization to register handlers.
 */
void register_mining_control_rpc_methods(RpcRegistry& registry) {
    // mining.start - Start CPU mining
    RpcMethodMeta start_meta;
    start_meta.name = "start";
    start_meta.ns = "mining";
    start_meta.description = "Start CPU mining with N threads";
    start_meta.result.type = "object";
    start_meta.result.desc = "Mining start status";

    registry.registerHandler("mining.start", rpc_mining_start, start_meta, "Phase Y");

    // mining.stop - Stop CPU mining
    RpcMethodMeta stop_meta;
    stop_meta.name = "stop";
    stop_meta.ns = "mining";
    stop_meta.description = "Stop CPU mining";
    stop_meta.result.type = "object";
    stop_meta.result.desc = "Mining stop status";

    registry.registerHandler("mining.stop", rpc_mining_stop, stop_meta, "Phase Y");

    // mining.getstatus - Get mining status
    RpcMethodMeta status_meta;
    status_meta.name = "getstatus";
    status_meta.ns = "mining";
    status_meta.description = "Get mining status and statistics";
    status_meta.result.type = "object";
    status_meta.result.desc = "Mining status";

    registry.registerHandler("mining.getstatus", rpc_mining_getstatus, status_meta, "Phase Y");

    // mining.setthreads - Adjust thread count
    RpcMethodMeta setthreads_meta;
    setthreads_meta.name = "setthreads";
    setthreads_meta.ns = "mining";
    setthreads_meta.description = "Adjust mining thread count";
    setthreads_meta.result.type = "object";
    setthreads_meta.result.desc = "Thread count update status";

    registry.registerHandler("mining.setthreads", rpc_mining_setthreads, setthreads_meta, "Phase Y");

    // mining.setaddress - Set mining address
    RpcMethodMeta setaddress_meta;
    setaddress_meta.name = "setaddress";
    setaddress_meta.ns = "mining";
    setaddress_meta.description = "Set mining address for coinbase rewards";
    setaddress_meta.result.type = "object";
    setaddress_meta.result.desc = "Address update status";

    registry.registerHandler("mining.setaddress", rpc_mining_setaddress, setaddress_meta, "Phase Y");
}
