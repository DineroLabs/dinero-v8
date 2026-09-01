// Copyright (c) 2026 Dinero Labs.
//
// Daemon-side port of `Core/Vault/Ledger.swift`. Coordinator +
// invariant enforcement; per-account state lives in LedgerAccount.

#include "vault/ledger.h"

#include "vault/ledger_store.h"

#include <sstream>
#include <utility>
#include <variant>

namespace dinero::vault {

void Ledger::append(LedgerEntry entry) {
    LedgerSeq seq = entrySeq(entry);
    // Invariant §6.2.4: sequence monotonicity.
    if (seq < nextSeq_) {
        std::ostringstream oss;
        oss << "sequence not monotonic: expected at least " << nextSeq_ << " got " << seq;
        throw LedgerError(LedgerError::Kind::SEQUENCE_NOT_MONOTONIC, oss.str());
    }

    validate(entry);
    // Write-ahead: persist only after validation (so a rejected entry
    // leaves no trace to resurrect on replay) but before the in-memory
    // apply (so a crash between the two replays the entry rather than
    // losing it). A store failure throws with ledger state untouched.
    if (store_ != nullptr) {
        store_->append(entry);
    }
    applyToAccounts(entry);
    entries_.push_back(std::move(entry));
    nextSeq_ = seq + 1;
}

Ledger Ledger::replay(const std::vector<LedgerEntry>& entries, const LedgerCaps& caps) {
    Ledger ledger{caps};
    for (const auto& entry : entries) {
        ledger.append(entry);
    }
    return ledger;
}

LedgerAccount& Ledger::ensureAccount(const AccountId& account) {
    auto it = accounts_.find(account);
    if (it == accounts_.end()) {
        auto [inserted, ok] = accounts_.emplace(account, LedgerAccount{account});
        return inserted->second;
    }
    return it->second;
}

void Ledger::validate(const LedgerEntry& entry) {
    if (auto* opened = std::get_if<CreditOpened>(&entry); opened != nullptr) {
        // Replay-protection: reject if deposit is settled or already
        // reverted (design doc §5.4).
        auto acct_it = accounts_.find(opened->account);
        if (acct_it != accounts_.end()) {
            auto deposits_it = acct_it->second.deposits().find(opened->deposit);
            if (deposits_it != acct_it->second.deposits().end()) {
                if (std::holds_alternative<DepositSettledState>(deposits_it->second)) {
                    throw LedgerError(LedgerError::Kind::DEPOSIT_LIFECYCLE_CLOSED, "settled");
                }
                if (std::holds_alternative<DepositRevertedState>(deposits_it->second)) {
                    throw LedgerError(LedgerError::Kind::DEPOSIT_LIFECYCLE_CLOSED, "reverted");
                }
                if (std::holds_alternative<DepositCreditedState>(deposits_it->second)) {
                    throw LedgerError(
                        LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                        "creditOpened on already-credited deposit");
                }
            }
        }
        // Caps (design doc §5.3).
        if (opened->amount > caps_.per_deposit) {
            std::ostringstream oss;
            oss << "per-deposit cap exceeded: " << opened->amount << " > " << caps_.per_deposit;
            throw LedgerError(LedgerError::Kind::OPEN_CREDITS_EXCEED_CAP, oss.str());
        }
        UnaAmount per_account = openCreditsByAccount_.count(opened->account) != 0U
                                    ? openCreditsByAccount_.at(opened->account)
                                    : 0;
        per_account += opened->amount;
        if (per_account > caps_.per_user) {
            std::ostringstream oss;
            oss << "per-user cap exceeded: " << per_account << " > " << caps_.per_user;
            throw LedgerError(LedgerError::Kind::PER_USER_CAP_EXCEEDED, oss.str());
        }
        UnaAmount global = totalOpenCredits_ + opened->amount;
        if (global > caps_.global) {
            std::ostringstream oss;
            oss << "global cap exceeded: " << global << " > " << caps_.global;
            throw LedgerError(LedgerError::Kind::OPEN_CREDITS_EXCEED_CAP, oss.str());
        }
        return;
    }

    if (auto* settled = std::get_if<CreditSettled>(&entry); settled != nullptr) {
        auto acct_it = accounts_.find(settled->account);
        if (acct_it == accounts_.end()) {
            throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                              "creditSettled without prior credited state");
        }
        auto deposits_it = acct_it->second.deposits().find(settled->deposit);
        if (deposits_it == acct_it->second.deposits().end() ||
            !std::holds_alternative<DepositCreditedState>(deposits_it->second)) {
            throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                              "creditSettled without prior credited state");
        }
        return;
    }

    if (auto* reverted = std::get_if<CreditReverted>(&entry); reverted != nullptr) {
        auto acct_it = accounts_.find(reverted->account);
        if (acct_it == accounts_.end()) {
            throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                              "creditReverted on unknown deposit");
        }
        auto deposits_it = acct_it->second.deposits().find(reverted->deposit);
        if (deposits_it == acct_it->second.deposits().end()) {
            throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                              "creditReverted on unknown deposit");
        }
        if (std::holds_alternative<DepositObservedState>(deposits_it->second) ||
            std::holds_alternative<DepositRevertedState>(deposits_it->second)) {
            throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                              "creditReverted on observed/reverted deposit");
        }
        return;
    }

    if (auto* compensating = std::get_if<CompensatingDebit>(&entry); compensating != nullptr) {
        auto acct_it = accounts_.find(compensating->account);
        if (acct_it != accounts_.end()) {
            auto deposits_it = acct_it->second.deposits().find(compensating->deposit);
            if (deposits_it != acct_it->second.deposits().end() &&
                std::holds_alternative<DepositRevertedState>(deposits_it->second)) {
                return;
            }
        }
        throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                          "compensatingDebit without prior reverted state");
    }

    if (auto* w_settled = std::get_if<WithdrawalSettled>(&entry); w_settled != nullptr) {
        auto acct_it = accounts_.find(w_settled->account);
        if (acct_it == accounts_.end()) {
            throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                              "withdrawalSettled without initiated state");
        }
        auto withdrawals_it = acct_it->second.withdrawals().find(w_settled->request);
        if (withdrawals_it == acct_it->second.withdrawals().end() ||
            !std::holds_alternative<WithdrawalInitiatedState>(withdrawals_it->second)) {
            throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                              "withdrawalSettled without initiated state");
        }
        return;
    }

    if (auto* w_broadcast = std::get_if<WithdrawalBroadcastRecorded>(&entry);
        w_broadcast != nullptr) {
        auto acct_it = accounts_.find(w_broadcast->account);
        if (acct_it == accounts_.end()) {
            throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                              "withdrawalBroadcast without initiated state");
        }
        auto withdrawals_it = acct_it->second.withdrawals().find(w_broadcast->request);
        if (withdrawals_it == acct_it->second.withdrawals().end() ||
            !std::holds_alternative<WithdrawalInitiatedState>(withdrawals_it->second)) {
            throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                              "withdrawalBroadcast without initiated state");
        }
        return;
    }

    if (auto* w_reverted = std::get_if<WithdrawalReverted>(&entry); w_reverted != nullptr) {
        auto acct_it = accounts_.find(w_reverted->account);
        if (acct_it == accounts_.end()) {
            throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                              "withdrawalReverted without initiated state");
        }
        auto withdrawals_it = acct_it->second.withdrawals().find(w_reverted->request);
        if (withdrawals_it == acct_it->second.withdrawals().end() ||
            !std::holds_alternative<WithdrawalInitiatedState>(withdrawals_it->second)) {
            throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                              "withdrawalReverted without initiated state");
        }
        return;
    }

    if (auto* transfer = std::get_if<InternalTransfer>(&entry); transfer != nullptr) {
        if (transfer->from.raw.empty() || transfer->to.raw.empty()) {
            throw LedgerError(LedgerError::Kind::TRANSFER_INVALID,
                              "internalTransfer with empty account id");
        }
        if (transfer->from == transfer->to) {
            throw LedgerError(LedgerError::Kind::TRANSFER_INVALID,
                              "internalTransfer from an account to itself");
        }
        if (transfer->amount == 0) {
            throw LedgerError(LedgerError::Kind::TRANSFER_INVALID,
                              "internalTransfer of zero una");
        }
        // Settled funds only. `transferable()` excludes `pending`, so a
        // credit that a reorg may still revert cannot be moved out from
        // under the CompensatingDebit that would land on `from`.
        UnaAmount available = accountOr(transfer->from).transferable();
        if (transfer->amount > available) {
            std::ostringstream oss;
            oss << "insufficient transferable balance: " << transfer->amount << " > " << available;
            throw LedgerError(LedgerError::Kind::INSUFFICIENT_TRANSFERABLE_BALANCE, oss.str());
        }
        // Caps are deliberately NOT consulted: they bound outstanding
        // (open) credits, and a transfer moves settled funds only, so
        // totalOpenCredits_ / openCreditsByAccount_ are unchanged.
        return;
    }

    // depositObserved, withdrawalInitiated, policyAdjustment have no
    // pre-conditions beyond what the type already enforces.
}

void Ledger::applyToAccounts(const LedgerEntry& entry) {
    if (auto* observed = std::get_if<DepositObserved>(&entry); observed != nullptr) {
        ensureAccount(observed->account).applyDepositObserved(observed->deposit, observed->amount);
        return;
    }

    if (auto* opened = std::get_if<CreditOpened>(&entry); opened != nullptr) {
        ensureAccount(opened->account).applyCreditOpened(opened->deposit, opened->amount);
        openCreditsByAccount_[opened->account] += opened->amount;
        totalOpenCredits_ += opened->amount;
        return;
    }

    if (auto* settled = std::get_if<CreditSettled>(&entry); settled != nullptr) {
        UnaAmount amount = 0;
        auto acct_it = accounts_.find(settled->account);
        if (acct_it != accounts_.end()) {
            auto deposits_it = acct_it->second.deposits().find(settled->deposit);
            if (deposits_it != acct_it->second.deposits().end()) {
                if (auto* credited = std::get_if<DepositCreditedState>(&deposits_it->second);
                    credited != nullptr) {
                    amount = credited->amount;
                }
            }
        }
        ensureAccount(settled->account).applyCreditSettled(settled->deposit);
        UnaAmount existing = openCreditsByAccount_.count(settled->account) != 0U
                                 ? openCreditsByAccount_.at(settled->account)
                                 : 0;
        openCreditsByAccount_[settled->account] = existing >= amount ? existing - amount : 0;
        totalOpenCredits_ = totalOpenCredits_ >= amount ? totalOpenCredits_ - amount : 0;
        return;
    }

    if (auto* reverted = std::get_if<CreditReverted>(&entry); reverted != nullptr) {
        UnaAmount amount = 0;
        auto acct_it = accounts_.find(reverted->account);
        if (acct_it != accounts_.end()) {
            auto deposits_it = acct_it->second.deposits().find(reverted->deposit);
            if (deposits_it != acct_it->second.deposits().end()) {
                if (auto* credited = std::get_if<DepositCreditedState>(&deposits_it->second);
                    credited != nullptr) {
                    amount = credited->amount;
                } else if (auto* settled_state = std::get_if<DepositSettledState>(&deposits_it->second);
                           settled_state != nullptr) {
                    amount = settled_state->amount;
                }
            }
        }
        ensureAccount(reverted->account).applyCreditReverted(reverted->deposit);
        if (amount > 0) {
            UnaAmount existing = openCreditsByAccount_.count(reverted->account) != 0U
                                     ? openCreditsByAccount_.at(reverted->account)
                                     : 0;
            openCreditsByAccount_[reverted->account] = existing >= amount ? existing - amount : 0;
            totalOpenCredits_ = totalOpenCredits_ >= amount ? totalOpenCredits_ - amount : 0;
        }
        return;
    }

    if (auto* w_initiated = std::get_if<WithdrawalInitiated>(&entry); w_initiated != nullptr) {
        ensureAccount(w_initiated->account)
            .applyWithdrawalInitiated(w_initiated->request, w_initiated->amount, w_initiated->backend);
        return;
    }

    if (auto* w_settled = std::get_if<WithdrawalSettled>(&entry); w_settled != nullptr) {
        ensureAccount(w_settled->account).applyWithdrawalSettled(w_settled->request);
        return;
    }

    if (std::holds_alternative<WithdrawalBroadcastRecorded>(entry)) {
        // Audit-only: the reservation was taken by WithdrawalInitiated, so
        // binding the txid moves no balance.
        return;
    }

    if (auto* w_reverted = std::get_if<WithdrawalReverted>(&entry); w_reverted != nullptr) {
        ensureAccount(w_reverted->account).applyWithdrawalReverted(w_reverted->request);
        return;
    }

    if (auto* compensating = std::get_if<CompensatingDebit>(&entry); compensating != nullptr) {
        UnaAmount loss_before = 0;
        auto before_it = accounts_.find(compensating->account);
        if (before_it != accounts_.end()) {
            loss_before = before_it->second.operatorLoss();
        }
        ensureAccount(compensating->account)
            .applyCompensatingDebit(compensating->deposit, compensating->amount, compensating->operatorLoss);
        UnaAmount loss_after = accounts_.at(compensating->account).operatorLoss();
        totalOperatorLoss_ += (loss_after - loss_before);
        return;
    }

    if (auto* transfer = std::get_if<InternalTransfer>(&entry); transfer != nullptr) {
        // Two ensureAccount() calls, used strictly one after the other:
        // the second may rehash and invalidate the first reference.
        ensureAccount(transfer->from).applyInternalTransferOut(transfer->amount);
        ensureAccount(transfer->to).applyInternalTransferIn(transfer->amount);
        return;
    }

    if (auto* policy = std::get_if<PolicyAdjustment>(&entry); policy != nullptr) {
        if (policy->account.has_value()) {
            ensureAccount(policy->account.value()).applyPolicyAdjustment(policy->deltaUserBalance);
        }
        return;
    }
}

}  // namespace dinero::vault
