/**
 * @file mining_policy.h
 * @brief Mining Policy Enforcement (Phase E.1 + E.2)
 *
 * Purpose: Pure policy functions that operate on state views, not implementations.
 *
 * Architecture:
 * - Policy layer operates on VIEWS of state (no IO, no disk, no network)
 * - RPC layer builds views from real services (WalletManager, ChainStateService)
 * - Tests inject views directly (no wallet initialization, no database)
 *
 * This ensures:
 * - Policy is testable without touching SQLite, RocksDB, or encryption
 * - Tests are fast, deterministic, and impossible to weaken
 * - Policy enforcement is auditable and explicit
 *
 * Contract: E.7 — Layer Boundaries
 * - RPC = policy enforcement (this file)
 * - Service = execution (WalletManager, MiningService)
 * - Storage = persistence (SQLite, RocksDB)
 */

#pragma once

#include <string>
#include <cstdio>  // for snprintf (progress formatting)

namespace dinero {
namespace rpc {

/**
 * @brief View of wallet state for policy enforcement
 *
 * This is NOT WalletManager. This is a pure state snapshot.
 * No methods, no IO, just data.
 */
struct WalletPolicyView {
    bool has_active_wallet;      // Is any wallet loaded?
    bool wallet_encrypted;       // Is wallet encrypted?
    bool wallet_unlocked;        // Is wallet unlocked (if encrypted)?
    bool address_owned;          // Does wallet own the mining address?

    std::string wallet_name;     // For error messages only
};

/**
 * @brief View of chain state for policy enforcement
 *
 * Used for Phase E.2 (IBD safety), defined here for consistency
 */
struct ChainPolicyView {
    bool is_initial_block_download;  // Is node syncing?
    bool is_reindexing;              // Is chainstate rebuilding?
    bool chainstate_ready;           // Is chainstate accessible?

    uint64_t current_height;         // For error messages
    uint64_t total_blocks;           // For progress reporting
};

/**
 * @brief View of restart state for policy enforcement
 *
 * Used for Phase E.3 (Restart Semantics)
 */
struct RestartPolicyView {
    bool is_fresh_start;             // true immediately after daemon restart
    bool mining_was_active_before;   // persisted flag (read-only)
};

/**
 * @brief View of mining operational state for policy enforcement
 *
 * Used for Phase E.4 (Policy Completeness)
 */
struct MiningStatePolicyView {
    bool is_mining_active;           // Is mining currently running?
    std::string current_wallet_name; // Currently active wallet
    std::string mining_wallet_name;  // Wallet that started mining
};

/**
 * @brief Mining policy configuration
 *
 * Escape hatches and policy overrides
 */
struct MiningPolicyConfig {
    bool allow_external_mining;      // --allow-external-mining flag
    bool skip_ibd_check;             // --mine-during-ibd (dangerous)
};

/**
 * @brief Result of policy check
 *
 * If allowed=false, error_code and error_message explain why
 */
struct MiningPolicyResult {
    bool allowed;
    int error_code;           // JSON-RPC error code (-1 to -999)
    std::string error_message;

    // Optional: E.3 restart semantics
    bool must_explicitly_restart = false;  // true if mining requires explicit restart

    // Success constructor
    static MiningPolicyResult Success() {
        return {true, 0, "", false};
    }

    // Error constructor
    static MiningPolicyResult Error(int code, const std::string& message) {
        return {false, code, message, false};
    }

    // Restart required constructor
    static MiningPolicyResult RestartRequired(const std::string& message) {
        return {false, -10, message, true};
    }
};

/**
 * @brief Check if mining.start should be allowed (Phase E.1 + E.2)
 *
 * Contract: E.1 — Wallet Ownership Enforcement + E.2 — IBD Safety
 *
 * This function is PURE:
 * - No WalletManager calls
 * - No disk IO
 * - No network access
 * - Deterministic (same inputs → same output)
 *
 * Policy Enforcement:
 * - E.1.1: Mining address must be owned by active wallet
 * - E.1.2: Wallet must be unlocked (if encrypted)
 * - E.1.3: Wallet must be loaded
 * - E.2.1: Mining blocked during IBD (Initial Block Download)
 * - E.2.1: Mining blocked during reindex
 * - E.2.2: Mining requires chainstate ready
 *
 * @param wallet View of wallet state (injected by caller)
 * @param chain View of chain state (injected by caller)
 * @param config Policy configuration (escape hatches)
 * @return MiningPolicyResult indicating allowed/denied with error details
 */
inline MiningPolicyResult CheckMiningStartPolicy(
    const WalletPolicyView& wallet,
    const ChainPolicyView& chain,
    const MiningPolicyConfig& config
) {
    // ═══════════════════════════════════════════════════════════════════
    // Phase E.1.3: Mining requires active wallet
    // ═══════════════════════════════════════════════════════════════════

    if (!wallet.has_active_wallet) {
        // Exception: --allow-external-mining flag
        if (config.allow_external_mining) {
            // Allowed, but this is dangerous (user explicitly opted in)
            return MiningPolicyResult::Success();
        }

        return MiningPolicyResult::Error(
            -13,
            "No active wallet. Load wallet first: wallet.load <name>"
        );
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase E.1.1: Mining address must be owned by wallet
    // ═══════════════════════════════════════════════════════════════════

    if (!wallet.address_owned) {
        // Exception: --allow-external-mining flag
        if (config.allow_external_mining) {
            // Allowed, but dangerous (user accepts risk of losing rewards)
            return MiningPolicyResult::Success();
        }

        return MiningPolicyResult::Error(
            -13,
            "Mining address not owned by active wallet. "
            "Verify address with wallet.getaddressinfo or use wallet-derived address."
        );
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase E.1.2: Wallet must be unlocked (if encrypted)
    // ═══════════════════════════════════════════════════════════════════

    if (wallet.wallet_encrypted && !wallet.wallet_unlocked) {
        return MiningPolicyResult::Error(
            -13,
            "Wallet is locked. Unlock wallet to verify you can spend mining rewards: "
            "wallet.unlock <passphrase> <timeout>"
        );
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase E.2.2: Mining requires chainstate ready
    // ═══════════════════════════════════════════════════════════════════

    if (!chain.chainstate_ready) {
        return MiningPolicyResult::Error(
            -10,
            "Chainstate service not available. Verify daemon initialization completed."
        );
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase E.2.1: Mining blocked during reindex
    // ═══════════════════════════════════════════════════════════════════

    if (chain.is_reindexing) {
        // No escape hatch for reindex (chainstate is inconsistent)
        return MiningPolicyResult::Error(
            -10,
            "Mining disabled during blockchain reindex. Wait for reindex to complete."
        );
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase E.2.1: Mining blocked during IBD (Initial Block Download)
    // ═══════════════════════════════════════════════════════════════════

    if (chain.is_initial_block_download && !config.skip_ibd_check) {
        // Exception: --mine-during-ibd flag (dangerous, wastes electricity)
        // Format sync progress message
        std::string progress_msg = "Mining disabled during initial block download.";

        if (chain.total_blocks > 0) {
            double progress = (static_cast<double>(chain.current_height) /
                             static_cast<double>(chain.total_blocks)) * 100.0;

            char progress_buf[256];
            snprintf(progress_buf, sizeof(progress_buf),
                    "Mining disabled during initial block download. "
                    "Sync progress: %.1f%% (blocks: %lu/%lu)",
                    progress,
                    static_cast<unsigned long>(chain.current_height),
                    static_cast<unsigned long>(chain.total_blocks));
            progress_msg = std::string(progress_buf);
        }

        return MiningPolicyResult::Error(-10, progress_msg);
    }

    // ═══════════════════════════════════════════════════════════════════
    // All checks passed
    // ═══════════════════════════════════════════════════════════════════

    return MiningPolicyResult::Success();
}

/**
 * @brief Check if mining can resume after daemon restart (Phase E.3)
 *
 * Contract: E.3 — Restart Semantics
 *
 * This function is PURE:
 * - No MiningManager calls
 * - No persistence writes
 * - No daemon lifecycle hooks
 * - Deterministic (same inputs → same output)
 *
 * Policy Enforcement:
 * - E.3.1: Mining does NOT auto-resume after restart
 * - E.3.2: Mining requires explicit mining.start call
 * - E.3.3: Configuration persists, state does not
 *
 * Rationale:
 * Bitcoin Core behavior - explicit > implicit.
 * Users should consciously decide to mine after restart.
 *
 * @param restart View of restart state (injected by caller)
 * @return MiningPolicyResult indicating allowed/denied with restart flag
 */
inline MiningPolicyResult CheckMiningResumePolicy(
    const RestartPolicyView& restart
) {
    // ═══════════════════════════════════════════════════════════════════
    // Phase E.3.1: Mining does NOT auto-resume after restart
    // ═══════════════════════════════════════════════════════════════════

    if (restart.is_fresh_start && restart.mining_was_active_before) {
        // Contract: CONFIG persists, STATE does not
        // Mining address persists (config), but mining enabled state does not
        return MiningPolicyResult::RestartRequired(
            "Mining does not auto-resume after restart. "
            "Call mining.start explicitly."
        );
    }

    // ═══════════════════════════════════════════════════════════════════
    // Not a restart scenario - allow
    // ═══════════════════════════════════════════════════════════════════

    return MiningPolicyResult::Success();
}

/**
 * @brief Check if wallet can be switched (Phase E.4)
 *
 * Contract: E.4.1 — Wallet Switch Prevention
 *
 * This function is PURE:
 * - No WalletManager calls
 * - No mining state modification
 * - Deterministic (same inputs → same output)
 *
 * Policy Enforcement:
 * - E.4.1: Cannot switch wallet while mining is active
 * - Prevents silent address changes
 * - Prevents accidental reward loss
 *
 * Rationale:
 * User must explicitly stop mining before changing wallet context.
 *
 * @param mining_state View of current mining state
 * @return MiningPolicyResult indicating allowed/denied
 */
inline MiningPolicyResult CheckWalletSwitchPolicy(
    const MiningStatePolicyView& mining_state
) {
    // ═══════════════════════════════════════════════════════════════════
    // Phase E.4.1: Cannot switch wallet while mining active
    // ═══════════════════════════════════════════════════════════════════

    if (mining_state.is_mining_active) {
        // Contract: Mining context MUST be explicit
        // Switching wallets mid-mining would change reward destination silently
        return MiningPolicyResult::Error(
            -13,
            "Cannot switch wallet while mining is active. "
            "Stop mining first: mining.stop"
        );
    }

    // ═══════════════════════════════════════════════════════════════════
    // Mining not active - wallet switch allowed
    // ═══════════════════════════════════════════════════════════════════

    return MiningPolicyResult::Success();
}

/**
 * @brief Check if mining.stop is allowed (Phase E.4)
 *
 * Contract: E.4.2 — mining.stop Invariants
 *
 * This function is PURE:
 * - No MiningManager calls
 * - No state modification
 * - Deterministic (same inputs → same output)
 *
 * Policy Enforcement:
 * - E.4.2: mining.stop is ALWAYS allowed (idempotent)
 * - E.4.2: mining.stop ALWAYS succeeds
 * - E.4.2: Stopping already-stopped mining is not an error
 *
 * Rationale:
 * Stop operations should be unconditionally safe.
 * Idempotency prevents race conditions and simplifies client logic.
 *
 * @return MiningPolicyResult always returns Success
 */
inline MiningPolicyResult CheckMiningStopPolicy() {
    // ═══════════════════════════════════════════════════════════════════
    // Phase E.4.2: mining.stop is always allowed
    // ═══════════════════════════════════════════════════════════════════

    // Contract: Stop is idempotent and unconditionally safe
    // - If mining is active → stop it
    // - If mining is NOT active → no-op (still success)
    // - No error conditions exist for stop

    return MiningPolicyResult::Success();
}

} // namespace rpc
} // namespace dinero
