/**
 * Mining RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file demonstrates the migration from legacy globals to DaemonContext.
 * Compare with methods_mining.cpp to see the difference.
 *
 * OLD PATTERN (legacy):
 *   extern ChainDB* g_chain_db_direct;
 *   extern WalletManager* g_wallet_manager;
 *   uint32_t height = dinero::storage::GetChainHeight(dinero::legacy::g_chain_db_direct());
 *
 * NEW PATTERN (context-aware):
 *   auto mining = ctx.daemon->mining;
 *   auto chainstate = ctx.daemon->chainstate;
 *   uint32_t height = chainstate->getBlockHeight();
 *
 * Benefits:
 * - No dependency on global variables
 * - Testable with mock services
 * - Clear dependency tracking
 * - Type-safe service access
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "rpc/mining_policy.h"  // Phase E.1: Policy view abstraction
#include "daemon/config.h"      // GetConfig() for sync-profile mining gates
#include "daemon/daemon_context.h"
#include "daemon/services/mining_service.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/p2p_service.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/config_service.h"
#include "mining/mining_readiness.h"
#include "common/logger.h"
#include "storage/chain_db.h"
#include "storage/chain_direct.h"
// #include "mining/mining_manager.h"  // Phase C: Removed - use MiningManager v2 through MiningService
#include "mining/address_validator.h"  // Phase A: Bech32 validation
#include "consensus/chainparams.h"  // Phase A: Chain params detection
#include "wallet/wallet_worker.h"    // For WalletNotify::OnBlockConnected
#include "daemon/block_acceptor.h"   // For BlockAcceptor::AcceptBlockFromRPC
// NOTE: ExplorerSyncService removed (December 2025) - SQLite mirror (wrong architecture)
#include <memory>
#include <sstream>
#include <iomanip>

namespace {

// Hard gate: local mining RPCs are disabled when the active sync profile forbids them.
bool EnsureLocalMiningAllowed(din::Json& result, bool status_like_response = false) {
    if (GetConfig().allow_local_mining) {
        return true;
    }

    const std::string message = "Local mining disabled by sync profile: " + GetConfig().sync_profile;
    if (status_like_response) {
        result["supported"] = false;
        result["mining"] = false;
        result["error"] = message;
    } else {
        result["error"]["code"] = -32020;
        result["error"]["message"] = message;
    }
    return false;
}

void PopulateMiningReadinessJson(din::Json& out, const dinero::mining::MiningReadiness& readiness) {
    out["ready"] = readiness.ready;
    out["reason"] = readiness.reason_code;
    out["message"] = readiness.message;
    out["p2p_running"] = readiness.p2p_running;
    out["is_initial_block_download"] = readiness.is_ibd;
    out["pause_if_ahead_of_network_view"] = readiness.pause_if_ahead_of_network_view;
    out["peer_count"] = static_cast<int64_t>(readiness.peer_count);
    out["min_peers"] = static_cast<int64_t>(readiness.min_peers);
    out["local_height"] = static_cast<int64_t>(readiness.local_height);
    out["network_height_estimate"] = static_cast<int64_t>(readiness.network_height_estimate);
    out["peer_best_height"] = static_cast<int64_t>(readiness.peer_best_height);
    out["peer_median_height"] = static_cast<int64_t>(readiness.peer_median_height);
    out["max_tip_lag"] = static_cast<int64_t>(readiness.max_tip_lag);
    out["max_tip_ahead"] = static_cast<int64_t>(readiness.max_tip_ahead);
    if (readiness.peer_freshest_age_seconds >= 0) {
        out["peer_freshest_age_seconds"] = static_cast<int64_t>(readiness.peer_freshest_age_seconds);
    }
    out["max_peer_staleness_seconds"] = static_cast<int64_t>(readiness.max_peer_staleness_seconds);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════
// PHASE F.1: VIEW BUILDERS (RPC Layer Responsibility)
// ═══════════════════════════════════════════════════════════════
//
// These functions build policy views from real services.
// They are ONE-WAY: they read state, never mutate.
//
// Guardrail: Views may only read state, never write.
// ═══════════════════════════════════════════════════════════════

/**
 * Build WalletPolicyView from WalletService
 *
 * One-way: reads wallet state, does not mutate
 */
static dinero::rpc::WalletPolicyView BuildWalletView(
    dinero::WalletService* wallet,
    const std::string& mining_address
) {
    if (!wallet || !wallet->hasActiveWallet()) {
        return {
            .has_active_wallet = false,
            .wallet_encrypted = false,
            .wallet_unlocked = true,
            .address_owned = false,
            .wallet_name = ""
        };
    }

    try {
        bool encrypted = wallet->get().isWalletEncrypted();
        bool locked = wallet->get().isWalletLocked();
        bool owned = wallet->get().isAddressMine(mining_address);

        return {
            .has_active_wallet = true,
            .wallet_encrypted = encrypted,
            .wallet_unlocked = !locked,
            .address_owned = owned,
            .wallet_name = wallet->getCurrentWalletName()
        };
    } catch (const std::exception& e) {
        // View building failed - treat as unsafe state
        return {
            .has_active_wallet = false,
            .wallet_encrypted = false,
            .wallet_unlocked = true,
            .address_owned = false,
            .wallet_name = ""
        };
    }
}

/**
 * Build ChainPolicyView from ChainstateService
 *
 * One-way: reads chain state, does not mutate
 */
static dinero::rpc::ChainPolicyView BuildChainView(
    dinero::ChainstateService* chainstate
) {
    if (!chainstate) {
        return {
            .is_initial_block_download = false,
            .is_reindexing = false,
            .chainstate_ready = false,
            .current_height = 0,
            .total_blocks = 0
        };
    }

    const uint64_t current_height = chainstate->getBlockHeight();
    const auto ibd_progress = chainstate->GetIBDProgress();
    uint64_t total_blocks = ibd_progress.network_height > 0
        ? static_cast<uint64_t>(ibd_progress.network_height)
        : static_cast<uint64_t>(ibd_progress.local_height);
    if (total_blocks < current_height) {
        total_blocks = current_height;
    }

    return {
        .is_initial_block_download = chainstate->IsInIBD(),
        .is_reindexing = false,
        .chainstate_ready = true,
        .current_height = current_height,
        .total_blocks = total_blocks
    };
}

/**
 * Build RestartPolicyView from daemon state
 *
 * One-way: reads restart state, does not mutate
 */
static dinero::rpc::RestartPolicyView BuildRestartView(
    const ExecutionContext& ctx
) {
    // Phase F.2: Load restart state from DaemonContext
    // These values are set at daemon startup and persisted at shutdown
    if (!ctx.daemon) {
        // No daemon context - assume runtime state (not a fresh start)
        return {
            .is_fresh_start = false,
            .mining_was_active_before = false
        };
    }

    return {
        .is_fresh_start = ctx.daemon->is_fresh_start,
        .mining_was_active_before = ctx.daemon->mining_was_active_before
    };
}

/**
 * Build MiningStatePolicyView from MiningService
 *
 * One-way: reads mining state, does not mutate
 */
static dinero::rpc::MiningStatePolicyView BuildMiningStateView(
    dinero::MiningService* mining,
    dinero::WalletService* wallet
) {
    if (!mining) {
        return {
            .is_mining_active = false,
            .current_wallet_name = "",
            .mining_wallet_name = ""
        };
    }

    std::string current_wallet = "";
    if (wallet && wallet->hasActiveWallet()) {
        current_wallet = wallet->getCurrentWalletName();
    }

    return {
        .is_mining_active = mining->isMiningEnabled(),
        .current_wallet_name = current_wallet,
        .mining_wallet_name = current_wallet  // MiningManager does not persist starter wallet yet.
    };
}

/**
 * Build MiningPolicyConfig from daemon configuration
 *
 * Phase F.4: Escape Hatch Wiring
 * Reads config flags and populates MiningPolicyConfig
 *
 * One-way: reads config, does not mutate
 */
static dinero::rpc::MiningPolicyConfig BuildPolicyConfig(
    dinero::ConfigService* config
) {
    if (!config) {
        // No config service - use safe defaults (all escape hatches disabled)
        return {
            .allow_external_mining = false,
            .skip_ibd_check = false
        };
    }

    // Phase F.4: Read escape hatch flags from config
    // --allow-external-mining: Allow mining to external addresses (not owned by wallet)
    // --mine-during-ibd: Allow mining during initial block download (wastes electricity)
    return {
        .allow_external_mining = config->GetBool("allow-external-mining", false),
        .skip_ibd_check = config->GetBool("mine-during-ibd", false)
    };
}

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

/**
 * mining.info - Get mining information
 *
 * OLD: extern ChainDB* g_chain_db_direct;
 *      dinero::storage::GetChainHeight(dinero::legacy::g_chain_db_direct())
 *
 * NEW: ctx.daemon->chainstate->getBlockHeight()
 *      ctx.daemon->mining->getMiningState()
 */
din::Json rpc_context_mining_info(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!EnsureLocalMiningAllowed(result, true)) {
        return result;
    }

    // Week 2: Access services via DaemonContext
    if (!ctx.daemon) {
        result["error"] = "DaemonContext not available";
        return result;
    }

    // Access chainstate for blockchain info
    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    // Phase 39: Get chain database via ChainstateService (ChainManager deleted)
    auto* daemon_ctx = DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }
    auto chainstate_svc = std::dynamic_pointer_cast<dinero::ChainstateService>(daemon_ctx->chainstate);
    auto* chain_db = chainstate_svc ? chainstate_svc->GetChainDB() : nullptr;
    if (!chain_db) {
        result["error"] = "Chain database not available";
        return result;
    }

    // Access mining service for mining state
    auto mining = std::dynamic_pointer_cast<dinero::MiningService>(ctx.daemon->mining);
    if (!mining) {
        result["error"] = "Mining service not available";
        return result;
    }

    // Get mining stats from MiningManager v2
    const auto& stats = mining->getMiningManager().getStats();
    std::string address = mining->getMiningAddress();

    // Get blockchain info
    uint32_t height = chainstate->getBlockHeight();
    double difficulty = dinero::storage::GetDifficulty(chain_db, height + 1);

    // Build result with real data from MiningManager
    result["mining"] = stats.is_mining.load();
    result["threads"] = static_cast<int>(stats.active_threads.load());
    result["hashrate"] = stats.current_hashrate.load();
    result["difficulty"] = difficulty;
    result["blocks_found"] = static_cast<int>(stats.blocks_found.load());
    result["block_height"] = static_cast<int>(height);
    result["current_job_id"] = stats.current_job_id;
    result["current_job_height"] = static_cast<int>(stats.current_height);
    result["total_hashes"] = static_cast<int64_t>(stats.total_hashes.load());

    // Calculate uptime if mining
    if (stats.is_mining.load() && stats.mining_start_time.load() > 0) {
        uint64_t uptime_seconds = static_cast<uint64_t>(std::time(nullptr)) - stats.mining_start_time.load();
        result["uptime_seconds"] = static_cast<int64_t>(uptime_seconds);
    } else {
        result["uptime_seconds"] = 0;
    }

    // Mining address
    if (!address.empty()) {
        result["address"] = address;
    } else {
        result["address"] = din::null();
    }

    auto config_svc = std::dynamic_pointer_cast<dinero::ConfigService>(ctx.daemon->config);
    auto p2p_svc = std::dynamic_pointer_cast<dinero::P2PService>(ctx.daemon->p2p);
    const auto readiness = dinero::mining::EvaluateMiningReadiness(
        chainstate.get(),
        p2p_svc.get(),
        config_svc.get());
    din::Json readiness_json;
    PopulateMiningReadinessJson(readiness_json, readiness);
    result["mining_readiness"] = readiness_json;

    return result;
}

/**
 * mining.start - Start mining
 *
 * OLD: extern ChainDB* g_chain_db_direct;
 *      extern WalletManager* g_wallet_manager;
 *
 * NEW: ctx.daemon->mining->Start()
 *      ctx.daemon->wallet->GetWalletManager()
 */
din::Json rpc_context_mining_start(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!EnsureLocalMiningAllowed(result)) {
        return result;
    }

    if (!ctx.daemon) {
        result["error"] = "DaemonContext not available";
        return result;
    }

    auto mining = std::dynamic_pointer_cast<dinero::MiningService>(ctx.daemon->mining);
    if (!mining) {
        result["error"] = "Mining service not available";
        return result;
    }

    // Check if already mining
    if (mining->isMiningEnabled()) {
        result["error"]["code"] = -32000;
        result["error"]["message"] = "Mining is already active";
        return result;
    }

    // Parse parameters — support both array [threads, address] and object [{"threads":N, "address":"..."}]
    int threads = 0;  // 0 = use config value or auto-detect
    std::string address;

    if (params.isArray() && params.size() >= 1) {
        if (params[0].isObject()) {
            // Object form: [{"threads": 8, "address": "din1p..."}]
            if (params[0].isMember("threads") && (params[0]["threads"].isInt() || params[0]["threads"].isUInt())) {
                threads = params[0]["threads"].asInt();
            }
            if (params[0].isMember("address") && params[0]["address"].isString()) {
                address = params[0]["address"].asString();
            }
        } else {
            // Array form: [threads, address]
            if (params[0].isInt() || params[0].isUInt()) {
                threads = params[0].asInt();
            }
            if (params.size() >= 2 && params[1].isString()) {
                address = params[1].asString();
            }
        }
    }

    // If threads not specified via RPC, read from config (--miningthreads CLI flag)
    if (threads == 0) {
        auto config_svc = std::dynamic_pointer_cast<dinero::ConfigService>(ctx.daemon->config);
        if (config_svc) {
            threads = config_svc->GetInt("genproclimit", 0);
        }
    }
    // 0 means auto-detect in MiningManager::startMining()

    // Validate threads
    if (threads < 0 || threads > 256) {
        result["error"]["code"] = -32602;
        result["error"]["message"] = "Invalid thread count (0-256, 0=auto)";
        return result;
    }

    // Get address from wallet if not provided
    if (address.empty()) {
        address = mining->getMiningAddress();
    }

    // Try to get from WalletService if still empty
    if (address.empty() && ctx.daemon->wallet) {
        auto wallet = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        if (wallet && wallet->hasActiveWallet()) {
            try {
                address = wallet->get().getMiningAddress(
                    wallet->getCurrentWalletName(),
                    dinero::ChainToString(dinero::Chain::MAINNET)
                );
            } catch (const std::exception& e) {
                // Continue - will error below if still empty
            }
        }
    }

    // Enforce invariant: Mining requires valid address
    if (address.empty()) {
        result["error"]["code"] = -32602;
        result["error"]["message"] = "Mining cannot start without address. Use 'mining.setaddress <address>' or pass address as parameter.";
        return result;
    }

    // Validate address format
    if (!dinero::mining::IsValidDineroAddress(address)) {
        result["error"]["code"] = -5;
        result["error"]["message"] = "Invalid mining address format. Must be valid Bech32 (din1...) or Base58 (D...) address.";
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════
    // TAPROOT-ONLY MINING POLICY
    // ═══════════════════════════════════════════════════════════════════
    // Dinero mining uses Taproot-only coinbase outputs by policy.
    // Wallets remain fully backward compatible with all address types.
    // ═══════════════════════════════════════════════════════════════════
    if (!dinero::mining::IsCoinbaseEligibleAddress(address)) {
        result["error"]["code"] = -5;
        result["error"]["message"] = dinero::mining::GetTaprootRequiredMessage(address);
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase F.1: RPC Policy Gates (E.1 + E.2 + E.3 Enforcement)
    // ═══════════════════════════════════════════════════════════════════
    //
    // Architecture:
    // 1. Build policy views using view builder functions (F.1 helper functions)
    // 2. Call ALL policy checks (CheckMiningStartPolicy + CheckMiningResumePolicy)
    // 3. Only if ALL policies pass → execute MiningManager
    //
    // This ensures:
    // - Policy is testable without touching SQLite, RocksDB, or encryption
    // - All policy contracts (E.1, E.2, E.3) are enforced before execution
    // - No shortcuts, no early execution
    //
    // ═══════════════════════════════════════════════════════════════════

    // Step 1: Build WalletPolicyView from WalletService state
    auto wallet_svc = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    dinero::rpc::WalletPolicyView wallet_view = BuildWalletView(wallet_svc.get(), address);

    // Step 2: Build ChainPolicyView from ChainstateService state
    auto chainstate_svc = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    dinero::rpc::ChainPolicyView chain_view = BuildChainView(chainstate_svc.get());

    // Step 3: Build RestartPolicyView from daemon state
    dinero::rpc::RestartPolicyView restart_view = BuildRestartView(ctx);

    // Step 4: Build MiningPolicyConfig from daemon config (Phase F.4)
    auto config_svc = std::dynamic_pointer_cast<dinero::ConfigService>(ctx.daemon->config);
    dinero::rpc::MiningPolicyConfig policy_config = BuildPolicyConfig(config_svc.get());

    // Step 5: Check mining start policy (E.1 + E.2)
    auto start_policy = dinero::rpc::CheckMiningStartPolicy(wallet_view, chain_view, policy_config);

    if (!start_policy.allowed) {
        result["error"]["code"] = start_policy.error_code;
        result["error"]["message"] = start_policy.error_message;
        return result;
    }

    auto p2p_svc = std::dynamic_pointer_cast<dinero::P2PService>(ctx.daemon->p2p);
    const auto readiness = dinero::mining::EvaluateMiningReadiness(
        chainstate_svc.get(),
        p2p_svc.get(),
        config_svc.get());
    if (!readiness.ready) {
        result["error"]["code"] = -10;
        result["error"]["message"] = readiness.message;
        din::Json readiness_json;
        PopulateMiningReadinessJson(readiness_json, readiness);
        result["mining_readiness"] = readiness_json;
        return result;
    }

    // Step 6: E.3 restart policy check NOT needed here
    // Rationale: All calls to mining.start RPC are explicit user actions.
    // The E.3 contract ("Mining does not auto-resume after restart") prevents
    // AUTOMATIC resumption, which would happen in daemon startup code, not here.
    // Explicit mining.start() calls should always be allowed (subject to E.1/E.2 checks).

    // ═══════════════════════════════════════════════════════════════════
    // End Phase F.1 Enforcement - All policy checks passed
    // ONLY NOW execute MiningManager (no shortcuts, no early execution)
    // ═══════════════════════════════════════════════════════════════════

    // Set address before starting (in-memory)
    mining->setMiningAddress(address);

    // Phase D.3: Persist mining address to WalletManager so it survives daemon restarts
    if (ctx.daemon->wallet) {
        auto wallet = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        if (wallet && wallet->hasActiveWallet()) {
            try {
                wallet->get().setMiningAddress(
                    address,
                    wallet->getCurrentWalletName(),
                    dinero::ChainToString(dinero::Chain::MAINNET)
                );
            } catch (const std::exception& e) {
                // Non-fatal: Address set in MiningService, just not persisted
                dinero::g_logger.warn("Failed to persist mining address to wallet: " + std::string(e.what()));
            }
        }
    }

    // Start mining via MiningManager v2
    // Pass threads parameter (0 = auto-detect optimal thread count)
    bool started = mining->getMiningManager().startMining(threads);

    if (!started) {
        result["error"]["code"] = -32000;
        result["error"]["message"] = "Failed to start mining. Check logs for details.";
        return result;
    }

    if (p2p_svc) {
        p2p_svc->SetMiningRelayActive(true);
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase E.3 Fix: Clear is_fresh_start after explicit mining.start()
    // ═══════════════════════════════════════════════════════════════════
    // Once the user explicitly calls mining.start(), we're no longer in a
    // "fresh start" state. This allows subsequent mining.start() calls to
    // work without being blocked by the restart policy check.
    //
    // Rationale: E.3 contract prevents AUTO-resume, not EXPLICIT user calls.
    if (ctx.daemon && ctx.daemon->is_fresh_start) {
        ctx.daemon->is_fresh_start = false;
        dinero::g_logger.info("[mining.start] Cleared is_fresh_start flag after explicit user call");
    }

    // Return success with actual mining state
    const auto& stats = mining->getMiningManager().getStats();
    result["mining"] = true;
    result["threads"] = static_cast<int>(stats.active_threads.load());
    result["address"] = address;
    result["message"] = threads == 0 ? "Mining started with auto-detected thread count" : "Mining started successfully";

    return result;
}

/**
 * mining.stop - Stop mining
 *
 * NEW: ctx.daemon->mining->Stop()
 */
din::Json rpc_context_mining_stop(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!EnsureLocalMiningAllowed(result)) {
        return result;
    }

    if (!ctx.daemon) {
        result["error"] = "DaemonContext not available";
        return result;
    }

    auto mining = std::dynamic_pointer_cast<dinero::MiningService>(ctx.daemon->mining);
    if (!mining) {
        result["error"] = "Mining service not available";
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase F.1: RPC Policy Gates (E.4.2 Enforcement)
    // ═══════════════════════════════════════════════════════════════════
    //
    // Contract: mining.stop is ALWAYS allowed (idempotent)
    // - If mining is active → stop it
    // - If mining is NOT active → no-op (still success)
    // - No error conditions exist for stop
    //
    // ═══════════════════════════════════════════════════════════════════

    // Step 1: Check mining stop policy (E.4.2 - always succeeds)
    auto stop_policy = dinero::rpc::CheckMiningStopPolicy();
    if (!stop_policy.allowed) {
        // This should NEVER happen (CheckMiningStopPolicy always returns Success)
        // If it does, it means the policy contract was violated
        result["error"]["code"] = stop_policy.error_code;
        result["error"]["message"] = stop_policy.error_message;
        return result;
    }

    // Step 2: Get stats before stopping (for reporting, safe even if not mining)
    bool was_mining = mining->isMiningEnabled();
    const auto& stats = mining->getMiningManager().getStats();
    uint64_t total_hashes = stats.total_hashes.load();
    uint64_t blocks_found = stats.blocks_found.load();
    uint64_t uptime_seconds = 0;
    if (stats.mining_start_time.load() > 0) {
        uptime_seconds = static_cast<uint64_t>(std::time(nullptr)) - stats.mining_start_time.load();
    }

    // Step 3: Stop mining via MiningManager v2 (idempotent - safe to call if already stopped)
    mining->getMiningManager().stopMining();
    if (was_mining) {
        if (auto p2p_svc = std::dynamic_pointer_cast<dinero::P2PService>(ctx.daemon->p2p)) {
            p2p_svc->SetMiningRelayActive(false);
        }
    }

    // ═══════════════════════════════════════════════════════════════════
    // End Phase F.1 Enforcement - Policy check passed (always succeeds)
    // ═══════════════════════════════════════════════════════════════════

    // Return success with session summary
    result["mining"] = false;
    result["message"] = was_mining ? "Mining stopped successfully" : "Mining was not active (idempotent stop)";

    // Only include session summary if mining was actually active
    if (was_mining && (total_hashes > 0 || blocks_found > 0)) {
        result["session_summary"]["total_hashes"] = static_cast<int64_t>(total_hashes);
        result["session_summary"]["blocks_found"] = static_cast<int>(blocks_found);
        result["session_summary"]["uptime_seconds"] = static_cast<int64_t>(uptime_seconds);
        if (uptime_seconds > 0) {
            result["session_summary"]["average_hashrate"] = static_cast<double>(total_hashes) / static_cast<double>(uptime_seconds);
        }
    }

    return result;
}

/**
 * mining.setrelayactive - Signal external miner activity to the daemon's relay auto-mode.
 *
 * Why: mining.start/stop already flip P2PService::SetMiningRelayActive() so the daemon
 * advertises NODE_RELAY while its own mining engine runs. The Qt embedded miner and other
 * external clients drive mining purely through getblocktemplate/submitblock and never
 * call mining.start, so under p2p.relay=auto the relay role never activates. This RPC
 * lets such clients drive the same state directly.
 *
 * Semantics: identical to the SetMiningRelayActive() calls inside mining.start/stop.
 * p2p.relay=0 hard opt-out and p2p.relay=1 explicit opt-in continue to override.
 */
din::Json rpc_context_mining_setrelayactive(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!EnsureLocalMiningAllowed(result)) {
        return result;
    }

    if (!params.isArray() || params.size() < 1 || !params[0].isBool()) {
        result["error"]["code"] = -8;
        result["error"]["message"] = "active (boolean) required";
        return result;
    }

    bool active = params[0].asBool();

    if (!ctx.daemon) {
        result["error"]["code"] = -32000;
        result["error"]["message"] = "DaemonContext not available";
        return result;
    }

    auto p2p_svc = std::dynamic_pointer_cast<dinero::P2PService>(ctx.daemon->p2p);
    if (!p2p_svc) {
        result["error"]["code"] = -32000;
        result["error"]["message"] = "P2P service not available";
        return result;
    }

    p2p_svc->SetMiningRelayActive(active);

    const auto status = p2p_svc->GetNetworkStatus();
    result["mining_relay_active"] = status.mining_relay_active;
    result["relay_mode"] = status.relay_mode;
    result["local_relay"] = status.local_relay;
    return result;
}

/**
 * mining.setaddress - Set mining payout address
 *
 * NEW: ctx.daemon->mining->SetMiningAddress()
 *      ctx.daemon->wallet->SaveMiningAddress()
 */
din::Json rpc_context_mining_setaddress(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!EnsureLocalMiningAllowed(result)) {
        return result;
    }

    if (!ctx.daemon) {
        result["error"] = "DaemonContext not available";
        return result;
    }

    auto mining = std::dynamic_pointer_cast<dinero::MiningService>(ctx.daemon->mining);
    if (!mining) {
        result["error"] = "Mining service not available";
        return result;
    }

    if (params.empty() || !params[0].is<std::string>()) {
        result["error"]["code"] = -32602;
        result["error"]["message"] = "Missing address parameter";
        return result;
    }

    std::string address = params[0].as<std::string>();

    // Special case: if address is "wallet" or "derive", derive from wallet
    if (address == "wallet" || address == "derive") {
        if (!ctx.daemon->wallet) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "Wallet service not available";
            return result;
        }

        auto wallet = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        if (!wallet) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = "Failed to cast wallet service";
            return result;
        }

        try {
            if (!wallet->hasActiveWallet()) {
                result["error"]["code"] = -13;
                result["error"]["message"] = "No active wallet. Use wallet.load first.";
                return result;
            }
            address = wallet->get().getNewAddress("mining");
            result["derived_from_wallet"] = true;
        } catch (const std::exception& e) {
            result["error"]["code"] = -32602;
            result["error"]["message"] = std::string("Failed to derive address: ") + e.what();
            return result;
        }
    }

    // Validate address format with Bech32 checksum validation
    if (!dinero::mining::IsValidDineroAddress(address)) {
        result["error"]["code"] = -5;
        result["error"]["message"] = "Invalid address format. Must be valid Bech32 (din1...) or Base58 (D...) address.";
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════
    // TAPROOT-ONLY MINING POLICY
    // ═══════════════════════════════════════════════════════════════════
    // Dinero mining uses Taproot-only coinbase outputs by policy.
    // Wallets remain fully backward compatible with all address types.
    // ═══════════════════════════════════════════════════════════════════
    if (!dinero::mining::IsCoinbaseEligibleAddress(address)) {
        result["error"]["code"] = -5;
        result["error"]["message"] = dinero::mining::GetTaprootRequiredMessage(address);
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase F.1: RPC Policy Gates (E.4.1 Enforcement)
    // ═══════════════════════════════════════════════════════════════════
    //
    // Contract: Cannot switch wallet while mining is active
    // - Changing address mid-mining = silent wallet switch = reward loss
    // - User must explicitly stop mining before changing address
    //
    // ═══════════════════════════════════════════════════════════════════

    // Step 1: Build MiningStatePolicyView to check if mining is active
    auto wallet_svc = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
    dinero::rpc::MiningStatePolicyView mining_state = BuildMiningStateView(mining.get(), wallet_svc.get());

    // Step 2: Check wallet switch policy (E.4.1)
    auto switch_policy = dinero::rpc::CheckWalletSwitchPolicy(mining_state);
    if (!switch_policy.allowed) {
        result["error"]["code"] = switch_policy.error_code;
        result["error"]["message"] = switch_policy.error_message;
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════
    // End Phase F.1 Enforcement - Policy check passed
    // Safe to change mining address (mining is not active)
    // ═══════════════════════════════════════════════════════════════════

    // Set mining address via service
    mining->setMiningAddress(address);

    // Save to wallet if available
    if (ctx.daemon->wallet) {
        auto wallet = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        if (wallet && wallet->hasActiveWallet()) {
            try {
                wallet->get().setMiningAddress(
                    address,
                    wallet->getCurrentWalletName(),
                    dinero::ChainToString(dinero::Chain::MAINNET)
                );
                result["wallet_saved"] = true;
            } catch (const std::exception& e) {
                // Non-critical - mining address still set
                dinero::g_logger.warning("Failed to save mining address to wallet: " + std::string(e.what()));
            }
        }
    }

    result["address"] = address;
    result["message"] = "Mining address updated";

    return result;
}

/**
 * mining.getaddress - Get current mining payout address
 *
 * NEW: ctx.daemon->mining->GetMiningAddress()
 *      ctx.daemon->wallet->GetMiningAddress()
 */
din::Json rpc_context_mining_getaddress(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!EnsureLocalMiningAllowed(result, true)) {
        return result;
    }

    if (!ctx.daemon) {
        result["error"] = "DaemonContext not available";
        return result;
    }

    auto mining = std::dynamic_pointer_cast<dinero::MiningService>(ctx.daemon->mining);
    if (!mining) {
        result["error"] = "Mining service not available";
        return result;
    }

    // Try to get from wallet first (persistent storage)
    std::string mining_address;
    std::string source = "memory";

    if (ctx.daemon->wallet) {
        auto wallet = std::dynamic_pointer_cast<dinero::WalletService>(ctx.daemon->wallet);
        if (wallet && wallet->hasActiveWallet()) {
            try {
                mining_address = wallet->get().getMiningAddress(
                    wallet->getCurrentWalletName(),
                    dinero::ChainToString(dinero::Chain::MAINNET)
                );
                source = "wallet";
            } catch (const std::exception& e) {
                // Fall back to mining service
            }
        }
    }

    // Fall back to mining service if wallet doesn't have it
    if (mining_address.empty()) {
        mining_address = mining->getMiningAddress();
        source = "memory";
    }

    result["address"] = mining_address.empty() ? din::null() : din::Json(mining_address);
    result["mining"] = mining->isMiningEnabled();
    result["source"] = source;

    return result;
}

/**
 * mining.generatetoaddress - Generate blocks to a specified address (regtest/testnet)
 *
 * Mines a specified number of blocks immediately and returns the block hashes.
 * This is useful for regtest testing scenarios where you need to mine blocks on-demand.
 *
 * Parameters:
 *   nblocks  (int)    - Number of blocks to generate
 *   address  (string) - Address to receive block rewards
 *
 * Returns:
 *   Array of block hashes
 */
din::Json rpc_context_mining_generatetoaddress(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!EnsureLocalMiningAllowed(result)) {
        return result;
    }

    // This RPC requires a synchronous MiningManager::GenerateBlocks() path.
    // Until that execution path exists, fail closed.
    result["error"]["code"] = -32601;
    result["error"]["message"] = "Method unavailable";
    result["error"]["detail"] = "mining.generatetoaddress requires MiningManager::GenerateBlocks() - use mining.start for continuous mining";
    return result;
}

// Removed old implementation - will be reimplemented when MiningManager::GenerateBlocks() is added

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

/**
 * Register context-aware mining methods (Week 2)
 *
 * These methods replace the legacy versions and use DaemonContext
 * instead of global variables for service access.
 */
void registerMiningMethodsContext() {
    extern RpcRegistry g_rpcRegistry;

    // Note: Using RegisterMode::Overwrite to replace legacy handlers

    g_rpcRegistry.registerHandler("mining.info",
                                 rpc_context_mining_info,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("mining.start",
                                 rpc_context_mining_start,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("mining.stop",
                                 rpc_context_mining_stop,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("mining.setrelayactive",
                                 rpc_context_mining_setrelayactive,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("mining.setaddress",
                                 rpc_context_mining_setaddress,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("mining.getaddress",
                                 rpc_context_mining_getaddress,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("mining.generatetoaddress",
                                 rpc_context_mining_generatetoaddress,
                                 RegisterMode::IfAbsent,  // Don't overwrite real implementation from methods_mining_extras_vnext.cpp
                                 "context-aware");

    // Register aliases for convenience and Bitcoin Core compatibility
    g_rpcRegistry.registerHandler("mining.status",
                                 rpc_context_mining_info,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("mining.getinfo",
                                 rpc_context_mining_info,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    dinero::g_logger.info("[RPC Context] Registered 8 mining context-aware methods (6 core + 2 aliases)");
}
