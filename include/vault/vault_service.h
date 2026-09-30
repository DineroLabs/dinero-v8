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
#include "vault/state_owner.h"
#include "vault/vault_types.h"
#include "vault/withdrawal_queue.h"
#include "vault/withdrawal_dispatch.h"

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

/// Immutable metrics captured under one service lock. These describe one
/// in-memory state; they do not acknowledge chain delivery or durable storage.
struct VaultAccountMetrics {
    UnaAmount spendable{0}, confirmed{0}, pending{0}, locked{0}, operator_loss{0};
    bool operator==(const VaultAccountMetrics&) const = default;
};
struct VaultMetrics {
    UnaAmount total_open_credits{0}, total_operator_loss{0};
    LedgerSeq ledger_next_seq{0};
    size_t account_count{0};
    int withdrawal_queue_depth{0};
    bool operator==(const VaultMetrics&) const = default;
};

/// Inputs and results for one selected-chain observation. Included outputs
/// must bind the exact outpoint, height and transparent amount. An absent
/// block hash denotes a height above the completely observed selected tip.
struct VaultDepositQuery {
    OutpointId outpoint;
    uint64_t height{0};
    UnaAmount amount{0};
    bool operator==(const VaultDepositQuery&) const = default;
};
struct VaultDepositObservation {
    VaultDepositQuery query;
    std::optional<std::array<uint8_t,32>> block_hash;
    bool included{false};
};
struct VaultTipSnapshot {
    uint64_t height{0};
    std::array<uint8_t,32> block_hash{};
    std::vector<VaultDepositObservation> deposits;
};
using VaultTipSnapshotFn = std::function<VaultTipSnapshot(
    uint64_t, const std::vector<VaultDepositQuery>&)>;

/// Single-actor orchestrator. The vault service serializes through
/// one mutex for thread-safety. Every public verb is idempotent on
/// its natural identity (outpoint for deposits, request_id for
/// withdrawals).
struct VaultStateSnapshot;

class VaultService {
   public:
    using BlockHashAtHeightFn = ReorgWatcher::BlockHashAtHeightFn;
    using TxIncludedAtFn = ReorgWatcher::TxIncludedAtFn;

    VaultService(std::unique_ptr<SigningBackend> backend, VaultServiceConfig config,
                 BlockHashAtHeightFn block_hash_at_height, TxIncludedAtFn tx_included_at,
                 VaultTipSnapshotFn capture_tip = {});

    /// Chainstate-side: a confirmed UTXO with `txid:vout` belongs to
    /// `account`. Idempotent. The caller (typically a wallet hook
    /// inside ConnectBlock) supplies the enclosing block hash so the
    /// reorg watcher can later detect chain-level reverts.
    void recordDeposit(const std::array<uint8_t, 32>& txid, uint32_t vout,
                       const AccountId& account, UnaAmount amount, uint64_t height,
                       const std::array<uint8_t, 32>& block_hash);

    /// Chainstate-side: a new block was connected. Drives:
    ///   1. checked inclusion/reorg reconciliation for every tracked deposit
    ///   2. deposit-flow lifecycle advancement
    ///   3. withdrawal-queue settlement at K confirmations
    /// Publishes all in-memory state only when these phases complete.
    /// Unknown chain observations and lifecycle/cap errors propagate without
    /// changing the prior state.
    void tipChanged(uint64_t height);
    // A configured snapshot reader runs outside mu_. Publication refuses if
    // any service mutation occurred during the read. Legacy injected readers
    // retain their narrower per-deposit behavior.

    /// RPC-side: enqueue a withdrawal for `account`. Returns the
    /// stable request id. Throws WithdrawalQueueError on validation
    /// failure (insufficient spendable, cap exceeded, bad destination).
    WithdrawalId enqueueWithdrawal(const AccountId& account, UnaAmount amount,
                                   const std::vector<uint8_t>& destination_script_pub_key);

    // Explicit terms are retained in the same authenticated state transaction
    // as the new request. Requires a durable wallet owner; no historical terms
    // are inferred by this overload.
    WithdrawalId enqueueWithdrawal(const AccountId&, UnaAmount,
        const std::vector<uint8_t>&, const WithdrawalPaymentTerms&);

    /// Driver for the withdrawal queue's signing path. Caller (a
    /// vault main loop or per-tip task) calls this on a cadence; one
    /// call advances at most one pending withdrawal.
    std::optional<WithdrawalId> processNextWithdrawal();

    /// Caller supplies the inclusion height for a previously-broadcast
    /// withdrawal tx (called from chainstate when the broadcast tx
    /// makes it into a block).
    void markWithdrawalIncluded(const WithdrawalId& id, uint64_t height);

    // ----- introspection (used by RPC handlers) -----

    // Durable Pending/Signing reservations reduce spendable and contribute to
    // locked. Retained payments already have a ledger lock and count once.
    // These are captured live metrics, not a chain/readiness certificate.
    [[nodiscard]] VaultAccountMetrics accountMetrics(const AccountId& account);
    [[nodiscard]] VaultMetrics metrics();

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

    // Capture all present service fields under one mutex. Returned bytes/state
    // are not authenticated ownership, deletion completeness or readiness.
    // No chain, wallet, SQL or signing callback is invoked while capturing.
    [[nodiscard]] VaultStateSnapshot captureState();

    /// Backend healthcheck pass-through.
    [[nodiscard]] HealthReport backendHealth();

   private:
    friend class WalletVaultStateOwner;
    struct PreparedState;
    void publish(PreparedState& state) noexcept;
    void commitAndPublish(PreparedState&, VaultStateWrite*);
    void requireCurrentOwner(const VaultStateWrite*) const;
    VaultStateSnapshot captureStateLocked() const;
    static std::shared_ptr<VaultService> RestorePrepared(
        const VaultStateSnapshot&, std::shared_ptr<const VaultStateOwner>,
        std::unique_ptr<SigningBackend>, BlockHashAtHeightFn, TxIncludedAtFn,
        VaultTipSnapshotFn, std::shared_ptr<VaultWithdrawalDispatcher> = {});
    void requireRevisionCapacity() const;
    void requirePendingCapacity(const AccountId&, UnaAmount additional) const;
    UnaAmount pendingReservedLocked(const AccountId&, UnaAmount spendable) const;
    VaultAccountMetrics accountMetricsLocked(const AccountId&) const;
    std::optional<WithdrawalId> processDurableWithdrawal();
    // Immutable after construction; the wallet transaction is always acquired
    // before mu_. Ordinary injected services retain their in-memory behavior.
    std::shared_ptr<const VaultStateOwner> state_owner_;
    std::shared_ptr<VaultWithdrawalDispatcher> withdrawal_dispatcher_;
    std::mutex mu_;
    uint64_t revision_{0};
    VaultTipSnapshotFn capture_tip_;
    std::unique_ptr<SigningBackend> backend_;
    Ledger ledger_;
    DepositFlowMachine deposit_flow_;
    ReorgWatcher reorg_watcher_;
    WithdrawalQueue withdrawals_;
};

}  // namespace dinero::vault
