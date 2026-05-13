// Copyright (c) 2026 Dinero Labs.
//
// Daemon-side port of `Core/Vault/LedgerAccount.swift`. State
// transitions only; lifecycle invariants live in `Ledger::validate`.

#include "vault/ledger_account.h"

#include <algorithm>
#include <variant>

namespace dinero::vault {

void LedgerAccount::applyDepositObserved(const OutpointId& deposit, UnaAmount amount) {
    // Idempotent: same outpoint re-observed is a no-op.
    if (deposits_.find(deposit) == deposits_.end()) {
        deposits_[deposit] = DepositObservedState{amount};
    }
}

void LedgerAccount::applyCreditOpened(const OutpointId& deposit, UnaAmount amount) {
    deposits_[deposit] = DepositCreditedState{amount};
    pending_ += amount;
}

void LedgerAccount::applyCreditSettled(const OutpointId& deposit) {
    auto it = deposits_.find(deposit);
    if (it == deposits_.end()) {
        return;
    }
    auto* credited = std::get_if<DepositCreditedState>(&it->second);
    if (credited == nullptr) {
        return;
    }
    UnaAmount amount = credited->amount;
    it->second = DepositSettledState{amount};
    pending_ = pending_ >= amount ? pending_ - amount : 0;
    confirmed_ += amount;
}

void LedgerAccount::applyCreditReverted(const OutpointId& deposit) {
    auto it = deposits_.find(deposit);
    if (it == deposits_.end()) {
        return;
    }
    if (auto* credited = std::get_if<DepositCreditedState>(&it->second); credited != nullptr) {
        UnaAmount amount = credited->amount;
        it->second = DepositRevertedState{amount};
        pending_ = pending_ >= amount ? pending_ - amount : 0;
        return;
    }
    if (auto* settled = std::get_if<DepositSettledState>(&it->second); settled != nullptr) {
        // Settled credit reverted — caller must follow with
        // applyCompensatingDebit. Keep state consistent here: the
        // confirmed bucket shrinks by amount.
        UnaAmount amount = settled->amount;
        it->second = DepositRevertedState{amount};
        confirmed_ = confirmed_ >= amount ? confirmed_ - amount : 0;
        return;
    }
    // observed / reverted: nothing to roll back at the credit layer.
}

void LedgerAccount::applyWithdrawalInitiated(const OutpointId& request, UnaAmount amount, const BackendId& backend) {
    if (withdrawals_.find(request) == withdrawals_.end()) {
        withdrawals_[request] = WithdrawalInitiatedState{amount, backend};
        locked_ += amount;
    }
}

void LedgerAccount::applyWithdrawalSettled(const OutpointId& request) {
    auto it = withdrawals_.find(request);
    if (it == withdrawals_.end()) {
        return;
    }
    auto* initiated = std::get_if<WithdrawalInitiatedState>(&it->second);
    if (initiated == nullptr) {
        return;
    }
    UnaAmount amount = initiated->amount;
    BackendId backend = initiated->backend;
    it->second = WithdrawalSettledState{amount, backend};
    // Locked → outflow. Drain confirmed first, then pending if the
    // user is spending against a not-yet-settled credit.
    UnaAmount from_confirmed = std::min(confirmed_, amount);
    confirmed_ -= from_confirmed;
    UnaAmount remaining = amount - from_confirmed;
    UnaAmount from_pending = std::min(pending_, remaining);
    pending_ -= from_pending;
    locked_ = locked_ >= amount ? locked_ - amount : 0;
}

void LedgerAccount::applyWithdrawalReverted(const OutpointId& request) {
    auto it = withdrawals_.find(request);
    if (it == withdrawals_.end()) {
        return;
    }
    auto* initiated = std::get_if<WithdrawalInitiatedState>(&it->second);
    if (initiated == nullptr) {
        return;
    }
    UnaAmount amount = initiated->amount;
    BackendId backend = initiated->backend;
    it->second = WithdrawalRevertedState{amount, backend};
    locked_ = locked_ >= amount ? locked_ - amount : 0;
}

void LedgerAccount::applyCompensatingDebit(const OutpointId& deposit, UnaAmount amount, UnaAmount operator_loss) {
    // CompensatingDebit is purely an operator-side accounting entry.
    // The companion CreditReverted has already rolled back pending /
    // confirmed; the caller supplies `operator_loss` as the
    // unrecoverable portion (computed by ReorgWatcher::unrecoverableLoss
    // from the user's absorptive capacity at revert time). Touching
    // pending / confirmed here would double-debit the user. Keep
    // balances intact, just bump the per-account operator loss.
    operatorLoss_ += operator_loss;

    auto it = deposits_.find(deposit);
    if (it != deposits_.end()) {
        if (!std::holds_alternative<DepositRevertedState>(it->second)) {
            it->second = DepositRevertedState{amount};
        }
    }
}

void LedgerAccount::applyPolicyAdjustment(int64_t delta_user_balance) {
    if (delta_user_balance >= 0) {
        confirmed_ += static_cast<UnaAmount>(delta_user_balance);
        return;
    }
    auto magnitude = static_cast<UnaAmount>(-delta_user_balance);
    UnaAmount from_confirmed = std::min(confirmed_, magnitude);
    confirmed_ -= from_confirmed;
    UnaAmount uncovered = magnitude - from_confirmed;
    operatorLoss_ += uncovered;
}

}  // namespace dinero::vault
