// Copyright (c) 2026 Dinero Labs.
//
// Daemon-side port of `Core/Vault/WithdrawalQueue.swift`.

#include "vault/withdrawal_queue.h"

#include "vault/ledger.h"
#include "vault/ledger_account.h"
#include "vault/ledger_entry.h"
#include "vault/signing_backend.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <utility>
#include <vector>

namespace dinero::vault {

LedgerTimestamp WithdrawalQueue::now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool WithdrawalQueue::isOutstanding(const WithdrawalState& s) const {
    return std::holds_alternative<WithdrawalPending>(s) ||
           std::holds_alternative<WithdrawalSigning>(s) ||
           std::holds_alternative<WithdrawalBroadcast>(s);
}

WithdrawalId WithdrawalQueue::enqueue(const AccountId& account, UnaAmount amount,
                                      const std::vector<uint8_t>& destination_script_pub_key) {
    if (amount == 0) {
        throw WithdrawalQueueError(WithdrawalQueueError::Kind::ZERO_AMOUNT, "amount must be > 0");
    }
    if (amount > caps_.per_request) {
        throw WithdrawalQueueError(WithdrawalQueueError::Kind::PER_REQUEST_CAP_EXCEEDED,
                                   "amount exceeds per-request cap");
    }
    if (!destination_validator_(destination_script_pub_key)) {
        throw WithdrawalQueueError(WithdrawalQueueError::Kind::DESTINATION_REJECTED,
                                   "destination rejected by validator");
    }
    UnaAmount spendable = 0;
    auto acct_it = ledger_->accounts().find(account);
    if (acct_it != ledger_->accounts().end()) {
        spendable = acct_it->second.spendable();
    }
    if (spendable < amount) {
        throw WithdrawalQueueError(WithdrawalQueueError::Kind::INSUFFICIENT_SPENDABLE,
                                   "insufficient spendable");
    }
    UnaAmount outstanding = currentOutstanding(account);
    if (outstanding + amount > caps_.per_account_outstanding) {
        throw WithdrawalQueueError(WithdrawalQueueError::Kind::PER_ACCOUNT_OUTSTANDING_EXCEEDED,
                                   "per-account outstanding cap exceeded");
    }
    int depth = outstandingDepth();
    if (depth >= caps_.global_queue_depth) {
        throw WithdrawalQueueError(WithdrawalQueueError::Kind::GLOBAL_QUEUE_FULL, "global queue full");
    }
    WithdrawalRequest req;
    req.request_id = request_id_generator_();
    req.account = account;
    req.amount = amount;
    req.destination_script_pub_key = destination_script_pub_key;
    req.created_at = now();
    requests_[req.request_id] = req;
    states_[req.request_id] = WithdrawalPending{};
    return req.request_id;
}

std::optional<WithdrawalId> WithdrawalQueue::processNext() {
    // Pick oldest pending.
    const WithdrawalRequest* oldest = nullptr;
    for (const auto& [id, req] : requests_) {
        auto state_it = states_.find(id);
        if (state_it == states_.end() || !std::holds_alternative<WithdrawalPending>(state_it->second)) {
            continue;
        }
        if (oldest == nullptr || req.created_at < oldest->created_at) {
            oldest = &req;
        }
    }
    if (oldest == nullptr) {
        return std::nullopt;
    }

    WithdrawalId id = oldest->request_id;
    states_[id] = WithdrawalSigning{};

    UnsignedTx unsigned_tx;
    unsigned_tx.request_id = id;
    SignOutput out;
    out.value = oldest->amount;
    out.script_pub_key = oldest->destination_script_pub_key;
    unsigned_tx.outputs.push_back(out);

    std::array<uint8_t, 32> txid{};
    try {
        txid = backend_->signAndBroadcast(unsigned_tx);
    } catch (const SigningBackendError& e) {
        states_[id] = WithdrawalFailed{e.what()};
        throw WithdrawalQueueError(WithdrawalQueueError::Kind::BACKEND_ERROR, e.what());
    }

    OutpointId outpoint;
    outpoint.txid_raw = txid;
    outpoint.vout = 0;
    try {
        ledger_->append(WithdrawalInitiated{ledger_->nextSeq(), now(), oldest->account, outpoint,
                                            oldest->amount, backend_->backendId()});
    } catch (const LedgerError& e) {
        states_[id] = WithdrawalFailed{std::string("ledger: ") + e.what()};
        throw WithdrawalQueueError(WithdrawalQueueError::Kind::LEDGER_ERROR, e.what());
    }
    WithdrawalBroadcast bc;
    bc.txid = txid;
    bc.included_at_height = 0;
    states_[id] = bc;
    return id;
}

void WithdrawalQueue::markBroadcastIncluded(const WithdrawalId& id, uint64_t height) {
    auto state_it = states_.find(id);
    if (state_it == states_.end()) {
        throw WithdrawalQueueError(WithdrawalQueueError::Kind::UNKNOWN_REQUEST, "unknown");
    }
    auto* bc = std::get_if<WithdrawalBroadcast>(&state_it->second);
    if (bc == nullptr) {
        throw WithdrawalQueueError(WithdrawalQueueError::Kind::LIFECYCLE_VIOLATION,
                                   "not in broadcast state");
    }
    bc->included_at_height = height;
}

int WithdrawalQueue::tipChanged(uint64_t tip_height) {
    int settled = 0;
    std::vector<WithdrawalId> ids;
    ids.reserve(states_.size());
    for (const auto& [id, st] : states_) {
        ids.push_back(id);
    }
    for (const auto& id : ids) {
        auto state_it = states_.find(id);
        if (state_it == states_.end()) {
            continue;
        }
        auto* bc = std::get_if<WithdrawalBroadcast>(&state_it->second);
        if (bc == nullptr || bc->included_at_height == 0) {
            continue;
        }
        if (tip_height < bc->included_at_height) {
            continue;
        }
        uint64_t confs = tip_height - bc->included_at_height + 1;
        if (confs < policy_.k_settle) {
            continue;
        }
        auto req_it = requests_.find(id);
        if (req_it == requests_.end()) {
            continue;
        }
        OutpointId outpoint;
        outpoint.txid_raw = bc->txid;
        outpoint.vout = 0;
        try {
            ledger_->append(WithdrawalSettled{ledger_->nextSeq(), now(), req_it->second.account, outpoint});
            std::array<uint8_t, 32> txid_copy = bc->txid;
            states_[id] = WithdrawalSettledOnChain{txid_copy};
            settled += 1;
        } catch (const LedgerError& e) {
            throw WithdrawalQueueError(WithdrawalQueueError::Kind::LEDGER_ERROR, e.what());
        }
    }
    return settled;
}

void WithdrawalQueue::revert(const WithdrawalId& id) {
    auto state_it = states_.find(id);
    if (state_it == states_.end()) {
        throw WithdrawalQueueError(WithdrawalQueueError::Kind::UNKNOWN_REQUEST, "unknown");
    }
    auto req_it = requests_.find(id);
    if (req_it == requests_.end()) {
        throw WithdrawalQueueError(WithdrawalQueueError::Kind::UNKNOWN_REQUEST, "unknown");
    }
    std::array<uint8_t, 32> txid{};
    bool has_onchain = false;
    if (auto* bc = std::get_if<WithdrawalBroadcast>(&state_it->second); bc != nullptr) {
        txid = bc->txid;
        has_onchain = true;
    } else if (auto* settled = std::get_if<WithdrawalSettledOnChain>(&state_it->second); settled != nullptr) {
        txid = settled->txid;
        has_onchain = true;
    }
    if (has_onchain) {
        OutpointId outpoint;
        outpoint.txid_raw = txid;
        outpoint.vout = 0;
        try {
            ledger_->append(WithdrawalReverted{ledger_->nextSeq(), now(), req_it->second.account, outpoint});
            states_[id] = WithdrawalRevertedOnChain{txid};
        } catch (const LedgerError& e) {
            throw WithdrawalQueueError(WithdrawalQueueError::Kind::LEDGER_ERROR, e.what());
        }
        return;
    }
    // pending / signing / failed / reverted: no on-chain footprint to revert.
    states_[id] = WithdrawalRevertedOnChain{std::array<uint8_t, 32>{}};
}

WithdrawalState WithdrawalQueue::state(const WithdrawalId& id) const {
    auto it = states_.find(id);
    if (it == states_.end()) {
        return WithdrawalFailed{"unknown"};
    }
    return it->second;
}

int WithdrawalQueue::outstandingDepth() const {
    int n = 0;
    for (const auto& [id, st] : states_) {
        if (isOutstanding(st)) {
            n += 1;
        }
    }
    return n;
}

UnaAmount WithdrawalQueue::currentOutstanding(const AccountId& account) const {
    UnaAmount total = 0;
    for (const auto& [id, st] : states_) {
        if (!isOutstanding(st)) {
            continue;
        }
        auto req_it = requests_.find(id);
        if (req_it == requests_.end() || req_it->second.account != account) {
            continue;
        }
        total += req_it->second.amount;
    }
    return total;
}

}  // namespace dinero::vault
