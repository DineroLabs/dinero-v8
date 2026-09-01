// Copyright (c) 2026 Dinero Labs.
//
// Liquidity Vault — per-account state derived from a replay of
// LedgerEntry values. Pure value semantics; the Ledger coordinator
// owns the live instance.
//
// Daemon-side port of `Core/Vault/LedgerAccount.swift`.
//
// Balance model (LIQUIDITY_VAULT_DESIGN.md §6.1):
//   - pending      : credit_opened but not yet settled (operator
//                    at-risk).
//   - confirmed    : credit_settled (fully on-chain).
//   - locked       : withdrawal_initiated but not yet settled.
//   - spendable    : pending + confirmed − locked  (UI-facing).

#pragma once

#include "vault/vault_types.h"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <variant>

namespace dinero::vault {

/// Lifecycle state for one tracked deposit. Mirrors the design
/// doc's `{detected, observed, credited, settled, reverted}` set,
/// minus `detected` which lives in DepositFlowMachine (not yet
/// ledger-resident).
struct DepositObservedState {
    UnaAmount amount{0};
    bool operator==(const DepositObservedState&) const = default;
};
struct DepositCreditedState {
    UnaAmount amount{0};
    bool operator==(const DepositCreditedState&) const = default;
};
struct DepositSettledState {
    UnaAmount amount{0};
    bool operator==(const DepositSettledState&) const = default;
};
struct DepositRevertedState {
    UnaAmount amount{0};
    bool operator==(const DepositRevertedState&) const = default;
};
using DepositLifecycle = std::variant<
    DepositObservedState,
    DepositCreditedState,
    DepositSettledState,
    DepositRevertedState
>;

struct WithdrawalInitiatedState {
    UnaAmount amount{0};
    BackendId backend;
    bool operator==(const WithdrawalInitiatedState&) const = default;
};
struct WithdrawalSettledState {
    UnaAmount amount{0};
    BackendId backend;
    bool operator==(const WithdrawalSettledState&) const = default;
};
struct WithdrawalRevertedState {
    UnaAmount amount{0};
    BackendId backend;
    bool operator==(const WithdrawalRevertedState&) const = default;
};
using WithdrawalLifecycle = std::variant<
    WithdrawalInitiatedState,
    WithdrawalSettledState,
    WithdrawalRevertedState
>;

class LedgerAccount {
   public:
    explicit LedgerAccount(AccountId account) : account_{std::move(account)} {}

    // Balance accessors.
    [[nodiscard]] const AccountId& account() const noexcept { return account_; }
    [[nodiscard]] UnaAmount pending() const noexcept { return pending_; }
    [[nodiscard]] UnaAmount confirmed() const noexcept { return confirmed_; }
    [[nodiscard]] UnaAmount locked() const noexcept { return locked_; }
    [[nodiscard]] UnaAmount operatorLoss() const noexcept { return operatorLoss_; }
    /// Spendable balance the UI displays. pending + confirmed − locked,
    /// saturated at 0 (per invariant §6.2.3 a negative balance is
    /// represented as `operatorLoss`, never as a negative number).
    [[nodiscard]] UnaAmount spendable() const noexcept {
        const UnaAmount K_CREDIT = pending_ + confirmed_;
        return K_CREDIT >= locked_ ? K_CREDIT - locked_ : 0;
    }

    /// Balance that may move to another vault account: settled funds
    /// that are not already committed to an in-flight withdrawal.
    /// `min(spendable, confirmed)` — deliberately excludes `pending`,
    /// which is operator-at-risk until the deposit settles (see
    /// InternalTransfer in ledger_entry.h).
    [[nodiscard]] UnaAmount transferable() const noexcept {
        const UnaAmount free = spendable();
        return free < confirmed_ ? free : confirmed_;
    }

    [[nodiscard]] const std::unordered_map<OutpointId, DepositLifecycle>& deposits() const noexcept {
        return deposits_;
    }
    [[nodiscard]] const std::unordered_map<OutpointId, WithdrawalLifecycle>& withdrawals() const noexcept {
        return withdrawals_;
    }

    // State transitions (called by the Ledger coordinator on replay).

    void applyDepositObserved(const OutpointId& deposit, UnaAmount amount);
    void applyCreditOpened(const OutpointId& deposit, UnaAmount amount);
    void applyCreditSettled(const OutpointId& deposit);
    void applyCreditReverted(const OutpointId& deposit);

    void applyWithdrawalInitiated(const OutpointId& request, UnaAmount amount, const BackendId& backend);
    void applyWithdrawalSettled(const OutpointId& request);
    void applyWithdrawalReverted(const OutpointId& request);

    void applyCompensatingDebit(const OutpointId& deposit, UnaAmount amount, UnaAmount operator_loss);

    void applyPolicyAdjustment(int64_t delta_user_balance);

    /// Debit / credit legs of an InternalTransfer. Both move `confirmed`
    /// only; the Ledger validates sufficiency before either is called.
    void applyInternalTransferOut(UnaAmount amount);
    void applyInternalTransferIn(UnaAmount amount);

    bool operator==(const LedgerAccount& other) const = default;

   private:
    AccountId account_;
    UnaAmount pending_{0};
    UnaAmount confirmed_{0};
    UnaAmount locked_{0};
    UnaAmount operatorLoss_{0};
    std::unordered_map<OutpointId, DepositLifecycle> deposits_;
    std::unordered_map<OutpointId, WithdrawalLifecycle> withdrawals_;
};

}  // namespace dinero::vault
