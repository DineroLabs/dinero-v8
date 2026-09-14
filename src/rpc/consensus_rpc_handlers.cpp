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
 *
 * #741: enumerates every known tip via GetChainTipsSnapshot(), not the
 * ActivateBestChain candidate set. The candidate set drops a tip the moment it
 * is activated and never re-adds it after a reorg abandons it, so the old
 * handler returned exactly one tip after any reorg. Tips whose fork point is
 * more than DEFAULT_CHAINTIPS_FORK_DEPTH blocks below the active tip are
 * omitted to bound the per-call walk.
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
        const CBlockIndex* active_tip = sync.has_active_tip
            ? FindBlockIndex(sync.active_tip_hash)
            : nullptr;

        // Every known tip (active, abandoned/valid forks, header-only, invalid),
        // sorted by chainwork descending (ByWorkThenHash) for stable ordering.
        for (const ChainTipEntry& entry : GetChainTipsSnapshot(active_tip)) {
            din::Json tip_info(Json::objectValue);
            tip_info["height"] = static_cast<int>(entry.tip->height);
            tip_info["hash"] = entry.tip->hash.GetHex();
            tip_info["chainwork"] = entry.tip->chainwork;
            tip_info["status"] = ChainTipStatusName(entry.status);
            tip_info["branchlen"] = static_cast<int>(entry.branchlen);
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
            .help = "getchaintips\n\n"
                    "Return information about all known tips in the block tree, including the main chain, "
                    "branches abandoned by a reorg, header-only branches and invalid branches.\n\n"
                    "Result: array of objects, sorted by chainwork descending:\n"
                    "  height     (numeric) height of the chain tip\n"
                    "  hash       (string)  block hash of the tip\n"
                    "  branchlen  (numeric) 0 for the active tip, otherwise the number of blocks between "
                    "the tip and its fork point with the active chain\n"
                    "  status     (string)  one of:\n"
                    "               active        the active chain tip\n"
                    "               valid-fork    fully validated branch that is not active (e.g. abandoned by a reorg)\n"
                    "               valid-headers block bodies present but never fully validated\n"
                    "               headers-only  headers known, block bodies not downloaded\n"
                    "               invalid       branch contains an invalid block\n"
                    "  chainwork  (string)  total chainwork up to this tip, hex\n\n"
                    "Branches whose fork point is more than 2016 blocks below the active tip are omitted."
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
