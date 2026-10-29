// Copyright (c) 2026 Dinero Labs.
//
// Daemon-side port of `Core/Vault/Ledger.swift`. Coordinator +
// invariant enforcement; per-account state lives in LedgerAccount.

#include "vault/ledger.h"

#include <sstream>
#include <limits>
#include <type_traits>
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

    if (seq == std::numeric_limits<LedgerSeq>::max()) {
        throw LedgerError(LedgerError::Kind::SEQUENCE_EXHAUSTED,
                          "ledger sequence capacity exhausted");
    }
    validate(entry);
    // Copy the current derived state, but not the entire entry history. A
    // failed account operation or allocation cannot publish a partial append.
    Ledger prepared{caps_};
    prepared.accounts_ = accounts_;
    prepared.allocations_ = allocations_;
    prepared.allocationAccounts_ = allocationAccounts_;
    if (const auto* reserved=std::get_if<WithdrawalAllocationReserved>(&entry);
        reserved && !hasCreditAllocation(reserved->account)) {
        prepared.allocations_=captureUnambiguousCreditOrigins(reserved->account);
        prepared.allocationAccounts_.insert(reserved->account);
    }
    prepared.reversals_ = reversals_;
    prepared.openCreditsByAccount_ = openCreditsByAccount_;
    prepared.totalOpenCredits_ = totalOpenCredits_;
    prepared.totalOperatorLoss_ = totalOperatorLoss_;
    try {
        prepared.applyToAccounts(entry);
    } catch (const std::overflow_error& error) {
        throw LedgerError(LedgerError::Kind::ARITHMETIC_OVERFLOW, error.what());
    }
    static_assert(std::is_nothrow_move_constructible_v<LedgerEntry>);
    static_assert(noexcept(accounts_.swap(prepared.accounts_)));
    static_assert(noexcept(allocations_.swap(prepared.allocations_)));
    static_assert(noexcept(allocationAccounts_.swap(prepared.allocationAccounts_)));
    static_assert(noexcept(reversals_.swap(prepared.reversals_)));
    static_assert(noexcept(openCreditsByAccount_.swap(prepared.openCreditsByAccount_)));
    // vector::push_back provides the strong guarantee with this nothrow-move
    // entry type. Everything after it is nonthrowing publication.
    entries_.push_back(std::move(entry));
    accounts_.swap(prepared.accounts_);
    allocations_.swap(prepared.allocations_);
    allocationAccounts_.swap(prepared.allocationAccounts_);
    reversals_.swap(prepared.reversals_);
    openCreditsByAccount_.swap(prepared.openCreditsByAccount_);
    totalOpenCredits_ = prepared.totalOpenCredits_;
    totalOperatorLoss_ = prepared.totalOperatorLoss_;
    nextSeq_ = seq + 1;
}

void Ledger::revertCredit(const AccountId& account, const OutpointId& deposit,
                          LedgerTimestamp at) {
    if (hasCreditAllocation(account)) {
        append(CreditPositionReverted{nextSeq(),at,account,creditPositionSeq(account,deposit),deposit});
        return;
    }
    // A candidate owns both entries and all derived state until both appends
    // succeed. This copies history; it is not a bounded-memory replay scheme.
    Ledger prepared = *this;
    prepared.append(CreditReverted{prepared.nextSeq(), at, account, deposit});
    const auto& reversed = prepared.reversals_.at(account).at(deposit).values;
    if (reversed.refund > reversed.amount)
        throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                          "reversal debit exceeds its nominal credit");
    const auto amount = reversed.amount;
    const auto loss = amount - reversed.refund;
    prepared.append(CompensatingDebit{prepared.nextSeq(), at, account, deposit, amount, loss});

    static_assert(noexcept(entries_.swap(prepared.entries_)));
    static_assert(noexcept(accounts_.swap(prepared.accounts_)));
    static_assert(noexcept(allocations_.swap(prepared.allocations_)));
    static_assert(noexcept(allocationAccounts_.swap(prepared.allocationAccounts_)));
    static_assert(noexcept(reversals_.swap(prepared.reversals_)));
    static_assert(noexcept(openCreditsByAccount_.swap(prepared.openCreditsByAccount_)));
    entries_.swap(prepared.entries_);
    accounts_.swap(prepared.accounts_);
    allocations_.swap(prepared.allocations_);
    allocationAccounts_.swap(prepared.allocationAccounts_);
    reversals_.swap(prepared.reversals_);
    openCreditsByAccount_.swap(prepared.openCreditsByAccount_);
    totalOpenCredits_ = prepared.totalOpenCredits_;
    totalOperatorLoss_ = prepared.totalOperatorLoss_;
    nextSeq_ = prepared.nextSeq_;
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

std::optional<CreditReinstatement> Ledger::reinstatement(
    const AccountId& account,const OutpointId& deposit) const {
    auto a=reversals_.find(account);
    if(a==reversals_.end())return std::nullopt;
    auto d=a->second.find(deposit);
    if(d==a->second.end() || !d->second.usable || !d->second.compensated)return std::nullopt;
    return d->second.values;
}

void Ledger::validate(const LedgerEntry& entry) {
    if(const auto* restored=std::get_if<CreditReinstated>(&entry)) {
        const auto retained=reinstatement(restored->account,restored->deposit);
        auto a=accounts_.find(restored->account);
        if(!retained || a==accounts_.end() || retained->reversalSeq!=restored->reversalSeq ||
           retained->compensationSeq!=restored->compensationSeq ||
           retained->operatorLoss>totalOperatorLoss_ || retained->operatorLoss>a->second.operatorLoss())
            throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,"unbound credit reinstatement");
        auto d=a->second.deposits().find(restored->deposit);
        if(d==a->second.deposits().end() || !std::holds_alternative<DepositRevertedState>(d->second) ||
           std::get<DepositRevertedState>(d->second).amount!=retained->amount)
            throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,"credit reinstatement without reverted owner");
        return;
    }
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
        if (per_account > caps_.per_user || opened->amount > caps_.per_user - per_account) {
            std::ostringstream oss;
            oss << "per-user cap exceeded: current " << per_account << ", addition "
                << opened->amount << ", cap " << caps_.per_user;
            throw LedgerError(LedgerError::Kind::PER_USER_CAP_EXCEEDED, oss.str());
        }
        if (totalOpenCredits_ > caps_.global || opened->amount > caps_.global - totalOpenCredits_) {
            std::ostringstream oss;
            oss << "global cap exceeded: current " << totalOpenCredits_ << ", addition "
                << opened->amount << ", cap " << caps_.global;
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

    // depositObserved, withdrawalInitiated, policyAdjustment have no
    // pre-conditions beyond what the type already enforces.
}

void Ledger::applyToAccounts(const LedgerEntry& entry) {
    if (applyAllocationEntry(entry)) return;
    const auto owner=entryAccount(entry);
    if (owner && hasCreditAllocation(*owner) &&
        !std::holds_alternative<DepositObserved>(entry) && !std::holds_alternative<CreditOpened>(entry))
        throw LedgerError(LedgerError::Kind::LIFECYCLE_INCONSISTENT,
                          "attributed account requires attributed monetary transitions");
    if(const auto* restored=std::get_if<CreditReinstated>(&entry)) {
        const auto retained=*reinstatement(restored->account,restored->deposit);
        accounts_.at(restored->account).applyCreditReinstated(restored->deposit,retained.amount,
                                                           retained.refund,retained.operatorLoss);
        totalOperatorLoss_-=retained.operatorLoss;
        reversals_.at(restored->account).erase(restored->deposit);
        // The service requires full settlement maturity. Reinstatement does
        // not reopen an advance or consume/release another deposit's cap.
        return;
    }
    if (auto* observed = std::get_if<DepositObserved>(&entry); observed != nullptr) {
        ensureAccount(observed->account).applyDepositObserved(observed->deposit, observed->amount);
        return;
    }

    if (auto* opened = std::get_if<CreditOpened>(&entry); opened != nullptr) {
        ensureAccount(opened->account).applyCreditOpened(opened->deposit, opened->amount);
        openCreditsByAccount_[opened->account] += opened->amount;
        totalOpenCredits_ += opened->amount;
        if (hasCreditAllocation(opened->account)) {
            allocations_.open(opened->seq,opened->account,opened->deposit,opened->amount);
            syncAllocatedAccount(opened->account);
        }
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
                // Settled deposits left the open-credit counters at settlement.
                // Reverting one must not release another deposit's capacity.
                if (auto* credited = std::get_if<DepositCreditedState>(&deposits_it->second);
                    credited != nullptr) {
                    amount = credited->amount;
                }
            }
        }
        auto& account=ensureAccount(reverted->account);
        const auto before=account.pending()+account.confirmed();
        const auto nominal=std::visit([](const auto& value){return value.amount;},
                                      account.deposits().at(reverted->deposit));
        account.applyCreditReverted(reverted->deposit);
        const auto refund=before-(account.pending()+account.confirmed());
        reversals_[reverted->account][reverted->deposit]=
            Reversal{CreditReinstatement{reverted->seq,0,nominal,refund,0},false,true};
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
        const UnaAmount increase = loss_after - loss_before;
        if (increase > std::numeric_limits<UnaAmount>::max() - totalOperatorLoss_)
            throw std::overflow_error("vault total operator loss overflow");
        totalOperatorLoss_ += increase;
        auto& retained=reversals_.at(compensating->account).at(compensating->deposit);
        if(retained.compensated || compensating->amount!=retained.values.amount ||
           compensating->operatorLoss>compensating->amount)retained.usable=false;
        if(!retained.compensated) {
            retained.values.compensationSeq=compensating->seq;
            retained.values.operatorLoss=increase;
            retained.compensated=true;
        }
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
