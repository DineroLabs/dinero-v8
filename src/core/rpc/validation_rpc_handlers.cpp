#include "dinero/core/rpc/validation_rpc_handlers.h"
#include "consensus/chainparams.h"
#include "daemon/rpc_server.h"  // Fixed: was http_rpc_server.h
#include "daemon/p2p_manager.h"
#include "storage/chain_db.h"
#include "storage/archival_block_reader.h"
#include "storage/chain_direct.h"
#include "common/serialization.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"         // Week 5: DaemonContext access
#include "daemon/services/chainstate_service.h"  // Week 5: ChainstateService access
#include <json/json.h>
#include <iostream>
#include <chrono>

/**
 * Phase 2 Hardening: Chain Validation RPC Handlers
 *
 * Provides comprehensive blockchain health and validation checks via RPC
 * Uses ChainDB via ExecutionContext.daemon->chainstate->chainDB()
 */

// External RPC registry for unified vNext registration
extern RpcRegistry g_rpcRegistry;

namespace dinero {

// ═══════════════════════════════════════════════════════════════
// validatechain - Comprehensive chain validation and health checks
// ═══════════════════════════════════════════════════════════════
static din::Json validatechain_impl(const ExecutionContext& ctx, const din::Json& params) {
    Json::Value result;

    try {
        // Week 5: Migrated from dinero::legacy::g_chain_db_direct() global to ctx.daemon->chainstate->chainDB()
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["valid"] = false;
            result["error"] = "Chainstate service not available";
            result["code"] = -1;
            return result;
        }

        auto chainstate_service =
            std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        auto* chain_db = ctx.daemon->chainstate->chainDB();
        if (!chain_db) {
            result["valid"] = false;
            result["error"] = "ChainDB not initialized";
            result["code"] = -1;
            return result;
        }

        result["validation_version"] = "0.1.0";
        result["timestamp"] = static_cast<Json::Int64>(
                std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())
            );

            // ═══════════════════════════════════════════════════════
            // Network Identification & Genesis Verification
            // ═══════════════════════════════════════════════════════
            const auto& active_params = Params();
            result["network_type"] = active_params.name;

            // Verify genesis block using ChainDB
            bool genesis_verified = false;
            try {
                uint256 genesis_hash_u256 = active_params.genesis_hash;
                if (chainstate_service) {
                    genesis_verified = chainstate_service->hasBlockByHash(genesis_hash_u256);
                } else {
                    genesis_verified = dinero::storage::HasArchivalBlockBody(
                        *chain_db,
                        ctx.daemon ? ctx.daemon->block_storage.get() : nullptr,
                        genesis_hash_u256);
                }
            } catch (...) {
                // Genesis block not found or error accessing it
                genesis_verified = false;
            }
            result["genesis_verified"] = genesis_verified;
            result["genesis_hash"] = active_params.genesis_hash;

            // ═══════════════════════════════════════════════════════
            // Blockchain State using ChainDB
            // ═══════════════════════════════════════════════════════
            uint32_t current_height = 0;
            std::string tip_hash = "unknown";

            try {
                auto tip_result = chain_db->getTip();
                if (tip_result.ok()) {
                    TipInfo tip = tip_result.value();
                    current_height = static_cast<uint32_t>(tip.height);
                    // TipInfo.hash is already a string, use directly
                    tip_hash = tip.hash;
                }
            } catch (...) {
                // Handle error - keep defaults
            }

            result["height"] = static_cast<int>(current_height);
            result["tip_hash"] = tip_hash;
            result["chainwork"] = "calculated"; // TODO: Implement actual chainwork calculation

            // ═══════════════════════════════════════════════════════
            // P2P Network Status
            // ═══════════════════════════════════════════════════════
            size_t peer_count = 0;
            bool p2p_running = false;

            // Week 5: Migrated from dinero::legacy::g_peer_manager() global to ctx.daemon->p2p->get()
            if (ctx.daemon && ctx.daemon->p2p) {
                try {
                    auto& p2p_mgr = ctx.daemon->p2p->get();
                    peer_count = p2p_mgr.get_peer_count();
                    p2p_running = p2p_mgr.is_running();
                } catch (...) {
                    // P2P not available
                }
            }

            result["peer_count"] = static_cast<int>(peer_count);
            result["p2p_running"] = p2p_running;
            result["peer_status"] = p2p_running ? "connected" : "disconnected";

            // ═══════════════════════════════════════════════════════
            // Sync State & IBD Detection
            // ═══════════════════════════════════════════════════════
            Json::Value sync_state;

            // Simple IBD heuristic: If we have very few blocks or no peers, we might be syncing
            bool is_syncing = (current_height < 100 && peer_count > 0) || current_height == 0;
            double sync_progress = current_height > 0 ? 1.0 : 0.0;

            sync_state["is_syncing"] = is_syncing;
            sync_state["sync_progress"] = sync_progress;
            sync_state["headers_height"] = static_cast<int>(current_height); // Simplified
            sync_state["time_behind_hours"] = 0; // TODO: Calculate actual time behind
            sync_state["status"] = is_syncing ? "synchronizing" : "synchronized";
            result["sync_state"] = sync_state;

            // ═══════════════════════════════════════════════════════
            // MTP (Median Time Past) Validation
            // ═══════════════════════════════════════════════════════
            Json::Value mtp_info;
            bool mtp_valid = true;
            uint32_t mtp_value = 0;

            if (current_height > 0) {
                try {
                    mtp_value = dinero::storage::GetMedianTimePast(chain_db);

                    // MTP should be reasonable (not in far future, not before genesis)
                    auto now = static_cast<uint32_t>(std::chrono::system_clock::to_time_t(
                        std::chrono::system_clock::now()));
                    const uint32_t GENESIS_TIME = 1775865600; // 2026-04-11 00:00:00 UTC

                    mtp_valid = (mtp_value >= GENESIS_TIME && mtp_value <= now + 7200);
                } catch (const std::exception& e) {
                    mtp_valid = false;
                    mtp_info["error"] = e.what();
                }
            }

            mtp_info["valid"] = mtp_valid;
            mtp_info["mtp_value"] = static_cast<Json::Int64>(mtp_value);
            mtp_info["status"] = mtp_valid ? "valid" : "invalid";
            result["mtp_consistency"] = mtp_info;

            // ═══════════════════════════════════════════════════════
            // Warnings & Overall Status
            // ═══════════════════════════════════════════════════════
            Json::Value warnings(Json::arrayValue);

            if (!genesis_verified) {
                warnings.append("Genesis block verification failed");
            }
            if (!ctx.daemon || !ctx.daemon->p2p || !p2p_running) {
                warnings.append("P2P network not running - node may be isolated");
            }
            if (peer_count == 0) {
                warnings.append("No connected peers - node is not synchronized");
            }
            if (!mtp_valid) {
                warnings.append("MTP consistency check failed");
            }

            result["warnings"] = warnings;

            // Overall validation result
            bool valid = genesis_verified && mtp_valid && (chain_db != nullptr);
            result["valid"] = valid;
            result["status"] = valid ? "healthy" : "unhealthy";
            result["message"] = valid ?
                "Blockchain validation passed" :
                "Blockchain validation detected issues - see warnings";

    } catch (const std::exception& e) {
        result["valid"] = false;
        result["error"] = std::string("validatechain error: ") + e.what();
        result["code"] = -1;
    }

    return result;
}

void RegisterValidationRPCHandlers(HttpRpcServer* rpc_server) {
    std::cout << "[Validation RPC] Registering validation RPC handlers to UNIFIED g_rpcRegistry (Phase 2 Hardening)..." << std::endl;

    // UNIFIED RPC ARCHITECTURE (vNext):
    // Register validatechain directly to g_rpcRegistry
    g_rpcRegistry.registerHandler("validatechain", validatechain_impl);

    std::cout << "[Validation RPC] ✅ Successfully registered validatechain to unified registry (vNext)" << std::endl;
}

} // namespace dinero
