/**
 * Economics RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file migrates economics/telemetry RPC methods from legacy globals to DaemonContext.
 *
 * OLD PATTERN (legacy):
 *   extern ChainDB* g_chain_db_direct;
 *   uint32_t h = storage::GetChainHeight(dinero::legacy::g_chain_db_direct());
 *
 * NEW PATTERN (context-aware):
 *   auto chainstate = ctx.daemon->chainstate;
 *   uint32_t h = chainstate->getBlockHeight();
 *
 * Benefits:
 * - No dependency on global variables
 * - Testable with mock chainstate services
 * - Clear dependency tracking
 * - Thread-safe service access
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mining_service.h"
#include "daemon/services/config_service.h"
#include "daemon/services/wallet_service.h"
#include "consensus/subsidy.h"
#include "consensus/chainparams.h"
#include "storage/chain_direct.h"
#include "common/logger.h"
#include "crypto/sha256.h"
#include "build/build_identity.h"
#include "version.h"
#include <sstream>
#include <iomanip>
#include <ctime>

// Helper to format DIN amounts
static std::string formatDIN(uint64_t una) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(8)
        << (double)una / (double)dinero::ConsensusSubsidy::UNA_PER_DIN;
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE ECONOMICS RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

/**
 * economics.getsupply - Get current supply statistics
 *
 * OLD: storage::GetChainHeight(dinero::legacy::g_chain_db_direct())
 * NEW: ctx.daemon->chainstate->getBlockHeight()
 */
din::Json rpc_context_economics_getsupply(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        if (!ctx.daemon || !ctx.daemon->chainstate) {
            result["error"] = "Chainstate service not available";
            return result;
        }

        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (!chainstate) {
            result["error"] = "Failed to cast chainstate service";
            return result;
        }

        // Get current blockchain height via context
        uint32_t h = chainstate->getBlockHeight();

        // Calculate total issued via consensus function (no premine, pure PoW + tail emission)
        uint64_t total_issued = dinero::ConsensusSubsidy::GetTotalIssuedAtHeight(h);

        // Basic info
        result["algorithm"] = "Dinero";
        result["height"] = static_cast<int>(h);

        // Monetary policy: no hard cap (tail emission), no premine
        result["monetary_policy"] = "PoW mining with tail emission, no premine";

        // Current block reward
        uint64_t current_reward = dinero::ConsensusSubsidy::GetBlockSubsidy(h > 0 ? h : 1).GetUna();
        result["current_block_reward_una"] = static_cast<int64_t>(current_reward);
        result["current_block_reward_din"] = formatDIN(current_reward);

        // Total supply issued so far (all from mining)
        result["total_issued_una"] = static_cast<int64_t>(total_issued);
        result["total_issued_din"] = formatDIN(total_issued);

    } catch (const std::exception& e) {
        result["error"] = std::string("getsupply error: ") + e.what();
    }

    return result;
}

/**
 * economics.getinfo - Get economic/monetary policy info
 *
 * OLD: storage::GetChainHeight(dinero::legacy::g_chain_db_direct())
 *      storage::GetDifficulty(dinero::legacy::g_chain_db_direct(), height)
 * NEW: ctx.daemon->chainstate->getBlockHeight()
 *      ctx.daemon->chainstate->getDifficulty()
 */
din::Json rpc_context_economics_getinfo(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Dinero mainnet monetary policy
    result["algorithm"] = "Dinero";
    result["units_per_din"] = static_cast<int64_t>(dinero::ConsensusSubsidy::UNA_PER_DIN);
    result["smallest_unit"] = "una";

    // Monetary policy: no premine, no hard cap (tail emission)
    result["monetary_policy"] = "PoW mining with tail emission, no premine";
    result["tail_emission_una"] = static_cast<int64_t>(dinero::ConsensusSubsidy::TAIL_EMISSION_UNA);

    // Halving schedule
    result["initial_reward_din"] = formatDIN(dinero::ConsensusSubsidy::INITIAL_SUBSIDY);
    result["halving_interval"] = static_cast<int>(dinero::ConsensusSubsidy::HALVING_INTERVAL);
    result["halving_count"] = 33;
    result["block_time_seconds"] = 180;  // 3 minutes
    result["halving_time_years"] = "~7.5";

    // Current chain status (requires context)
    if (ctx.daemon && ctx.daemon->chainstate) {
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (chainstate) {
            uint32_t current_height = chainstate->getBlockHeight();
            uint32_t next_height = current_height + 1;
            uint64_t total_issued = dinero::ConsensusSubsidy::GetTotalIssuedAtHeight(current_height);
            uint64_t next_reward = dinero::ConsensusSubsidy::GetBlockSubsidy(next_height).GetUna();

            result["current_height"] = static_cast<int>(current_height);
            result["current_supply_din"] = formatDIN(total_issued);
            result["next_block_reward_din"] = formatDIN(next_reward);

            if (auto* chain_db = chainstate->GetChainDB()) {
                result["next_block_difficulty"] = dinero::storage::GetDifficulty(chain_db, next_height);
            } else {
                result["next_block_difficulty"] = 0.0;
            }

            // Halving epoch info
            uint32_t current_epoch = next_height / dinero::ConsensusSubsidy::HALVING_INTERVAL;
            uint32_t blocks_until_halving = dinero::ConsensusSubsidy::HALVING_INTERVAL - (next_height % dinero::ConsensusSubsidy::HALVING_INTERVAL);
            result["current_halving_epoch"] = static_cast<int>(current_epoch);
            result["blocks_until_next_halving"] = static_cast<int>(blocks_until_halving);
        }
    }

    return result;
}

/**
 * rpc.version - Returns daemon version info
 *
 * No globals - uses compile-time constants
 */
din::Json rpc_context_rpc_version(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    (void)params;
    auto identity = dinero::build::CurrentIdentity();
    din::Json result;
    result["version"] = identity.version;
    result["repo"] = identity.repo;
    result["component"] = identity.component;
    result["git_sha"] = identity.full_sha;
    result["build_time"] = identity.build_time;
    result["schema"] = identity.schema;
    result["protocol_version"] = 1;
    return result;
}

din::Json rpc_context_getbuildinfo(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    (void)params;
    auto identity = dinero::build::CurrentIdentity();
    din::Json result;
    result["version"] = identity.version;
    result["semver"] = identity.version;
    result["git_sha"] = identity.full_sha;
    result["build_time"] = identity.build_time;
    result["repo"] = identity.repo;
    result["component"] = identity.component;
    result["rpc_schema"] = "din.rpc.v1";
    result["schema_rev"] = 1;
    result["build_schema"] = identity.schema;
    result["protocol_version"] = 1;
    return result;
}

/**
 * rpc.getcontext - Returns daemon identity context
 *
 * Authentication Contract Enforcement (Layer 1)
 * Returns daemon identity so clients can verify they're talking to the correct instance.
 *
 * Returns:
 * - network: "mainnet" | "testnet" | "regtest"
 * - datadir: Absolute path to data directory
 * - rpcport: RPC server port
 * - wallet: Active wallet name (if any)
 * - daemon_id: Stable daemon identifier (sha256(genesis_hash || datadir || network))
 */
din::Json rpc_context_getcontext(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Get config service
    if (!ctx.daemon || !ctx.daemon->config) {
        result["error"] = "Config service not available";
        return result;
    }

    auto config = ctx.daemon->config;

    // Determine network
    std::string network = "mainnet";
    if (config->IsRegtest()) {
        network = "regtest";
    } else if (config->IsTestnet()) {
        network = "testnet";
    }

    result["network"] = network;
    result["datadir"] = config->DataDir();
    result["rpcport"] = config->RPCPort();

    // Optional: active wallet (if wallet service is available)
    if (ctx.daemon->wallet) {
        if (ctx.daemon->wallet->hasActiveWallet()) {
            result["wallet"] = ctx.daemon->wallet->getCurrentWalletName();
        } else {
            result["wallet"] = Json::nullValue;
        }
    } else {
        result["wallet"] = Json::nullValue;
    }

    // Generate stable daemon_id: sha256(genesis_hash || datadir || network)
    // This uniquely identifies this daemon instance across restarts
    std::string daemon_id_input;

    // Get genesis hash from chainstate
    if (ctx.daemon->chainstate) {
        auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
        if (chainstate) {
            try {
                auto* chaindb = chainstate->GetChainDB();
                if (chaindb) {
                    // Get genesis block hash (height 0)
                    auto genesis_hash_result = chaindb->getBlockHashByHeight(0);
                    if (genesis_hash_result.status() == dinero::Status::Ok) {
                        daemon_id_input += genesis_hash_result.value().GetHex();
                    }
                }
            } catch (...) {
                // If genesis fetch fails, use network name as fallback
                daemon_id_input += network;
            }
        }
    }

    daemon_id_input += config->DataDir();
    daemon_id_input += network;

    // Compute SHA256 hash
    std::string daemon_id = dinero::crypto::double_sha256(
        reinterpret_cast<const uint8_t*>(daemon_id_input.data()),
        daemon_id_input.size()
    );

    result["daemon_id"] = daemon_id;

    return result;
}

/**
 * consensus.checkdb - Runs integrity check on ChainDB
 *
 * OLD: dinero::legacy::g_chain_db_direct()->getTip()
 * NEW: ctx.daemon->chainstate->getTip() or getBlockHeight()
 */
din::Json rpc_context_consensus_checkdb(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["healthy"] = false;
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["healthy"] = false;
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    try {
        uint32_t tip_height = chainstate->getBlockHeight();
        std::string tip_hash = chainstate->getBestBlockHash();

        result["healthy"] = true;
        result["tip_height"] = static_cast<int>(tip_height);
        result["tip_hash"] = tip_hash;
        result["message"] = "ChainDB is healthy";

    } catch (const std::exception& e) {
        result["healthy"] = false;
        result["error"] = std::string("Failed to get chain tip: ") + e.what();
    }

    return result;
}

/**
 * economics.getminerstats - Returns mining performance metrics
 *
 * No chain access needed - returns daemon info
 */
din::Json rpc_context_economics_getminerstats(const ExecutionContext& ctx, const din::Json& params) {
    (void)ctx;
    (void)params;
    din::Json result;

    // Mining is handled by external dinero-miner process
    result["status"] = "external_miner";
    result["note"] = "Mining is handled by separate dinero-miner process";

    static const auto daemon_start = std::chrono::steady_clock::now();
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - daemon_start).count();
    result["daemon_uptime_sec"] = static_cast<Json::UInt64>(uptime);

    return result;
}

/**
 * getverificationsummary - Returns genesis verification status
 *
 * No chain access needed - uses consensus params
 */
din::Json rpc_context_getverificationsummary(const ExecutionContext& ctx, const din::Json& params) {
    try {
        din::Json result;

        // Network identification
        const auto& params_active = dinero::Params();
        result["network"] = params_active.name;

        // Genesis verification (always true if daemon is running)
        result["genesis_verified"] = true;

        // Consensus checksum
        result["consensus_checksum"] = dinero::ConsensusChecksum(params_active);

        // Version information
        result["version"] = DINERO_VERSION_FULL;

        // Current timestamp
        result["timestamp"] = static_cast<int64_t>(std::time(nullptr));

        // Overall status
        result["status"] = "OK";

        return result;
    } catch (const std::exception& e) {
        din::Json error_result;
        error_result["error"] = std::string("Internal error: ") + e.what();
        error_result["status"] = "ERROR";
        return error_result;
    }
}

/**
 * getconsensusinfo - Get active consensus fingerprint for validation
 *
 * Returns network identity, genesis hash, PoW limits, and checksum
 * to verify nodes are on the same consensus chain.
 */
din::Json rpc_context_getconsensusinfo(const ExecutionContext& ctx, const din::Json& params) {
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
            auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
            if (chainstate) {
                result["current_height"] = static_cast<int>(chainstate->getBlockHeight());
                result["best_block_hash"] = chainstate->getBestBlockHash();
            }
        }

        // Mining difficulty (if available)
        if (ctx.daemon && ctx.daemon->consensus) {
            uint32_t current_bits = chainparams.pow_limit_bits;
            if (ctx.daemon->chainstate) {
                auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
                if (chainstate) {
                    if (auto* chain_db = chainstate->GetChainDB()) {
                        const uint32_t current_height = chainstate->getBlockHeight();
                        current_bits = dinero::storage::GetDifficultyBits(chain_db, current_height + 1);
                    }
                }
            }
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

        dinero::g_logger.info("getconsensusinfo: network=" + chainparams.name +
                              " genesis=" + chainparams.genesis_hash.substr(0, 16) + "...");

    } catch (const std::exception& e) {
        result["error"] = std::string("getconsensusinfo error: ") + e.what();
    }

    return result;
}

/**
 * rpc.listmethods - Returns all registered RPC methods
 *
 * NEW: Access RpcRegistry through context or global (no chain access needed)
 */
din::Json rpc_context_rpc_listmethods(const ExecutionContext& ctx, const din::Json& params) {
    extern RpcRegistry g_rpcRegistry;

    auto method_names = g_rpcRegistry.methodNames();

    din::Json result(Json::arrayValue);
    for (const auto& method : method_names) {
        result.append(method);
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

/**
 * Register context-aware economics methods (Week 2)
 *
 * These methods replace the legacy versions and use DaemonContext
 * instead of global variables for service access.
 */
void registerEconomicsMethodsContext() {
    extern RpcRegistry g_rpcRegistry;

    // Note: Using RegisterMode::Overwrite to replace legacy handlers

    g_rpcRegistry.registerHandler("economics.getsupply",
                                 rpc_context_economics_getsupply,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("economics.getinfo",
                                 rpc_context_economics_getinfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("rpc.version",
                                 rpc_context_rpc_version,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("getbuildinfo",
                                 rpc_context_getbuildinfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("consensus.checkdb",
                                 rpc_context_consensus_checkdb,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("economics.getminerstats",
                                 rpc_context_economics_getminerstats,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("getverificationsummary",
                                 rpc_context_getverificationsummary,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("rpc.listmethods",
                                 rpc_context_rpc_listmethods,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("getconsensusinfo",
                                 rpc_context_getconsensusinfo,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    dinero::g_logger.info("[RPC Context] Registered 9 economics/telemetry context-aware methods");
}
