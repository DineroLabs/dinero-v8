// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — daemon-scoped runtime owner. Constructs and
// keeps alive the singleton VaultService for the dinerod process,
// and registers it with the RPC layer.
//
// Wiring contract:
//   - call InitializeVaultRuntime() once at daemon startup, AFTER
//     chain services exist but BEFORE RegisterAllRPCMethods().
//   - call NotifyVaultTipConnected(height) on every ConnectBlock
//     success.
//   - call NotifyVaultTipDisconnected(height) on every block-disconnect.
//   - call ShutdownVaultRuntime() at daemon shutdown.
//
// All four are no-ops when the vault is disabled (operator hasn't
// flipped the feature flag yet); the daemon stays correct without
// vault.

#pragma once

#include "vault/vault_service.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct DaemonContext;

namespace dinero::vault {

class VaultService;
class LedgerStore;

struct VaultRuntimeConfig {
    /// `true` to start the vault. When false, all wiring hooks
    /// no-op and the RPC methods return "vault service not
    /// initialized".
    bool enabled{false};
    /// `true` to keep the deposit-flow machine in shadow mode
    /// (writes deposit_observed but never opens credits). Stage 0
    /// rollouts.
    bool shadow_mode{false};
    /// Legacy ledger-file path. The current runtime opens/reads this store,
    /// but its rows do not restore complete service state and live service
    /// transitions are not durably appended here. This is not a recovery owner.
    std::string persistence_path;
    /// Bech32m operator address (e.g. "din1p..."). Decoded once at
    /// init; the resulting scriptPubKey is the observer's match
    /// key. Empty disables the auto-observer; deposits can still
    /// flow in via `vault.observe`.
    std::string operator_address;
    /// Account ID that auto-observed deposits credit to. Defaults
    /// to "default" if `operator_address` is set but this is empty.
    std::string default_account;
    /// K-confirmation policy (defaults: k_observe=1, k_credit=10,
    /// k_settle=20 — conservative starting point for first
    /// real-funds runs; tune via `-vault.k_credit` / `-vault.k_settle`).
    uint64_t k_observe{1};
    uint64_t k_credit{10};
    uint64_t k_settle{20};
    /// Whole-tip reader used by production daemon wiring. This captures all
    /// tracked deposits under one selected-chain owner, outside service locks.
    VaultTipSnapshotFn capture_tip;
    /// Narrow per-deposit readers retained for existing injected components.
    /// Chain-query closures wired by the daemon.
    std::function<std::array<uint8_t, 32>(uint64_t)> block_hash_at_height;
    /// Caller looks up whether `(outpoint)` is included in the block
    /// at `(height, block_hash)`. The daemon reader preserves the typed
    /// body and throws on unavailable or changed source state.
    std::function<bool(const std::array<uint8_t, 32>&, uint32_t, uint64_t,
                       const std::array<uint8_t, 32>&)>
        tx_included_at;
};

/// Initialise the singleton VaultService + register it with RPC.
/// Returns false only when disabled; an already published runtime returns true.
/// Enabled initialization failures throw before publication. Nonempty legacy
/// ledger/idempotency files refuse: those files cannot restore complete vault
/// state and must never be silently replaced by an empty runtime. This legacy
/// initializer is not the authenticated wallet-state recovery attachment.
bool InitializeVaultRuntime(VaultRuntimeConfig config);

/// Tear down the singleton (daemon shutdown).
void ShutdownVaultRuntime();

/// Block-connect hook. Called from validation_queue after
/// ConnectBlock succeeds.
void NotifyVaultTipConnected(uint64_t height);

/// Block-disconnect hook. Called from the reorg path. Currently a
/// no-op stub since the watcher detects reorgs via the next
/// ConnectBlock's hash mismatch; reserved for richer signaling.
void NotifyVaultTipDisconnected(uint64_t height);

/// Capture the current service under runtime ownership, or an empty owner.
/// Retain it through the complete call; shutdown detaches future acquisitions.
/// A retained service is not a readiness or durable-completion certificate.
std::shared_ptr<VaultService> GetVaultRuntimeService();

/// Update the operator address ↔ account binding at runtime. Decodes
/// the address (must be a Taproot din1p…) and atomically replaces
/// the auto-observer's match key. Pass empty `address` to disable
/// the auto-observer. Returns true on success; on decode failure
/// the existing binding is preserved and `error_out` is filled.
bool SetVaultOperator(const std::string& address,
                      const std::string& account,
                      std::string* error_out);

/// Read the current operator address + account binding. `address` is
/// empty if no operator is configured.
struct OperatorBinding {
    std::string address;
    std::string account;
};
OperatorBinding GetVaultOperator();

/// Capture service, operator script/account and chain-service lifetime before
/// acquiring a wallet database lease. Invoke after wallet/index commits and
/// release of that lease. A detached runtime or changed binding cannot retarget
/// this captured delivery. Empty means no observer configured at capture time.
/// This is a retained callback, not a durable acknowledgement or readiness.
using VaultWalletOutputObserver = std::function<void(
    const std::array<uint8_t,32>&, uint32_t, const std::vector<uint8_t>&,
    uint64_t, uint64_t, const std::string&)>;
VaultWalletOutputObserver CaptureVaultWalletOutputObserver();

/// Wallet-side hook: a UTXO with `script_pub_key` matching our
/// configured operator address landed at (txid, vout) in the block
/// at `block_hash_hex` and `height`. No-op if vault not running, no
/// operator address configured, or the script doesn't match.
/// `block_hash_hex` is the *display-order* hex (matches what wallet
/// worker logs); the runtime reverses the bytes internally to get
/// the consensus-order hash.
void ObserveWalletOutput(const std::array<uint8_t, 32>& txid_raw,
                         uint32_t vout,
                         const std::vector<uint8_t>& script_pub_key,
                         uint64_t amount_una,
                         uint64_t height,
                         const std::string& block_hash_hex);

/// SECURITY (F-CRIT-03, 2026-05-29) — trust boundary for the `vault.observe` RPC.
/// Verifies a *claimed* operator deposit against chainstate: the outpoint
/// (`txid_raw`, `vout`) must exist in the UTXO set, pay the configured operator
/// script, and be a transparent (non-confidential) output. On success returns the
/// REAL on-chain value/height/consensus-order block-hash via the out-params and
/// `true`; on any mismatch sets `err` and returns `false`. Callers MUST use the
/// returned values and never trust caller-supplied amount/height/block_hash.
bool VerifyOperatorDeposit(const std::shared_ptr<VaultService>& expected_service,
                           const std::array<uint8_t, 32>& txid_raw,
                           uint32_t vout,
                           uint64_t& out_amount,
                           uint64_t& out_height,
                           std::array<uint8_t, 32>& out_block_hash_raw,
                           std::string& err);

/// Capture the current chainstate service by strong ownership. Call after
/// service wiring. The checked canonical hash reader returns zero when source
/// state is unavailable; no body read or deposit acknowledgement is implied.
std::function<std::array<uint8_t,32>(uint64_t)>
MakeChainstateBlockHashClosure(::DaemonContext& ctx);

/// Check one exact output in the selected canonical body, preserving historical
/// and Orchard transaction families. Throws when unavailable or when the
/// expected canonical hash changed; only a complete read returns true/false.
/// The watcher must retain its prior observation if this call throws.
std::function<bool(const std::array<uint8_t,32>&,uint32_t,uint64_t,const std::array<uint8_t,32>&)>
MakeChainstateTxIncludedClosure(::DaemonContext& ctx);

/// One selected observation of the exact requested tip and all queries.
/// Throws on unavailable storage, changed tip or incomplete body reads.
VaultTipSnapshotFn MakeChainstateVaultSnapshotClosure(::DaemonContext& ctx);

}  // namespace dinero::vault
