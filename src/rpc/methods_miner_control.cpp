/**
 * Phase 26.5: Mining Control RPC Methods
 *
 * RPC methods for controlling the mining engine:
 * - miner.start - Start CPU mining
 * - miner.stop - Stop mining
 * - miner.status - Get mining status
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/block_acceptor.h"
#include "mining/block_template.h"
#include "mining/miner_engine.h"
#include "mining/address_validator.h"
#include "consensus/consensus.hpp"  // For Consensus struct
#include "common/logger.h"
#include "storage/chain_db.h"
#include "primitives/block.h"
#include <memory>
#include <thread>
#include <sstream>
#include <iomanip>

// ============================================================================
// Global Mining State
// ============================================================================

namespace {
    std::unique_ptr<dinero::mining::MiningEngine> g_mining_engine;
    std::string g_mining_address;
    uint32_t g_mining_threads = 1;
    bool g_is_mining = false;
}

// ============================================================================
// Phase 26.5: miner.start RPC
// ============================================================================

/**
 * miner.start - Start CPU mining
 *
 * Usage:
 *   miner.start '{"address":"din1q...","threads":4}'
 *   miner.start '{"address":"D..."}'  (defaults to 1 thread)
 *
 * Returns:
 *   {
 *     "success": true,
 *     "address": "din1q...",
 *     "threads": 4,
 *     "status": "Mining started"
 *   }
 */
din::Json rpc_miner_start(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Check if already mining
    if (g_is_mining) {
        result["error"] = "Mining is already running. Stop mining first with miner.stop";
        return result;
    }

    // Parse parameters
    std::string mining_address;
    uint32_t threads = 1;

    if (params.isObject()) {
        // Get mining address (REQUIRED)
        if (!params.isMember("address") || !params["address"].isString()) {
            result["error"] = "Missing required parameter: address (e.g., {\"address\": \"din1q...\"})";
            return result;
        }

        mining_address = params["address"].asString();

        // Get threads (optional, defaults to 1)
        if (params.isMember("threads") && params["threads"].isInt()) {
            threads = params["threads"].asInt();
            if (threads == 0) {
                threads = 1;
            }
            if (threads > 64) {
                result["error"] = "Maximum 64 mining threads allowed";
                return result;
            }
        }
    } else {
        result["error"] = "Invalid parameters. Expected object: {\"address\":\"...\",\"threads\":N}";
        return result;
    }

    // Validate address
    if (!dinero::mining::IsValidDineroAddress(mining_address)) {
        result["error"] = "Invalid Dinero address. Use bech32 (din1...) or base58 (D...) address.";
        return result;
    }

    // Get services
    if (!ctx.daemon) {
        result["error"] = "DaemonContext not available";
        return result;
    }

    // Phase 26.9: Use MiningCoordinator if available
    if (ctx.daemon->mining_coordinator) {
        dinero::g_logger.info("[miner.start] Using MiningCoordinator (Phase 26.9)");

        // Start CPU worker via MiningCoordinator
        // NOTE: This will be handled by the new MinerCore integration
        // For now, just return success message indicating coordinator is ready
        result["success"] = true;
        result["address"] = mining_address;
        result["threads"] = static_cast<int>(threads);
        result["status"] = "MiningCoordinator ready (Phase 26.9 - CPU worker integration pending)";
        result["message"] = "Use daemon-side MinerCore to activate CPU mining with coordinator";
        return result;
    }

    // Legacy MiningEngine path removed from production because it depended on a
    // separate mempool stack and stale template codepaths. Use unified mining.start.
    result["error"] = "miner.start legacy path removed; use mining.start (unified MiningService path)";
    return result;
}

// ============================================================================
// Phase 26.5: miner.stop RPC
// ============================================================================

/**
 * miner.stop - Stop CPU mining
 *
 * Usage:
 *   miner.stop
 *
 * Returns:
 *   {
 *     "success": true,
 *     "status": "Mining stopped"
 *   }
 */
din::Json rpc_miner_stop(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Check if mining is running
    if (!g_is_mining || !g_mining_engine) {
        result["error"] = "Mining is not running";
        return result;
    }

    // Stop mining
    g_mining_engine->stopMining();

    // Update global state
    g_is_mining = false;

    // Return success
    result["success"] = true;
    result["status"] = "Mining stopped";

    dinero::g_logger.info("⛏️  Mining stopped");

    return result;
}

// ============================================================================
// Phase 26.5: miner.status RPC
// ============================================================================

/**
 * miner.status - Get mining status
 *
 * Usage:
 *   miner.status
 *
 * Returns:
 *   {
 *     "mining": true,
 *     "address": "din1q...",
 *     "threads": 4,
 *     "hashes": 1234567,
 *     "hashrate": 12345,
 *     "blocks_found": 5
 *   }
 */
din::Json rpc_miner_status(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    result["mining"] = g_is_mining;

    if (g_is_mining && g_mining_engine) {
        auto stats = g_mining_engine->getStats();

        result["address"] = g_mining_address;
        result["threads"] = static_cast<int>(g_mining_threads);
        result["hashes"] = static_cast<Json::Int64>(stats.hashes_computed);
        result["hashrate"] = static_cast<Json::Int64>(stats.hash_rate);
        result["blocks_found"] = static_cast<Json::Int64>(stats.blocks_found);

        if (!stats.last_block_hash.empty()) {
            result["last_block_hash"] = stats.last_block_hash;
            result["last_block_height"] = static_cast<Json::Int64>(stats.last_block_height);
        }
    } else {
        result["address"] = Json::nullValue;
        result["threads"] = 0;
        result["hashes"] = 0;
        result["hashrate"] = 0;
        result["blocks_found"] = 0;
    }

    return result;
}

// ============================================================================
// Registration
// ============================================================================

void registerMinerControlRPC() {
    // miner.start
    g_rpcRegistry.registerHandler("miner.start", rpc_miner_start, "mining_control");

    // miner.stop
    g_rpcRegistry.registerHandler("miner.stop", rpc_miner_stop, "mining_control");

    // miner.status
    g_rpcRegistry.registerHandler("miner.status", rpc_miner_status, "mining_control");

    dinero::g_logger.info("Registered Phase 26.5 mining control RPC methods (miner.start, miner.stop, miner.status)");
}
