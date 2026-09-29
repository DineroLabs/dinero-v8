// Copyright (c) 2026 Dinero Labs.
//
// Daemon-side port of `Core/Vault/ReorgWatcher.swift`.

#include "vault/reorg_watcher.h"

#include "vault/ledger.h"
#include "vault/ledger_account.h"
#include "vault/vault_types.h"

#include <utility>
#include <vector>

namespace dinero::vault {

void ReorgWatcher::recordObservation(const OutpointId& outpoint,
                                     const std::array<uint8_t, 32>& block_hash) {
    if (deposit_block_hashes_.find(outpoint) == deposit_block_hashes_.end()) {
        deposit_block_hashes_[outpoint] = block_hash;
    }
}

int ReorgWatcher::tipChanged(uint64_t /*tip_height*/) {
    int reverts = 0;
    std::vector<std::pair<OutpointId, TrackedDeposit>> candidates;
    for (const auto& [k, v] : machine_->tracked()) {
        if (v.stage == DepositStage::CREDITED || v.stage == DepositStage::SETTLED) {
            candidates.emplace_back(k, v);
        }
    }
    for (const auto& [outpoint, dep] : candidates) {
        const auto inclusion = check(dep);
        switch (inclusion.kind) {
            case ChainInclusion::STILL_INCLUDED:
            case ChainInclusion::UNKNOWN:
                continue;
            case ChainInclusion::RE_MINED_SAME_TXID: {
                // Block at deposit's height changed but tx still
                // present (just re-mined). Update recorded hash.
                deposit_block_hashes_[outpoint] = inclusion.block_hash;
                continue;
            }
            case ChainInclusion::ORPHANED: {
                UnaAmount loss = unrecoverableLoss(dep);
                try {
                    machine_->revert(outpoint, loss);
                } catch (const DepositFlowError& e) {
                    throw ReorgError(ReorgError::Kind::DEPOSIT_FLOW, e.what());
                }
                reverts += 1;
                continue;
            }
        }
    }
    return reverts;
}

void ReorgWatcher::reconcileTracked() {
    for (const auto& [outpoint, dep] : machine_->tracked()) {
        if (dep.stage == DepositStage::REVERTED) continue;
        const auto inclusion = check(dep, true);
        if (inclusion.kind == ChainInclusion::UNKNOWN) {
            throw ReorgError(ReorgError::Kind::SOURCE_UNAVAILABLE,
                             "canonical deposit observation unavailable");
        }
        if (inclusion.kind == ChainInclusion::RE_MINED_SAME_TXID) {
            deposit_block_hashes_.at(outpoint) = inclusion.block_hash;
        } else if (inclusion.kind == ChainInclusion::ORPHANED) {
            try {
                machine_->revert(outpoint, unrecoverableLoss(dep));
            } catch (const DepositFlowError& e) {
                throw ReorgError(ReorgError::Kind::DEPOSIT_FLOW, e.what());
            }
        }
    }
}

ReorgWatcher::CheckedInclusion ReorgWatcher::check(const TrackedDeposit& dep, bool verify_recorded) {
    auto it = deposit_block_hashes_.find(dep.outpoint);
    if (it == deposit_block_hashes_.end()) {
        // Stage advanced to credited without a recorded block hash —
        // wiring is incomplete. Fail loudly rather than silently
        // treating as still-included (which would hide a real reorg).
        throw ReorgError(ReorgError::Kind::UNRECORDED_OBSERVATION, "no recorded block hash");
    }
    std::array<uint8_t, 32> recorded = it->second;
    std::array<uint8_t, 32> current = block_hash_at_height_(dep.deposit_height);
    // Sentinel: all-zero hash means "unknown" from the chain query.
    bool all_zero = true;
    for (auto byte : current) {
        if (byte != 0U) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        return {ChainInclusion::UNKNOWN,current};
    }
    if (current == recorded && !verify_recorded) {
        return {ChainInclusion::STILL_INCLUDED,current};
    }
    if (tx_included_at_(dep.outpoint, dep.deposit_height, current)) {
        return {current == recorded ? ChainInclusion::STILL_INCLUDED : ChainInclusion::RE_MINED_SAME_TXID,current};
    }
    return {ChainInclusion::ORPHANED,current};
}

UnaAmount ReorgWatcher::unrecoverableLoss(const TrackedDeposit& dep) {
    Ledger* ledger = machine_->ledger();
    if (ledger == nullptr) {
        return dep.amount;
    }
    auto it = ledger->accounts().find(dep.account);
    if (it == ledger->accounts().end()) {
        return dep.amount;
    }
    const LedgerAccount& acct = it->second;
    UnaAmount user_available = 0;
    if (dep.stage == DepositStage::CREDITED) {
        UnaAmount p_minus_dep = acct.pending() >= dep.amount ? acct.pending() - dep.amount : 0;
        user_available = p_minus_dep + acct.confirmed();
    } else if (dep.stage == DepositStage::SETTLED) {
        UnaAmount c_minus_dep = acct.confirmed() >= dep.amount ? acct.confirmed() - dep.amount : 0;
        user_available = acct.pending() + c_minus_dep;
    } else {
        user_available = acct.pending() + acct.confirmed();
    }
    return dep.amount > user_available ? dep.amount - user_available : 0;
}

}  // namespace dinero::vault
