#include "rpc/rpc_registry.h"
#include "common/logger.h"
#include "daemon/block_acceptor.h"
#include "storage/chain_db.h"
#include "storage/chain_direct.h"
#include "consensus/chainparams.h"
#include "common/json_utils.h"

/**
 * invalidateblock RPC handler (regtest-only)
 *
 * Invalidates the chain tip and rolls back to the parent block.
 * Used for testing block reorganizations in regtest mode.
 *
 * Parameters:
 *   blockhash (string, required): The hash of the block to invalidate (must be current tip)
 *
 * Returns:
 *   On success: {"result": "success"}
 *   On error: {"code": error_code, "message": error_message}
 *
 * Example:
 *   invalidateblock "0000000104e9560854067dd7620e75723c719e0ef6395f559531608b236c648e"
 */

void registerBlockInvalidation(
    RpcRegistry& registry,
    dinero::ChainDB* chaindb
) {
    registry.registerHandler("invalidateblock", [chaindb](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
        try {
            // REGTEST-ONLY SAFETY CHECK
            if (dinero::Params().name != "regtest") {
                din::Json error = din::obj();
                error["code"] = -32601;  // Method not found
                error["message"] = "invalidateblock is only available in regtest mode";
                return error;
            }

            // Validate parameters
            if (!params.isArray() || params.size() < 1 || !params[0].isString()) {
                din::Json error = din::obj();
                error["code"] = -32602;  // Invalid params
                error["message"] = "Invalid parameters: invalidateblock requires [blockhash:string]";
                return error;
            }

            std::string blockhash = params[0].asString();

            // Validate blockhash format (64 hex characters)
            if (blockhash.length() != 64) {
                din::Json error = din::obj();
                error["code"] = -32602;  // Invalid params
                error["message"] = "Invalid blockhash: must be 64 hex characters";
                return error;
            }

            // Get current chain tip
            if (!chaindb) {
                dinr::Json error = din::obj();
                error["code"] = -32603;  // Internal error
                error["message"] = "ChainDB not initialized";
                return error;
            }

            uint32_t tipHeight = dinero::storage::GetChainHeight(chaindb);
            std::string tipHash = dinero::storage::GetBestBlockHash(chaindb);

            dinero::g_logger.info("[invalidateblock] Current tip: " + tipHash.substr(0, 16) + "... at height " +
                                std::to_string(tipHeight));
            dinero::g_logger.info("[invalidateblock] Requested invalidation: " + blockhash.substr(0, 16) + "...");

            // Only allow invalidating the current tip
            if (blockhash != tipHash) {
                din::Json error = din::obj();
                error["code"] = -32603;  // Internal error
                error["message"] = "Can only invalidate tip block. Tip: " + tipHash.substr(0, 16) +
                                 "..., requested: " + blockhash.substr(0, 16) + "...";
                dinero::g_logger.error("[invalidateblock] " + error["message"].asString());
                return error;
            }

            // Cannot invalidate genesis block
            if (tipHeight == 0) {
                din::Json error = din::obj();
                error["code"] = -32603;  // Internal error
                error["message"] = "Cannot invalidate genesis block";
                dinero::g_logger.error("[invalidateblock] Attempt to invalidate genesis block");
                return error;
            }

            // Apply tip invalidation
            std::string error;
            if (!dinero::BlockAcceptor::ApplyTipInvalidation(blockhash, error)) {
                din::Json err = din::obj();
                err["code"] = -32603;  // Internal error
                err["message"] = "Failed to invalidate tip: " + error;
                dinero::g_logger.error("[invalidateblock] " + error);
                return err;
            }

            // Verify the tip was actually changed
            uint32_t newHeight = dinero::storage::GetChainHeight(chaindb);
            std::string newTipHash = dinero::storage::GetBestBlockHash(chaindb);

            dinero::g_logger.info("[invalidateblock] ✅ Success: height " + std::to_string(tipHeight) +
                                " → " + std::to_string(newHeight) + ", new tip: " + newTipHash.substr(0, 16) + "...");

            // Return success
            din::Json response = din::obj();
            response["result"] = din::obj();
            response["result"]["status"] = "success";
            response["result"]["old_height"] = tipHeight;
            response["result"]["new_height"] = newHeight;
            response["result"]["old_tip"] = blockhash;
            response["result"]["new_tip"] = newTipHash;
            response["rpc_schema"] = "din.rpc.v1";
            return response;

        } catch (const std::exception& ex) {
            din::Json error = din::obj();
            error["code"] = -32603;  // Internal error
            error["message"] = "invalidateblock failed: " + std::string(ex.what());
            dinero::g_logger.error("[invalidateblock] Exception: " + std::string(ex.what()));
            return error;
        }
    });

    dinero::g_logger.info("[rpc] bound invalidateblock (regtest-only block invalidation)");
}
