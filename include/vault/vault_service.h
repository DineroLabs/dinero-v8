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

class LedgerStore;

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

    /// `store` (optional) makes the ledger durable across restarts: the
    /// constructor replays whatever it holds into the in-memory ledger
    /// and then attaches it as the write-ahead sink for new entries.
    /// Replay happens with the store DETACHED, so a restart does not
    /// re-persist history. Throws LedgerError if the persisted log fails
    /// validation — better a dead vault than a half-loaded one.
    ///
    /// NOTE: only the LEDGER is restored. DepositFlowMachine,
    /// ReorgWatcher and WithdrawalQueue keep state that is not ledgered
    /// (deposit height, enclosing block hash, destination script), so
    /// anything mid-lifecycle at shutdown comes back with correct
    /// balances but a frozen lifecycle. See unreconciledDeposits() /
    /// unreconciledWithdrawals().
    VaultService(std::unique_ptr<SigningBackend> backend, VaultServiceConfig config,
                 BlockHashAtHeightFn block_hash_at_height, TxIncludedAtFn tx_included_at,
                 LedgerStore* store = nullptr);

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

    /// RPC-side: move `amount` una of SETTLED balance from `from` to
    /// `to` inside the vault. Atomic — one InternalTransfer ledger
    /// entry carries both legs, so no replay can observe a half-applied
    /// transfer. Nothing touches the chain and no fee is charged.
    ///
    /// Only `confirmed` funds move (`LedgerAccount::transferable()`);
    /// un-settled credits stay put so a reorg's CompensatingDebit lands
    /// on an account that still holds the money. Throws LedgerError
    /// (TRANSFER_INVALID / INSUFFICIENT_TRANSFERABLE_BALANCE) on
    /// validation failure, leaving ledger state unchanged.
    ///
    /// Returns the seq of the appended entry.
    LedgerSeq transfer(const AccountId& from, const AccountId& to, UnaAmount amount);

    /// Settled balance `account` may move to another vault account.
    [[nodiscard]] UnaAmount accountTransferable(const AccountId& account);

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

    /// Deposits that replay found credited-but-not-settled. No live state
    /// machine will advance them; the funds stay spendable meanwhile.
    [[nodiscard]] size_t unreconciledDeposits() const noexcept { return unreconciled_deposits_; }

    /// Withdrawals replay found reserved (WithdrawalInitiated) with NO
    /// WithdrawalBroadcastRecorded: the write-ahead reservation is durable
    /// but the coins never went on the wire. `locked` is held for nothing
    /// and is safe for an operator to release.
    [[nodiscard]] size_t withdrawalsReservedNotBroadcast() const noexcept {
        return withdrawals_reserved_not_broadcast_;
    }

    /// Withdrawals replay found broadcast but not settled. The coins ARE
    /// on the wire. `locked` must NOT be released — doing so would hand
    /// the balance back on top of a real payout. Reconcile the recorded
    /// txid against the chain instead.
    [[nodiscard]] size_t withdrawalsBroadcastNotSettled() const noexcept {
        return withdrawals_broadcast_not_settled_;
    }

    /// Return ledger entries with seq >= since (capped at limit).
    [[nodiscard]] std::vector<LedgerEntry> entriesSince(LedgerSeq since, size_t limit = 1000);

    [[nodiscard]] WithdrawalState withdrawalState(const WithdrawalId& id);

    /// Backend healthcheck pass-through.
    [[nodiscard]] HealthReport backendHealth();

   private:
    /// Tallies mid-lifecycle deposits/withdrawals found by replay.
    /// Called once, from the constructor, before the store is attached.
    void countUnreconciled();

    std::mutex mu_;
    std::unique_ptr<SigningBackend> backend_;
    Ledger ledger_;
    DepositFlowMachine deposit_flow_;
    ReorgWatcher reorg_watcher_;
    WithdrawalQueue withdrawals_;
    size_t unreconciled_deposits_{0};
    size_t withdrawals_reserved_not_broadcast_{0};
    size_t withdrawals_broadcast_not_settled_{0};
};

}  // namespace dinero::vault
