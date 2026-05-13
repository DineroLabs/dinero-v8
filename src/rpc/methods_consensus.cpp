// Consensus and economic RPC methods for vnext
#include "rpc/rpc_registry.h"
#include "common/logger.h"
#include "din_json.h"
#include "consensus/subsidy.h"
#include "consensus/chainparams.h"  // For Params(), ConsensusChecksum()
#include "storage/chain_db.h"
#include "daemon/daemon_context.h"         // Week 5: DaemonContext access
#include "daemon/services/chainstate_service.h"  // Week 5: ChainstateService access
#include "daemon/services/mining_service.h"      // For mining difficulty
#include <sstream>
#include <iomanip>

// Week 5: Removed incorrect extern declaration - now using ExecutionContext.daemon->chainstate->chainDB()

// Helper to format DIN amounts
static std::string formatDIN(uint64_t una) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8)
        << (double)una / (double)dinero::ConsensusSubsidy::UNA_PER_DIN;
    return oss.str();
}

// getsupply - Get total supply and issuance information
din::Json rpc_getsupply(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        // Week 5: Migrated from dinero::legacy::g_chain_db_direct() global to ctx.daemon->chainstate->chainDB()
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"] = "Chainstate service not available";
            result["rpc_schema"] = "din.rpc.v1";
            return result;
        }

        auto* daemon_ctx = DaemonContext::instance();
        if (!daemon_ctx || !daemon_ctx->chain_manager || !daemon_ctx->chain_manager->GetChainDB()) {
            result["error"] = "ChainDB not initialized";
            result["rpc_schema"] = "din.rpc.v1";
            return result;
        }
        auto* chain_db = daemon_ctx->chain_manager->GetChainDB();

        // Get current blockchain height
        uint32_t h = dinero::storage::GetChainHeight(chain_db);

        // Calculate total issued via consensus function (no premine, pure PoW + tail emission)
        uint64_t total_issued = dinero::ConsensusSubsidy::GetTotalIssuedAtHeight(h);

        // Monetary policy: no hard cap (tail emission), no premine
        result["monetary_policy"] = "PoW mining with tail emission, no premine";

        // Current block reward
        uint64_t current_reward = dinero::ConsensusSubsidy::GetBlockSubsidy(h > 0 ? h : 1).GetUna();
        result["current_block_reward_una"] = static_cast<int64_t>(current_reward);
        result["current_block_reward_din"] = formatDIN(current_reward);

        // Total supply issued so far (all from mining)
        result["total_issued_una"] = static_cast<int64_t>(total_issued);
        result["total_issued_din"] = formatDIN(total_issued);

        result["rpc_schema"] = "din.rpc.v1";

    } catch (const std::exception& e) {
        result["error"] = std::string("getsupply error: ") + e.what();
    }

    return result;
}

// geteconomics - Get economic parameters and monetary policy
din::Json rpc_geteconomics(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        // Week 5: Migrated from dinero::legacy::g_chain_db_direct() global to ctx.daemon->chainstate->chainDB()
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"] = "Chainstate service not available";
            result["rpc_schema"] = "din.rpc.v1";
            return result;
        }

        auto* daemon_ctx = DaemonContext::instance();
        if (!daemon_ctx || !daemon_ctx->chain_manager || !daemon_ctx->chain_manager->GetChainDB()) {
            result["error"] = "ChainDB not initialized";
            result["rpc_schema"] = "din.rpc.v1";
            return result;
        }
        auto* chain_db = daemon_ctx->chain_manager->GetChainDB();

        // Get current blockchain height
        uint32_t current_height = dinero::storage::GetChainHeight(chain_db);

        // Dinero mainnet monetary policy
        result["algorithm"] = "Dinero";
        result["units_per_din"] = static_cast<int64_t>(dinero::ConsensusSubsidy::UNA_PER_DIN);
        result["smallest_unit"] = "una";

        // Monetary policy: no premine, no hard cap (tail emission)
        result["monetary_policy"] = "PoW mining with tail emission, no premine";
        result["tail_emission_una"] = static_cast<int64_t>(dinero::ConsensusSubsidy::TAIL_EMISSION_UNA);

        // Current epoch and reward (PoW starts at height 1)
        uint32_t pow_blocks = (current_height > 0) ? (current_height - 1) : 0;
        uint32_t current_epoch = pow_blocks / dinero::ConsensusSubsidy::HALVING_INTERVAL;
        uint64_t current_reward = dinero::ConsensusSubsidy::GetBlockSubsidy(current_height).GetUna();

        result["current_height"] = static_cast<int>(current_height);
        result["current_epoch"] = static_cast<int>(current_epoch);
        result["current_reward_una"] = static_cast<int64_t>(current_reward);
        result["current_reward_din"] = formatDIN(current_reward);

        // Next halving info
        uint32_t halving_interval = dinero::ConsensusSubsidy::HALVING_INTERVAL;
        uint32_t next_halving_height = 1 + (current_epoch + 1) * halving_interval;
        uint64_t next_reward = dinero::ConsensusSubsidy::GetBlockSubsidy(next_halving_height).GetUna();
        uint32_t blocks_until_halving = (next_halving_height > current_height) ?
            (next_halving_height - current_height) : 0;

        result["next_halving_height"] = static_cast<int>(next_halving_height);
        result["next_reward_una"] = static_cast<int64_t>(next_reward);
        result["next_reward_din"] = formatDIN(next_reward);
        result["blocks_until_halving"] = static_cast<int>(blocks_until_halving);

        // Halving schedule
        result["halving_interval"] = static_cast<int>(halving_interval);
        result["initial_subsidy_una"] = static_cast<int64_t>(dinero::ConsensusSubsidy::INITIAL_SUBSIDY);

        result["rpc_schema"] = "din.rpc.v1";

        dinero::g_logger.debug("geteconomics: epoch=" + std::to_string(current_epoch) +
                               " height=" + std::to_string(current_height));

    } catch (const std::exception& e) {
        result["error"] = std::string("geteconomics error: ") + e.what();
    }

    return result;
}

// consensus.checkdb - Database integrity check
din::Json rpc_consensus_checkdb(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        // Week 5: Migrated from dinero::legacy::g_chain_db_direct() global to ctx.daemon->chainstate->chainDB()
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["healthy"] = false;
            result["error"] = "Chainstate service not available";
            result["rpc_schema"] = "din.rpc.v1";
            return result;
        }

        auto* chain_db = ctx.daemon->chainstate->chainDB();
        if (!chain_db) {
            result["healthy"] = false;
            result["error"] = "ChainDB not initialized";
            result["rpc_schema"] = "din.rpc.v1";
            return result;
        }

        // Get tip
        auto tip_result = chain_db->getTip();
        if (tip_result.status() != dinero::Status::Ok) {
            result["healthy"] = false;
            result["error"] = "Failed to get chain tip";
            result["rpc_schema"] = "din.rpc.v1";
            return result;
        }

        const auto& tip = tip_result.value();

        // Verify tip header exists
        auto header_result = chain_db->getHeader(tip.hash);
        if (header_result.status() != dinero::Status::Ok) {
            result["healthy"] = false;
            result["error"] = "Tip hash has no corresponding header";
            result["tip_hash"] = tip.hash.substr(0, 16) + "...";
            result["rpc_schema"] = "din.rpc.v1";
            return result;
        }

        const auto& header = header_result.value();

        // Basic sanity checks
        if (header.height != tip.height) {
            result["healthy"] = false;
            result["error"] = "Header height mismatch with tip";
            result["header_height"] = static_cast<int>(header.height);
            result["tip_height"] = static_cast<int>(tip.height);
            result["rpc_schema"] = "din.rpc.v1";
            return result;
        }

        // All checks passed
        result["healthy"] = true;
        result["tip_height"] = static_cast<int>(tip.height);
        result["tip_hash"] = tip.hash;
        result["message"] = "Database integrity check passed";
        result["rpc_schema"] = "din.rpc.v1";

        dinero::g_logger.info("consensus.checkdb: Database healthy at height " + std::to_string(tip.height));

    } catch (const std::exception& e) {
        result["healthy"] = false;
        result["error"] = std::string("consensus.checkdb error: ") + e.what();
        result["rpc_schema"] = "din.rpc.v1";
    }

    return result;
}

// getconsensusinfo - Get active consensus fingerprint for validation
din::Json rpc_getconsensusinfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        // Get chain parameters
        const auto& chainparams = dinero::Params();

        // Network identification
        result["network"] = chainparams.name;
        result["network_id"] = chainparams.network_id;
        result["hrp"] = chainparams.hrp;

        // Genesis block fingerprint
        result["genesis_hash"] = chainparams.genesis_hash;
        result["genesis_bits"] = chainparams.genesis.nBits;
        result["genesis_time"] = static_cast<int64_t>(chainparams.genesis.nTime);
        result["genesis_nonce"] = chainparams.genesis.nNonce;

        // PoW parameters
        std::ostringstream pow_hex;
        pow_hex << "0x" << std::hex << std::setw(8) << std::setfill('0') << chainparams.pow_limit_bits;
        result["pow_limit_bits"] = pow_hex.str();
        result["target_spacing_seconds"] = static_cast<int>(chainparams.target_spacing);

        // Current chain state
        if (ctx.daemon && ctx.daemon->chainstate) {
            auto* chain_db = ctx.daemon->chainstate->chainDB();
            if (chain_db) {
                uint32_t height = dinero::storage::GetChainHeight(chain_db);
                result["current_height"] = static_cast<int>(height);

                auto tip_result = chain_db->getTip();
                if (tip_result.status() == dinero::Status::Ok) {
                    result["best_block_hash"] = tip_result.value().hash;
                }
            }
        }

        // Mining difficulty (if available)
        if (ctx.daemon && ctx.daemon->mining) {
            uint32_t current_bits = ctx.daemon->mining->mining().getDifficulty();
            if (current_bits != 0) {
                std::ostringstream bits_hex;
                bits_hex << "0x" << std::hex << std::setw(8) << std::setfill('0') << current_bits;
                result["current_mining_bits"] = bits_hex.str();
            } else {
                result["current_mining_bits"] = "uninitialized";
            }
        }

        // Economics fingerprint (no premine, tail emission)
        result["initial_subsidy_din"] = formatDIN(dinero::ConsensusSubsidy::INITIAL_SUBSIDY);
        result["halving_interval"] = static_cast<int>(dinero::ConsensusSubsidy::HALVING_INTERVAL);
        result["tail_emission_una"] = static_cast<int64_t>(dinero::ConsensusSubsidy::TAIL_EMISSION_UNA);

        // Consensus checksum (SHA256 of critical params)
        result["consensus_checksum"] = dinero::ConsensusChecksum(chainparams);

        result["rpc_schema"] = "din.rpc.v1";

        dinero::g_logger.info("getconsensusinfo: network=" + chainparams.name +
                              " genesis=" + chainparams.genesis_hash.substr(0, 16) + "...");

    } catch (const std::exception& e) {
        result["error"] = std::string("getconsensusinfo error: ") + e.what();
        result["rpc_schema"] = "din.rpc.v1";
    }

    return result;
}

// Registration function
void registerConsensusRPC() {
    extern RpcRegistry g_rpcRegistry;

    // getsupply
    {
        RpcMethodMeta meta;
        meta.name = "getsupply";
        meta.description = "Get total supply and issuance information";
        meta.category = "blockchain";
        meta.result.type = "object";
        meta.result.desc = "Supply and issuance statistics";

        g_rpcRegistry.registerHandler("getsupply", rpc_getsupply, meta, "vnext_consensus");
    }

    // geteconomics
    {
        RpcMethodMeta meta;
        meta.name = "geteconomics";
        meta.description = "Get economic parameters and monetary policy";
        meta.category = "blockchain";
        meta.result.type = "object";
        meta.result.desc = "Economic parameters including current reward and halving schedule";

        g_rpcRegistry.registerHandler("geteconomics", rpc_geteconomics, meta, "vnext_consensus");
    }

    // consensus.checkdb
    {
        RpcMethodMeta meta;
        meta.name = "consensus.checkdb";
        meta.description = "Run database integrity check";
        meta.category = "blockchain";
        meta.result.type = "object";
        meta.result.desc = "Database health status";

        g_rpcRegistry.registerHandler("consensus.checkdb", rpc_consensus_checkdb, meta, "vnext_consensus");
    }

    // getconsensusinfo
    {
        RpcMethodMeta meta;
        meta.name = "getconsensusinfo";
        meta.description = "Get active consensus parameters and fingerprint for validation";
        meta.category = "blockchain";
        meta.result.type = "object";
        meta.result.desc = "Consensus parameters including network, genesis hash, PoW limits, and checksum";

        g_rpcRegistry.registerHandler("getconsensusinfo", rpc_getconsensusinfo, meta, "vnext_consensus");
    }

    dinero::g_logger.info("✅ Registered 4 consensus RPC methods (getsupply, geteconomics, consensus.checkdb, getconsensusinfo)");
}

// Auto-register at program startup
static auto _consensus_init = (registerConsensusRPC(), 0);
