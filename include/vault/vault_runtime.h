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

#include <array>
#include <cstdint>
#include <functional>
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
    /// Optional persistence path. When set, a FileLedgerStore is
    /// created and replayed at startup; appends are tee'd into
    /// the file.
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
    /// Chain-query closures wired by the daemon.
    std::function<std::array<uint8_t, 32>(uint64_t)> block_hash_at_height;
    /// Caller looks up whether `(outpoint)` is included in the block
    /// at `(height, block_hash)`. Daemon implementation walks the
    /// block's tx list. Stage-0-friendly default: always-true so
    /// `RE_MINED_SAME_TXID` is the conservative outcome.
    std::function<bool(const std::array<uint8_t, 32>&, uint32_t, uint64_t,
                       const std::array<uint8_t, 32>&)>
        tx_included_at;
};

/// Initialise the singleton VaultService + register it with RPC.
/// Idempotent — second call is a no-op.
void InitializeVaultRuntime(VaultRuntimeConfig config);

/// Tear down the singleton (daemon shutdown).
void ShutdownVaultRuntime();

/// Block-connect hook. Called from validation_queue after
/// ConnectBlock succeeds.
void NotifyVaultTipConnected(uint64_t height);

/// Block-disconnect hook. Called from the reorg path. Currently a
/// no-op stub since the watcher detects reorgs via the next
/// ConnectBlock's hash mismatch; reserved for richer signaling.
void NotifyVaultTipDisconnected(uint64_t height);

/// Get the live service pointer (or nullptr if not initialised).
/// Internal helper for RPC handlers.
VaultService* GetVaultRuntimeService();

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

/// Build a `block_hash_at_height` closure backed by the live
/// ChainDB on `ctx`. Returns the canonical hash of the active-chain
/// block at `height`, or a 32-byte zero array as the "unknown"
/// sentinel (block not yet on disk, ChainDB unavailable, etc.).
/// The closure captures `&ctx` by reference; caller must guarantee
/// `ctx` outlives the vault runtime (true for the daemon-static
/// DaemonContext).
std::function<std::array<uint8_t, 32>(uint64_t)>
MakeChainstateBlockHashClosure(::DaemonContext& ctx);

/// Build a `tx_included_at` closure backed by the live ChainDB on
/// `ctx`. Loads the block at `block_hash` and answers whether a
/// transaction with txid `outpoint.txid_raw` (and ≥ `vout+1`
/// outputs) is present. On any chain-query failure (block missing,
/// deserialisation error, ChainDB unavailable) returns `true`
/// conservatively — keeps the reorg watcher in RE_MINED_SAME_TXID
/// rather than ORPHANED, avoiding a false compensating-debit cascade
/// on transient block-fetch hiccups.
std::function<bool(const std::array<uint8_t, 32>&, uint32_t, uint64_t,
                   const std::array<uint8_t, 32>&)>
MakeChainstateTxIncludedClosure(::DaemonContext& ctx);

}  // namespace dinero::vault
