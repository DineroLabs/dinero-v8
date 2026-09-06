/**
 * Daemon status RPC methods
 *
 * Provides daemon readiness and health checking
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "daemon/health.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/config_service.h"
#include "daemon/services/logger_service.h"
#include "daemon/services/p2p_service.h"
#include "consensus/validation_queue.h"
#include "consensus/block_index.h"
#include "common/logger.h"
#include <ctime>
#include <memory>

/**
 * getdaemonstatus - Get daemon readiness status
 *
 * Returns the readiness state of all daemon services.
 * Use this to determine when the daemon is fully operational.
 *
 * Returns:
 * {
 *   "ready": bool,              // Overall readiness
 *   "services": {
 *     "rpc": "ready"|"starting"|"error",
 *     "chainstate": "ready"|"starting"|"error",
 *     "wallet": "ready"|"starting"|"error",
 *     "mempool": "ready"|"starting"|"error",
 *     "p2p": "ready"|"starting"|"error"
 *   },
 *   "chain": "mainnet"|"testnet"|"regtest",
 *   "height": number
 * }
 */
static din::Json rpc_getdaemonstatus(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Prefer explicit execution context; fall back to singleton for legacy callers.
    auto* daemon_ctx = ctx.daemon ? ctx.daemon : DaemonContext::instance();
    if (!daemon_ctx) {
        result["ready"] = false;
        result["error"] = "DaemonContext not initialized";
        return result;
    }

    din::Json services;
    bool all_ready = true;

    // Check RPC service
    if (daemon_ctx->rpc) {
        services["rpc"] = "ready";
    } else {
        services["rpc"] = "not_initialized";
        all_ready = false;
    }

    // Check Chainstate service
    if (daemon_ctx->chainstate) {
        try {
            // Try to get height - if this works, chainstate is ready
            auto chaindb = daemon_ctx->chainstate->GetChainDB();
            if (chaindb) {
                auto tip_result = chaindb->getTip();
                if (tip_result.ok()) {
                    result["height"] = tip_result.value().height;
                    services["chainstate"] = "ready";
                } else {
                    services["chainstate"] = "error";
                    all_ready = false;
                }
            } else {
                services["chainstate"] = "error";
                all_ready = false;
            }

            // One authoritative, lock-safe view of header/active-chain
            // convergence. Height alone is insufficient: equal-height forks
            // must remain visibly divergent until their hashes agree.
            const auto sync = daemon_ctx->chainstate->GetSyncSnapshot();
            din::Json sync_json(Json::objectValue);
            din::Json active(Json::objectValue);
            active["available"] = sync.has_active_tip;
            active["height"] = static_cast<Json::UInt>(sync.active_tip_height);
            active["hash"] = sync.has_active_tip
                ? sync.active_tip_hash.GetHex() : "";
            din::Json header(Json::objectValue);
            header["available"] = sync.has_best_header;
            header["height"] = static_cast<Json::UInt>(sync.best_header_height);
            header["hash"] = sync.has_best_header
                ? sync.best_header_hash.GetHex() : "";

            const char* convergence = "unknown";
            switch (sync.convergence) {
            case dinero::consensus::HeaderConvergence::Converged:
                convergence = "converged";
                break;
            case dinero::consensus::HeaderConvergence::Mismatch:
                convergence = "mismatch";
                break;
            case dinero::consensus::HeaderConvergence::Unknown:
                break;
            }
            sync_json["active_tip"] = active;
            sync_json["best_header"] = header;
            sync_json["convergence"] = convergence;
            sync_json["converged"] = sync.IsConverged();

            if (sync.has_active_tip) {
                result["height"] = static_cast<Json::UInt>(sync.active_tip_height);
                if (const auto* index = dinero::FindBlockIndex(sync.active_tip_hash)) {
                    sync_json["chainwork"] = index->chainwork;
                    result["chainwork"] = index->chainwork;
                }
            }
            result["sync"] = sync_json;
        } catch (...) {
            services["chainstate"] = "error";
            all_ready = false;
        }
    } else {
        services["chainstate"] = "not_initialized";
        all_ready = false;
    }

    // Check Wallet service
    if (daemon_ctx->wallet) {
        services["wallet"] = "ready";
    } else {
        services["wallet"] = "not_initialized";
        all_ready = false;
    }

    // Check Mempool service
    if (daemon_ctx->mempool) {
        services["mempool"] = "ready";
    } else {
        services["mempool"] = "not_initialized";
        all_ready = false;
    }

    // Check P2P service (optional - not required for RPC/mining)
    if (daemon_ctx->p2p) {
        services["p2p"] = "ready";
    } else {
        services["p2p"] = "not_initialized";
        // P2P not required for readiness
    }

    // Check Mining service
    if (daemon_ctx->mining) {
        services["mining"] = "ready";
    } else {
        services["mining"] = "not_initialized";
        // Mining not required for readiness
    }

    if (daemon_ctx->validation_queue) {
        services["validation_queue"] = daemon_ctx->validation_queue->isRunning() ? "ready" : "stopped";
    } else {
        services["validation_queue"] = "not_initialized";
    }

    if (daemon_ctx->parallel_validator) {
        services["parallel_validator"] = "ready";
    } else {
        services["parallel_validator"] = "not_initialized";
    }

    result["ready"] = all_ready;
    result["services"] = services;

    if (daemon_ctx->validation_queue) {
        din::Json queue;
        queue["running"] = daemon_ctx->validation_queue->isRunning();
        queue["queued_blocks"] = static_cast<Json::UInt64>(daemon_ctx->validation_queue->getQueuedCount());
        queue["in_flight_blocks"] = static_cast<Json::UInt64>(daemon_ctx->validation_queue->getInFlightCount());
        queue["processed_blocks"] = static_cast<Json::UInt64>(daemon_ctx->validation_queue->getTotalProcessed());
        result["validation_queue"] = queue;
    }

    // Get chain type
    if (daemon_ctx->config) {
        auto config = std::dynamic_pointer_cast<dinero::ConfigService>(daemon_ctx->config);
        if (config) {
            if (config->IsRegtest()) {
                result["chain"] = "regtest";
            } else if (config->IsTestnet()) {
                result["chain"] = "testnet";
            } else {
                result["chain"] = "mainnet";
            }
        } else {
            result["chain"] = "mainnet";
        }
    } else {
        result["chain"] = "mainnet";
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// safemode.status — operator can read current safe-mode state
// ═══════════════════════════════════════════════════════════════
static din::Json rpc_safemode_status(const ExecutionContext& ctx, const din::Json& params) {
    (void)params;
    din::Json result;
    auto* daemon_ctx = ctx.daemon ? ctx.daemon : DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->chainstate) {
        result["error"] = "chainstate_not_initialized";
        return result;
    }
    result["active"] = daemon_ctx->chainstate->IsInSafeMode();
    result["reason"] = daemon_ctx->chainstate->GetSafeModeReason();
    return result;
}

// ═══════════════════════════════════════════════════════════════
// reorg.status — read-only record of reorganisations this process lifetime.
//
// `total` is the process-lifetime count, NOT the ring length. A consumer that
// sees total outrun the events it can account for knows the ring overflowed and
// can report a gap instead of silently under-reporting. `boot_id` changes on
// restart, so a consumer can tell a reset apart from data loss.
// ═══════════════════════════════════════════════════════════════
static din::Json rpc_reorg_status(const ExecutionContext& ctx, const din::Json& params) {
    (void)params;
    din::Json result;
    auto* daemon_ctx = ctx.daemon ? ctx.daemon : DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->chainstate) {
        result["error"] = "chainstate_not_initialized";
        return result;
    }
    // ONE snapshot, taken under a single lock. Calling BootId()/Total()/Events()
    // separately would let a Record() land between them, so total could outrun
    // the events in the same reply — a fabricated overflow on the one signal a
    // consumer uses to detect real ones.
    const auto snapshot = daemon_ctx->chainstate->GetReorgLog().Take();
    result["boot_id"] = snapshot.boot_id;
    result["total"] = static_cast<Json::UInt64>(snapshot.total);

    din::Json events(Json::arrayValue);
    for (const auto& event : snapshot.events) {
        din::Json item(Json::objectValue);
        item["seq"] = static_cast<Json::UInt64>(event.seq);
        item["timestamp"] = event.timestamp;
        item["disconnected"] = static_cast<Json::UInt>(event.disconnected);
        item["connected"] = static_cast<Json::UInt>(event.connected);
        events.append(item);
    }
    result["events"] = events;
    return result;
}

// ═══════════════════════════════════════════════════════════════
// daemon.shieldedstatehash — composite hash of every reorg-bound
// state container (utreexo forest + shielded tree + nullifier set
// + anchor history). Drives the ShieldedReorgInvertibility property
// test and is generally useful for any caller that needs a single
// 32-byte fingerprint that drifts iff any tracked container drifts.
// ═══════════════════════════════════════════════════════════════
static din::Json rpc_shielded_state_hash(const ExecutionContext& ctx, const din::Json& params) {
    (void)params;
    din::Json result;
    auto* daemon_ctx = ctx.daemon ? ctx.daemon : DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->chainstate) {
        result["error"] = "chainstate_not_initialized";
        return result;
    }
    const auto state_hash = daemon_ctx->chainstate->ComputeShieldedReorgStateHash();
    result["state_hash"] = state_hash.GetHex();
    return result;
}

// ═══════════════════════════════════════════════════════════════
// daemon.shieldedroot — the shielded half of a snapshot's chain
// binding. Read-only, committed nowhere yet: this is the value a
// future block-header commitment would carry, exposed now so the
// fleet can be cross-checked for determinism BEFORE anything about
// it becomes consensus.
// ═══════════════════════════════════════════════════════════════
static din::Json rpc_shielded_root(const ExecutionContext& ctx, const din::Json& params) {
    (void)params;
    din::Json result;
    auto* daemon_ctx = ctx.daemon ? ctx.daemon : DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->chainstate) {
        result["error"] = "chainstate_not_initialized";
        return result;
    }
    using SRS = dinero::ChainstateService::ShieldedRootStatus;
    SRS status = SRS::Ok;
    const auto root = daemon_ctx->chainstate->ComputeShieldedRoot(&status);
    if (!root.has_value()) {
        // Never a digest here — an unreadable set, a set being mutated, and an
        // empty set are three different facts, and collapsing them would let a
        // caller read a transient condition as a state commitment.
        result["error"] = (status == SRS::StateBusy)
            ? "shielded_state_busy"   // retry: a block is connecting
            : "nullifier_set_unreadable";
        return result;
    }
    result["shielded_root"] = root->GetHex();
    return result;
}

// ═══════════════════════════════════════════════════════════════
// safemode.exit — operator manually clears safe mode
//
// Required to recover after a deep-reorg-triggered safe mode without
// restarting the daemon. Requires `confirm: true` to prevent
// accidental clearing — safe mode is a real safety gate and the
// operator should explicitly acknowledge they've inspected the chain
// state before resuming mining.
// ═══════════════════════════════════════════════════════════════
static din::Json rpc_safemode_exit(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    auto* daemon_ctx = ctx.daemon ? ctx.daemon : DaemonContext::instance();
    if (!daemon_ctx || !daemon_ctx->chainstate) {
        result["error"] = "chainstate_not_initialized";
        return result;
    }

    bool confirm = false;
    if (params.isObject() && params.isMember("confirm")) {
        confirm = params["confirm"].asBool();
    } else if (params.isArray() && params.size() >= 1) {
        confirm = params[0].asBool();
    }
    if (!confirm) {
        result["error"] = "confirmation_required";
        result["error_message"] =
            "safemode.exit must be called with {\"confirm\": true} after the "
            "operator has inspected the chain state. Refusing to clear safe "
            "mode silently.";
        result["current_active"] = daemon_ctx->chainstate->IsInSafeMode();
        result["current_reason"] = daemon_ctx->chainstate->GetSafeModeReason();
        return result;
    }

    if (!daemon_ctx->chainstate->IsInSafeMode()) {
        result["status"] = "not_active";
        result["message"] = "safe mode was not active; nothing to exit";
        return result;
    }

    const std::string previous_reason = daemon_ctx->chainstate->GetSafeModeReason();
    daemon_ctx->chainstate->ExitSafeMode();
    result["status"] = "exited";
    result["previous_reason"] = previous_reason;
    return result;
}

// ═══════════════════════════════════════════════════════════════
// health — Phase D.4 of Dinero Core 1.0 contract.
//
// Returns OK / DEGRADED / FAILING with stable JSON schema:
//   { status, exit_code, checks: { tip_height, tip_age_seconds,
//     tip_age_threshold_seconds, safemode_active, peer_count,
//     peer_threshold, tip_undo_present, fatal_in_last_5min },
//     reason? }
//
// Decision logic lives in dinero::health::ComputeHealthStatus()
// (pure function, unit-tested separately). This handler is the
// thin "gather + transform" wrapper.
//
// Stable schema. Adding a check is a contract change. Removing a
// check is a contract change. Reordering or renaming JSON keys is
// a contract change.
// ═══════════════════════════════════════════════════════════════
static din::Json rpc_health(const ExecutionContext& ctx, const din::Json& params) {
    (void)params;
    din::Json result;
    auto* daemon_ctx = ctx.daemon ? ctx.daemon : DaemonContext::instance();

    if (!daemon_ctx || !daemon_ctx->chainstate) {
        // Daemon not fully initialized → FAILING by definition.
        result["status"]    = "FAILING";
        result["exit_code"] = 2;
        result["reason"]    = "daemon not initialized";
        return result;
    }

    dinero::health::ChecksData d;

    // ── tip height + age ────────────────────────────────────────
    auto chaindb = daemon_ctx->chainstate->GetChainDB();
    bool tip_undo_present = false;
    if (chaindb) {
        auto tip_result = chaindb->getTip();
        if (tip_result.ok()) {
            const auto& tip = tip_result.value();
            d.tip_height = static_cast<uint32_t>(tip.height);
            const time_t now = std::time(nullptr);
            d.tip_age_seconds = static_cast<int64_t>(now) -
                                static_cast<int64_t>(tip.timestamp);

            // Undo-data presence at the tip: read header metadata, check
            // undo_size > 0. Catches the Apr 30 phantom-undo regression
            // class at the tip itself. Historical holes (older heights)
            // are NOT detected here — that needs the future
            // `undo_audit_holes_remaining` field per spec §10.
            auto meta = chaindb->getHeaderMetadata(tip.hash);
            if (meta.ok() && meta.value().undo_size > 0) {
                tip_undo_present = true;
            }
        }
    }
    d.tip_undo_present = tip_undo_present;

    // ── safemode ────────────────────────────────────────────────
    d.safemode_active = daemon_ctx->chainstate->IsInSafeMode();

    // ── peer count ──────────────────────────────────────────────
    d.peer_count = 0;
    if (daemon_ctx->p2p) {
        auto p2p_svc = std::dynamic_pointer_cast<dinero::P2PService>(daemon_ctx->p2p);
        if (p2p_svc) {
            d.peer_count = static_cast<int>(p2p_svc->GetPeerCount());
        }
    }

    // ── fatal log entries in last 5 min ─────────────────────────
    d.fatal_in_last_5min = 0;
    if (daemon_ctx->logger) {
        auto logger_svc = std::dynamic_pointer_cast<dinero::LoggerService>(daemon_ctx->logger);
        if (logger_svc) {
            d.fatal_in_last_5min = logger_svc->FatalCountLast5Min();
        }
    }

    // ── compute status ──────────────────────────────────────────
    const auto verdict = dinero::health::ComputeHealthStatus(d);

    result["status"]    = dinero::health::StatusToString(verdict.status);
    result["exit_code"] = verdict.exit_code;
    if (!verdict.reason.empty()) {
        result["reason"] = verdict.reason;
    }

    din::Json checks;
    checks["tip_height"]                 = static_cast<Json::UInt64>(d.tip_height);
    checks["tip_age_seconds"]            = static_cast<Json::Int64>(d.tip_age_seconds);
    checks["tip_age_threshold_seconds"]  = static_cast<Json::Int64>(
        dinero::health::kTipAgeThresholdDegradedSec);
    checks["safemode_active"]            = d.safemode_active;
    checks["peer_count"]                 = d.peer_count;
    checks["peer_threshold"]             = dinero::health::kPeerCountThreshold;
    checks["tip_undo_present"]           = d.tip_undo_present;
    checks["fatal_in_last_5min"]         = d.fatal_in_last_5min;
    result["checks"] = checks;

    return result;
}

// ═══════════════════════════════════════════════════════════════
// RPC REGISTRATION
// ═══════════════════════════════════════════════════════════════

/**
 * Register daemon status RPC methods
 *
 * Called during daemon initialization to register handlers.
 */
void register_daemon_status_rpc_methods(RpcRegistry& registry) {
    // daemon.getstatus - Daemon readiness status
    RpcMethodMeta getstatus_meta;
    getstatus_meta.name = "getstatus";
    getstatus_meta.ns = "daemon";
    getstatus_meta.description = "Get daemon readiness status (all services operational)";
    getstatus_meta.result.type = "object";
    getstatus_meta.result.desc = "Daemon readiness status";

    registry.registerHandler("daemon.getstatus", rpc_getdaemonstatus, getstatus_meta, "Readiness Contract");

    // Also register as getdaemonstatus for convenience
    RpcMethodMeta getdaemonstatus_meta;
    getdaemonstatus_meta.name = "getdaemonstatus";
    getdaemonstatus_meta.ns = "";
    getdaemonstatus_meta.description = "Get daemon readiness status (alias for daemon.getstatus)";
    getdaemonstatus_meta.result.type = "object";
    getdaemonstatus_meta.result.desc = "Daemon readiness status";

    registry.registerHandler("getdaemonstatus", rpc_getdaemonstatus, getdaemonstatus_meta, "Readiness Contract");

    // daemon.shieldedstatehash — composite reorg-state fingerprint
    RpcMethodMeta state_hash_meta;
    state_hash_meta.name = "shieldedstatehash";
    state_hash_meta.ns = "daemon";
    state_hash_meta.description =
        "32-byte SHA256 v2 fingerprint of the reorg-bound state. Includes: "
        "utreexo forest commitment + numLeaves + canonical-empty-roots flag, "
        "shielded tree root + size, nullifier set CONTENT (every entry, "
        "sorted by (block_height, nullifier) — v2 promotion from v1 closed "
        "audit gap #9), and the full anchor history (every (height, root) "
        "pair). Drives the ShieldedReorgInvertibility property test and is "
        "useful for asserting state byte-equality across "
        "Connect/Disconnect/Connect cycles or across restarts at the same "
        "tip. Tag bumped from 'DSRH' (v1) to 'DSR2' (v2); a digest from a "
        "v2 binary is intentionally not comparable to a v1 digest of the "
        "same state. v2: phase 3b atomic persistence is still open; this "
        "hash does not certify the working-copy + journal row contract.";
    state_hash_meta.result.type = "object";
    state_hash_meta.result.desc = "{ state_hash: string (hex) }";
    registry.registerHandler("daemon.shieldedstatehash", rpc_shielded_state_hash,
                             state_hash_meta, "Reorg Invertibility");

    // daemon.shieldedroot — shielded-only fingerprint (header-commitment candidate)
    RpcMethodMeta shielded_root_meta;
    shielded_root_meta.name = "shieldedroot";
    shielded_root_meta.ns = "daemon";
    shielded_root_meta.description =
        "32-byte SHA256 fingerprint (tag 'SHR1' v1) of the SHIELDED state only: "
        "commitment tree root + size, nullifier set content, and anchor history. "
        "Deliberately EXCLUDES the utreexo forest, which header.utreexo_root "
        "already commits — including it would tie this value to forest "
        "serialization. Every variable-length section is length-prefixed, so the "
        "boundary between nullifier content and anchor bytes is unambiguous "
        "(DSR2 concatenates them without lengths, which is fine for a reorg "
        "oracle but not for a value a header would commit). This is the "
        "candidate for state_commitment_v1. It is committed NOWHERE today and "
        "enforces nothing; it is exposed so independently-synced nodes can be "
        "compared at equal heights to prove determinism before activation.";
    shielded_root_meta.result.type = "object";
    shielded_root_meta.result.desc = "{ shielded_root: string (hex) }";
    registry.registerHandler("daemon.shieldedroot", rpc_shielded_root,
                             shielded_root_meta, "Reorg Invertibility");

    // safemode.status — read current safe-mode state
    RpcMethodMeta sm_status_meta;
    sm_status_meta.name = "status";
    sm_status_meta.ns = "safemode";
    sm_status_meta.description =
        "Read whether the chainstate is in safe mode and why. Safe mode "
        "pauses block-template generation when a deep reorg is detected.";
    sm_status_meta.result.type = "object";
    sm_status_meta.result.desc = "{ active: bool, reason: string }";
    registry.registerHandler("safemode.status", rpc_safemode_status, sm_status_meta, "Safe Mode");

    // reorg.status — read-only reorganisation record.
    //
    // Registered HERE, alongside safemode.status, deliberately. This function is
    // reached from RegisterDiagnosticsRPC(ctx) at src/rpc/rpc_init.cpp:32 and is
    // empirically live — safemode.status answers on production nodes. Creating a
    // new registration function is exactly how getchaintips, getchainwork and
    // getreorgstatus became unreachable: they are defined, compiled, and called
    // from nowhere.
    RpcMethodMeta reorg_status_meta;
    reorg_status_meta.name = "status";
    reorg_status_meta.ns = "reorg";
    reorg_status_meta.description =
        "Read the in-memory record of chain reorganisations for this process: "
        "a bounded ring of recent events plus a process-lifetime total.";
    reorg_status_meta.result.type = "object";
    reorg_status_meta.result.desc =
        "{ boot_id: string, total: uint, events: [{seq, timestamp, disconnected, connected}] }";
    registry.registerHandler("reorg.status", rpc_reorg_status, reorg_status_meta, "Reorg");

    // safemode.exit — operator manually clears safe mode
    RpcMethodMeta sm_exit_meta;
    sm_exit_meta.name = "exit";
    sm_exit_meta.ns = "safemode";
    sm_exit_meta.description =
        "Clear safe-mode lockout. Requires {\"confirm\": true}. After a "
        "deep-reorg-triggered safe mode the operator should inspect the "
        "chain state, confirm the canonical fork is what they expect, "
        "then call this RPC to resume mining/template generation. "
        "Without this RPC, the only recovery path is a daemon restart.";
    sm_exit_meta.result.type = "object";
    sm_exit_meta.result.desc =
        "{ status: 'exited'|'not_active'|error, previous_reason?: string }";
    registry.registerHandler("safemode.exit", rpc_safemode_exit, sm_exit_meta, "Safe Mode");

    // health — Phase D.4 of Dinero Core 1.0. Stable OK/DEGRADED/FAILING
    // schema with exit_code 0/1/2 and checks JSON. Locked in
    // docs/specs/dinero-core-1.0.md §10.
    RpcMethodMeta health_meta;
    health_meta.name = "health";
    health_meta.ns = "";
    health_meta.description =
        "Health check. Returns OK (exit 0), DEGRADED (exit 1), or "
        "FAILING (exit 2) based on tip-age, safemode, peer count, "
        "tip-undo presence, and recent FATAL log entries. Schema is "
        "stable across releases per Dinero Core 1.0 §10.";
    health_meta.result.type = "object";
    health_meta.result.desc =
        "{ status: 'OK'|'DEGRADED'|'FAILING', exit_code: 0|1|2, "
        "checks: { tip_height, tip_age_seconds, tip_age_threshold_seconds, "
        "safemode_active, peer_count, peer_threshold, tip_undo_present, "
        "fatal_in_last_5min }, reason?: string }";
    registry.registerHandler("health", rpc_health, health_meta, "Core 1.0 Health");
}
