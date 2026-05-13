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
        ChainInclusion inclusion = check(dep);
        switch (inclusion) {
            case ChainInclusion::STILL_INCLUDED:
            case ChainInclusion::UNKNOWN:
                continue;
            case ChainInclusion::RE_MINED_SAME_TXID: {
                // Block at deposit's height changed but tx still
                // present (just re-mined). Update recorded hash.
                std::array<uint8_t, 32> new_hash = block_hash_at_height_(dep.deposit_height);
                deposit_block_hashes_[outpoint] = new_hash;
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

ChainInclusion ReorgWatcher::check(const TrackedDeposit& dep) {
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
        return ChainInclusion::UNKNOWN;
    }
    if (current == recorded) {
        return ChainInclusion::STILL_INCLUDED;
    }
    if (tx_included_at_(dep.outpoint, dep.deposit_height, current)) {
        return ChainInclusion::RE_MINED_SAME_TXID;
    }
    return ChainInclusion::ORPHANED;
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
