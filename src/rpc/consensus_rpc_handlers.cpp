#include "rpc/consensus_rpc_handlers.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"  // Phase 39: Use ChainstateService instead of ChainManager
#include "consensus/block_index.h"
#include "consensus/chainwork.h"
#include "common/logger.h"
#include "din_json.h"
#include <vector>
#include <algorithm>

namespace dinero {

/**
 * getchaintips - Returns information about all known tips in the block tree
 * 
 * Result:
 * [
 *   {
 *     "height": n,           // height of the chain tip
 *     "hash": "hex",         // block hash of the tip
 *     "branchlen": n,        // zero for main chain, length of branch for others
 *     "status": "str"        // "active" for the main chain, "valid-fork", "valid-headers", "headers-only", "invalid"
 *     "chainwork": "hex"     // total chainwork for this tip
 *   },
 *   ...
 * ]
 */
din::Json rpc_getchaintips(const ExecutionContext& ctx, const din::Json& params) {
    try {
        din::Json result(Json::arrayValue);

        // Phase 39: Access ChainManager via ChainstateService (no globals)
        auto chainstate = std::dynamic_pointer_cast<ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            throw std::runtime_error("Chainstate service not available");
        }

        const auto sync = chainstate->GetSyncSnapshot();
        CBlockIndex* active_tip = sync.has_active_tip
            ? FindBlockIndex(sync.active_tip_hash)
            : nullptr;
        
        // Get all candidate tips
        std::vector<CBlockIndex*> tips = GetCandidateTipsSnapshot();
        if (active_tip && std::find(tips.begin(), tips.end(), active_tip) == tips.end()) {
            tips.push_back(active_tip);
        }
        
        // Sort by chainwork (descending) for consistent ordering
        std::sort(tips.begin(), tips.end(), [](const CBlockIndex* a, const CBlockIndex* b) {
            return chainwork::CompareWork(a->chainwork, b->chainwork) > 0;
        });
        
        for (CBlockIndex* tip : tips) {
            din::Json tip_info(Json::objectValue);
            tip_info["height"] = static_cast<int>(tip->height);
            tip_info["hash"] = tip->hash.GetHex();
            tip_info["chainwork"] = tip->chainwork;
            
            // Determine status and branch length
            if (tip == active_tip) {
                tip_info["status"] = "active";
                tip_info["branchlen"] = 0;
            } else {
                tip_info["status"] = "valid-fork";
                
                // Calculate branch length (distance from common ancestor)
                int branch_len = 0;
                if (active_tip) {
                    CBlockIndex* fork = chainstate->FindFork(active_tip, tip);
                    branch_len = tip->height - (fork ? fork->height : 0);
                }
                tip_info["branchlen"] = branch_len;
            }
            
            result.append(tip_info);
        }
        
        return result;
        
    } catch (const std::exception& e) {
        din::Json error(Json::objectValue);
        error["code"] = -1;
        error["message"] = std::string("getchaintips error: ") + e.what();
        
        din::Json response(Json::objectValue);
        response["error"] = error;
        return response;
    }
}

/**
 * getchainwork - Returns the total chainwork for the active tip
 * 
 * Result: "hex"  // chainwork as hex string
 */
din::Json rpc_getchainwork(const ExecutionContext& ctx, const din::Json& params) {
    try {
        // Phase 39: Access ChainManager via ChainstateService (no globals)
        auto chainstate = std::dynamic_pointer_cast<ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            throw std::runtime_error("Chainstate service not available");
        }

        const auto sync = chainstate->GetSyncSnapshot();
        CBlockIndex* active_tip = sync.has_active_tip
            ? FindBlockIndex(sync.active_tip_hash)
            : nullptr;
        if (!active_tip) {
            return "0000000000000000000000000000000000000000000000000000000000000000";
        }

        return active_tip->chainwork;
        
    } catch (const std::exception& e) {
        din::Json error(Json::objectValue);
        error["code"] = -1;
        error["message"] = std::string("getchainwork error: ") + e.what();
        
        din::Json response(Json::objectValue);
        response["error"] = error;
        return response;
    }
}

/**
 * getreorgstatus - Returns information about the last reorganization
 * 
 * Result:
 * {
 *   "boot_id": "hex",          // process-lifetime reorg-log identity
 *   "total": n,                // reorgs observed during this process lifetime
 *   "last_reorg": {
 *     "seq": n,                   // monotonically increasing event sequence
 *     "timestamp": "str",        // event timestamp
 *     "disconnect_depth": n,      // number of blocks disconnected
 *     "connect_depth": n          // number of blocks connected
 *   },
 *   "safe_mode": {
 *     "active": bool,            // whether safe mode is active
 *     "reason": "str"            // reason for safe mode (if active)
 *   }
 * }
 */
din::Json rpc_getreorgstatus(const ExecutionContext& ctx, const din::Json& params) {
    try {
        din::Json result(Json::objectValue);

        // Phase 39: Access ChainManager via ChainstateService (no globals)
        auto chainstate = std::dynamic_pointer_cast<ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            throw std::runtime_error("Chainstate service not available");
        }

        // The bounded process-lifetime reorg log is the authoritative source.
        // It is copied atomically so total and events cannot describe different
        // instants.
        const auto snapshot = chainstate->GetReorgLog().Take();
        result["boot_id"] = snapshot.boot_id;
        result["total"] = static_cast<Json::UInt64>(snapshot.total);
        if (snapshot.events.empty()) {
            result["last_reorg"] = Json::nullValue;
        } else {
            const auto& event = snapshot.events.back();
            din::Json reorg_info(Json::objectValue);
            reorg_info["seq"] = static_cast<Json::UInt64>(event.seq);
            reorg_info["timestamp"] = event.timestamp;
            reorg_info["disconnect_depth"] = static_cast<Json::UInt>(event.disconnected);
            reorg_info["connect_depth"] = static_cast<Json::UInt>(event.connected);
            result["last_reorg"] = reorg_info;
        }

        // Safe mode status
        din::Json safe_mode(Json::objectValue);
        const bool safe_mode_active = chainstate->IsInSafeMode();
        safe_mode["active"] = safe_mode_active;
        safe_mode["reason"] = safe_mode_active ? chainstate->GetSafeModeReason() : "";
        
        result["safe_mode"] = safe_mode;
        
        return result;
        
    } catch (const std::exception& e) {
        din::Json error(Json::objectValue);
        error["code"] = -1;
        error["message"] = std::string("getreorgstatus error: ") + e.what();
        
        din::Json response(Json::objectValue);
        response["error"] = error;
        return response;
    }
}

/**
 * Register all consensus RPC handlers
 */
void RegisterConsensusRPCHandlers(RpcRegistry& registry) {
    // getchaintips
    registry.registerHandler("blockchain.getchaintips", rpc_getchaintips,
        RpcMethodMeta{
            .name = "getchaintips",
            .ns = "blockchain",
            .description = "Return information about all known tips in the block tree",
            .params = {},
            .result = {"array", "Array of chain tip objects"},
            .help = "getchaintips\n\nReturn information about all known tips in the block tree, including the main chain and any valid forks."
        }, "Consensus introspection");
    
    // getchainwork  
    registry.registerHandler("blockchain.getchainwork", rpc_getchainwork,
        RpcMethodMeta{
            .name = "getchainwork",
            .ns = "blockchain", 
            .description = "Returns the total chainwork for the active tip",
            .params = {},
            .result = {"string", "Chainwork as hex string"},
            .help = "getchainwork\n\nReturns the total accumulated proof-of-work for the active chain tip."
        }, "Consensus introspection");
    
    // getreorgstatus
    registry.registerHandler("blockchain.getreorgstatus", rpc_getreorgstatus,
        RpcMethodMeta{
            .name = "getreorgstatus", 
            .ns = "blockchain",
            .description = "Returns information about the last reorganization and safe mode status",
            .params = {},
            .result = {"object", "Reorganization status information"},
            .help = "getreorgstatus\n\nReturns detailed information about the last blockchain reorganization and current safe mode status."
        }, "Consensus introspection");
    
    dinero::g_logger.info("Registered consensus RPC handlers: getchaintips, getchainwork, getreorgstatus");
}

} // namespace dinero
