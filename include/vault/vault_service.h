// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — top-level orchestrator. Daemon-side single
// source of truth that every client (DineroDPI iOS, dinero-qt,
// CLI, pool dashboard, web) talks to via RPC.
//
// Owns the Ledger, DepositFlowMachine, ReorgWatcher, WithdrawalQueue,
// and SigningBackend. Offers four public verbs:
//   recordDeposit       — chainstate-side caller after a confirmed
//                         UTXO is observed.
//   tipChanged          — chainstate-side caller on every block-connect.
//   tipReorged          — chainstate-side caller on block-disconnect /
//                         reorg event.
//   enqueueWithdrawal   — RPC caller (vault.withdraw).

#pragma once

#include "vault/deposit_flow.h"
#include "vault/ledger.h"
#include "vault/reorg_watcher.h"
#include "vault/signing_backend.h"
#include "vault/vault_types.h"
#include "vault/withdrawal_queue.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dinero::vault {

/// Settings the service accepts at construction. All fields have
/// safe defaults so a deployment can spin up an instance without
/// pre-configuring anything except the signing backend.
struct VaultServiceConfig {
    LedgerCaps ledger_caps;
    ConfirmationPolicy confirmation_policy;
    WithdrawalCaps withdrawal_caps;
    WithdrawalConfirmationPolicy withdrawal_policy;
    /// `true` keeps the deposit-flow machine in shadow mode (writes
    /// depositObserved but never opens credits). Default false (real
    /// production behaviour). Stage 0 deployments override.
    bool shadow_mode{false};
};

/// Single-actor orchestrator. The vault service serializes through
/// one mutex for thread-safety. Every public verb is idempotent on
/// its natural identity (outpoint for deposits, request_id for
/// withdrawals).
class VaultService {
   public:
    using BlockHashAtHeightFn = ReorgWatcher::BlockHashAtHeightFn;
    using TxIncludedAtFn = ReorgWatcher::TxIncludedAtFn;

    VaultService(std::unique_ptr<SigningBackend> backend, VaultServiceConfig config,
                 BlockHashAtHeightFn block_hash_at_height, TxIncludedAtFn tx_included_at);

    /// Chainstate-side: a confirmed UTXO with `txid:vout` belongs to
    /// `account`. Idempotent. The caller (typically a wallet hook
    /// inside ConnectBlock) supplies the enclosing block hash so the
    /// reorg watcher can later detect chain-level reverts.
    void recordDeposit(const std::array<uint8_t, 32>& txid, uint32_t vout,
                       const AccountId& account, UnaAmount amount, uint64_t height,
                       const std::array<uint8_t, 32>& block_hash);

    /// Chainstate-side: a new block was connected. Drives:
    ///   1. deposit-flow lifecycle advancement
    ///   2. reorg-watcher checks (cheap when no reorg)
    ///   3. withdrawal-queue settlement at K confirmations
    void tipChanged(uint64_t height);

    /// RPC-side: enqueue a withdrawal for `account`. Returns the
    /// stable request id. Throws WithdrawalQueueError on validation
    /// failure (insufficient spendable, cap exceeded, bad destination).
    WithdrawalId enqueueWithdrawal(const AccountId& account, UnaAmount amount,
                                   const std::vector<uint8_t>& destination_script_pub_key);

    /// Driver for the withdrawal queue's signing path. Caller (a
    /// vault main loop or per-tip task) calls this on a cadence; one
    /// call advances at most one pending withdrawal.
    std::optional<WithdrawalId> processNextWithdrawal();

    /// Caller supplies the inclusion height for a previously-broadcast
    /// withdrawal tx (called from chainstate when the broadcast tx
    /// makes it into a block).
    void markWithdrawalIncluded(const WithdrawalId& id, uint64_t height);

    // ----- introspection (used by RPC handlers) -----

    [[nodiscard]] UnaAmount accountSpendable(const AccountId& account);
    [[nodiscard]] UnaAmount accountConfirmed(const AccountId& account);
    [[nodiscard]] UnaAmount accountPending(const AccountId& account);
    [[nodiscard]] UnaAmount accountLocked(const AccountId& account);
    [[nodiscard]] UnaAmount accountOperatorLoss(const AccountId& account);

    [[nodiscard]] UnaAmount totalOpenCredits();
    [[nodiscard]] UnaAmount totalOperatorLoss();
    [[nodiscard]] uint64_t ledgerNextSeq();
    [[nodiscard]] size_t accountCount();
    [[nodiscard]] int withdrawalQueueDepth();

    /// Return ledger entries with seq >= since (capped at limit).
    [[nodiscard]] std::vector<LedgerEntry> entriesSince(LedgerSeq since, size_t limit = 1000);

    [[nodiscard]] WithdrawalState withdrawalState(const WithdrawalId& id);

    /// Backend healthcheck pass-through.
    [[nodiscard]] HealthReport backendHealth();

   private:
    std::mutex mu_;
    std::unique_ptr<SigningBackend> backend_;
    Ledger ledger_;
    DepositFlowMachine deposit_flow_;
    ReorgWatcher reorg_watcher_;
    WithdrawalQueue withdrawals_;
};

}  // namespace dinero::vault
