#include "rpc/rpc_registry.h"
#include "daemon/services/chainstate_service.h"  // Phase 39: Use ChainstateService instead of ChainManager
#include "consensus/block_index.h"
#include "consensus/chainwork.h"
#include "common/logger.h"
#include "din_json.h"
#include <vector>
#include <algorithm>

using namespace dinero;

extern RpcRegistry g_rpcRegistry;

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

        CBlockIndex* active_tip = chainstate->chainManager().GetTip();
        
        // Get all candidate tips
        std::vector<CBlockIndex*> tips;
        for (CBlockIndex* candidate : g_candidates) {
            tips.push_back(candidate);
        }
        
        // Sort by chainwork (descending) for consistent ordering
        std::sort(tips.begin(), tips.end(), [](const CBlockIndex* a, const CBlockIndex* b) {
            return chainwork::CompareWork(a->chainwork, b->chainwork) > 0;
        });
        
        for (CBlockIndex* tip : tips) {
            din::Json tip_info(Json::objectValue);
            tip_info["height"] = static_cast<int>(tip->height);
            tip_info["hash"] = tip->hash;
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
                    CBlockIndex* fork = chainstate->chainManager().FindFork(active_tip, tip);
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

        CBlockIndex* active_tip = chainstate->chainManager().GetTip();
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
 *   "last_reorg": {
 *     "fork_hash": "hex",        // hash of the fork point
 *     "old_tip": "hex",          // hash of the previous active tip
 *     "new_tip": "hex",          // hash of the new active tip
 *     "disconnect_depth": n,      // number of blocks disconnected
 *     "connect_depth": n,         // number of blocks connected
 *     "duration_ms": n,          // reorganization duration in milliseconds
 *     "mempool_resurrected": n,  // number of transactions resurrected
 *     "mempool_evicted": n       // number of transactions evicted
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

        // Last reorg stats
        const auto& stats = chainstate->chainManager().GetLastReorgStats();
        din::Json reorg_info(Json::objectValue);
        reorg_info["fork_hash"] = stats.fork_point;
        reorg_info["old_tip"] = stats.old_tip;
        reorg_info["new_tip"] = stats.new_tip;
        reorg_info["disconnect_depth"] = static_cast<int>(stats.depth_disconnected);
        reorg_info["connect_depth"] = static_cast<int>(stats.depth_connected);
        reorg_info["duration_ms"] = static_cast<int>(stats.duration.count());
        reorg_info["mempool_resurrected"] = static_cast<int>(stats.mempool_resurrected);
        reorg_info["mempool_evicted"] = static_cast<int>(stats.mempool_evicted);

        result["last_reorg"] = reorg_info;

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
void RegisterConsensusRPCHandlers() {
    // getchaintips
    g_rpcRegistry.registerHandler("getchaintips", rpc_getchaintips,
        RpcMethodMeta{
            .name = "getchaintips",
            .ns = "blockchain",
            .description = "Return information about all known tips in the block tree",
            .params = {},
            .result = {"array", "Array of chain tip objects"},
            .help = "getchaintips\n\nReturn information about all known tips in the block tree, including the main chain and any valid forks."
        });
    
    // getchainwork  
    g_rpcRegistry.registerHandler("getchainwork", rpc_getchainwork,
        RpcMethodMeta{
            .name = "getchainwork",
            .ns = "blockchain", 
            .description = "Returns the total chainwork for the active tip",
            .params = {},
            .result = {"string", "Chainwork as hex string"},
            .help = "getchainwork\n\nReturns the total accumulated proof-of-work for the active chain tip."
        });
    
    // getreorgstatus
    g_rpcRegistry.registerHandler("getreorgstatus", rpc_getreorgstatus,
        RpcMethodMeta{
            .name = "getreorgstatus", 
            .ns = "blockchain",
            .description = "Returns information about the last reorganization and safe mode status",
            .params = {},
            .result = {"object", "Reorganization status information"},
            .help = "getreorgstatus\n\nReturns detailed information about the last blockchain reorganization and current safe mode status."
        });
    
    dinero::g_logger.info("Registered consensus RPC handlers: getchaintips, getchainwork, getreorgstatus");
}
