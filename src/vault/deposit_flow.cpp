// Copyright (c) 2026 Dinero Labs.
//
// Daemon-side port of `Core/Vault/DepositFlowMachine.swift`.

#include "vault/deposit_flow.h"

#include "vault/ledger.h"
#include "vault/ledger_entry.h"

#include <chrono>
#include <utility>

namespace dinero::vault {

LedgerTimestamp DepositFlowMachine::now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void DepositFlowMachine::observe(const OutpointId& outpoint, const AccountId& account,
                                 UnaAmount amount, uint64_t height) {
    if (tracked_.find(outpoint) != tracked_.end()) {
        return;  // idempotent
    }
    tracked_[outpoint] = TrackedDeposit{outpoint, account, amount, height, DepositStage::DETECTED};
}

int DepositFlowMachine::tipChanged(uint64_t tip_height) {
    int transitions = 0;
    // Snapshot keys so we can mutate `tracked_` mid-iteration.
    std::vector<OutpointId> keys;
    keys.reserve(tracked_.size());
    for (const auto& [k, v] : tracked_) {
        keys.push_back(k);
    }
    for (const auto& key : keys) {
        auto it = tracked_.find(key);
        if (it == tracked_.end()) {
            continue;
        }
        TrackedDeposit& dep = it->second;
        if (tip_height < dep.deposit_height) {
            continue;
        }
        uint64_t confs = tip_height - dep.deposit_height + 1;
        transitions += advance(dep, confs);
    }
    return transitions;
}

int DepositFlowMachine::advance(TrackedDeposit& dep, uint64_t confs) {
    int transitions = 0;
    uint64_t k_credit = policy_.effectiveKCredit(dep.amount);

    if (dep.stage == DepositStage::DETECTED && confs >= policy_.k_observe) {
        try {
            ledger_->append(DepositObserved{ledger_->nextSeq(), now(), dep.account, dep.outpoint, dep.amount});
        } catch (const LedgerError& e) {
            throw DepositFlowError(DepositFlowError::Kind::LEDGER, e.what());
        }
        dep.stage = DepositStage::OBSERVED;
        transitions += 1;
    }

    if (dep.stage == DepositStage::OBSERVED && confs >= k_credit && !shadow_mode_) {
        try {
            ledger_->append(CreditOpened{ledger_->nextSeq(), now(), dep.account, dep.outpoint, dep.amount});
            dep.stage = DepositStage::CREDITED;
            transitions += 1;
        } catch (const LedgerError& e) {
            // Cap pressure or lifecycle violation. Caller decides
            // whether to retry next tip (e.g. waits for an earlier
            // credit to settle freeing per-user cap).
            throw DepositFlowError(DepositFlowError::Kind::LEDGER, e.what());
        }
    }

    if (dep.stage == DepositStage::CREDITED && confs >= policy_.k_settle) {
        try {
            if(ledger_->hasCreditAllocation(dep.account))
                ledger_->append(CreditPositionMatured{ledger_->nextSeq(),now(),dep.account,
                    ledger_->creditPositionSeq(dep.account,dep.outpoint),dep.outpoint});
            else ledger_->append(CreditSettled{ledger_->nextSeq(), now(), dep.account, dep.outpoint});
        } catch (const LedgerError& e) {
            throw DepositFlowError(DepositFlowError::Kind::LEDGER, e.what());
        }
        dep.stage = DepositStage::SETTLED;
        transitions += 1;
    }

    return transitions;
}

void DepositFlowMachine::reinstateIncluded(const OutpointId& outpoint,uint64_t tip_height) {
    auto& dep=tracked_.at(outpoint);
    if(dep.stage!=DepositStage::REVERTED || tip_height<dep.deposit_height)return;
    const auto account=ledger_->accountOr(dep.account);
    const auto found=account.deposits().find(outpoint);
    if(found==account.deposits().end()) {
        dep.stage=DepositStage::DETECTED;return;
    }
    if(const auto* observed=std::get_if<DepositObservedState>(&found->second)) {
        if(observed->amount!=dep.amount)throw DepositFlowError(DepositFlowError::Kind::LEDGER,"observed reinstatement amount mismatch");
        dep.stage=DepositStage::OBSERVED;return;
    }
    if(ledger_->hasCreditAllocation(dep.account)) {
        if(!std::holds_alternative<DepositRevertedState>(found->second) ||
           std::get<DepositRevertedState>(found->second).amount!=dep.amount)
            throw DepositFlowError(DepositFlowError::Kind::LEDGER,"attributed reinstatement owner mismatch");
        const auto required=std::max({policy_.k_observe,policy_.effectiveKCredit(dep.amount),policy_.k_settle});
        const auto distance=tip_height-dep.deposit_height;
        if(required>0 && distance<required-1)return;
        try {
            ledger_->append(CreditPositionRestored{ledger_->nextSeq(),now(),dep.account,
                ledger_->creditPositionSeq(dep.account,outpoint),outpoint});
        } catch(const LedgerError& e) {throw DepositFlowError(DepositFlowError::Kind::LEDGER,e.what());}
        dep.stage=DepositStage::SETTLED;return;
    }
    const auto retained=ledger_->reinstatement(dep.account,outpoint);
    if(!retained || retained->amount!=dep.amount)
        throw DepositFlowError(DepositFlowError::Kind::LEDGER,"incomplete credit reversal history");
    const auto required=std::max({policy_.k_observe,policy_.effectiveKCredit(dep.amount),policy_.k_settle});
    // Avoid tip-height + 1 overflow, including for a zero-confirmation policy.
    const auto distance=tip_height-dep.deposit_height;
    if(required>0 && distance<required-1)return;
    try {
        ledger_->append(CreditReinstated{ledger_->nextSeq(),now(),dep.account,outpoint,
                                        retained->reversalSeq,retained->compensationSeq});
    } catch(const LedgerError& e) {throw DepositFlowError(DepositFlowError::Kind::LEDGER,e.what());}
    dep.stage=DepositStage::SETTLED;
}

void DepositFlowMachine::revertOwned(const OutpointId& outpoint) {
    auto it = tracked_.find(outpoint);
    if (it == tracked_.end())
        throw DepositFlowError(DepositFlowError::Kind::UNKNOWN_DEPOSIT, "unknown");
    auto& dep = it->second;
    if (dep.stage == DepositStage::REVERTED) return;
    if (dep.stage == DepositStage::DETECTED || dep.stage == DepositStage::OBSERVED) {
        dep.stage = DepositStage::REVERTED;
        return;
    }
    if (!ledger_)
        throw DepositFlowError(DepositFlowError::Kind::LEDGER, "missing reversal ledger owner");
    const auto account = ledger_->accounts().find(dep.account);
    if (account == ledger_->accounts().end())
        throw DepositFlowError(DepositFlowError::Kind::LEDGER, "missing reversal account owner");
    const auto owner = account->second.deposits().find(outpoint);
    if (owner == account->second.deposits().end() ||
        (dep.stage == DepositStage::CREDITED && !std::holds_alternative<DepositCreditedState>(owner->second)) ||
        (dep.stage == DepositStage::SETTLED && !std::holds_alternative<DepositSettledState>(owner->second)) ||
        (dep.stage != DepositStage::CREDITED && dep.stage != DepositStage::SETTLED) ||
        std::visit([](const auto& state) { return state.amount; }, owner->second) != dep.amount)
        throw DepositFlowError(DepositFlowError::Kind::LEDGER, "reversal deposit owner mismatch");
    try {
        ledger_->revertCredit(dep.account, outpoint, now());
    } catch (const LedgerError& error) {
        throw DepositFlowError(DepositFlowError::Kind::LEDGER, error.what());
    }
    dep.stage = DepositStage::REVERTED;
}

void DepositFlowMachine::revert(const OutpointId& outpoint, UnaAmount operator_loss) {
    auto it = tracked_.find(outpoint);
    if (it == tracked_.end()) {
        throw DepositFlowError(DepositFlowError::Kind::UNKNOWN_DEPOSIT, "unknown");
    }
    TrackedDeposit& dep = it->second;
    if (dep.stage == DepositStage::OBSERVED || dep.stage == DepositStage::DETECTED) {
        dep.stage = DepositStage::REVERTED;
        return;
    }
    if (dep.stage == DepositStage::CREDITED || dep.stage == DepositStage::SETTLED) {
        try {
            ledger_->append(CreditReverted{ledger_->nextSeq(), now(), dep.account, dep.outpoint});
            ledger_->append(CompensatingDebit{ledger_->nextSeq(), now(), dep.account, dep.outpoint, dep.amount, operator_loss});
            dep.stage = DepositStage::REVERTED;
        } catch (const LedgerError& e) {
            throw DepositFlowError(DepositFlowError::Kind::LEDGER, e.what());
        }
        return;
    }
    // already reverted, idempotent
}

}  // namespace dinero::vault
