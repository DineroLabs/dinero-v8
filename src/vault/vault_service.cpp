// Copyright (c) 2026 Dinero Labs.
//
// Top-level vault orchestrator. Owns Ledger + 3 state machines +
// SigningBackend. Single-actor through a mutex.

#include "vault/vault_service.h"

#include "vault/deposit_flow.h"
#include "vault/ledger_account.h"
#include "vault/ledger_entry.h"
#include "vault/reorg_watcher.h"
#include "vault/signing_backend.h"
#include "vault/withdrawal_queue.h"

#include <utility>

namespace dinero::vault {

VaultService::VaultService(std::unique_ptr<SigningBackend> backend, VaultServiceConfig config,
                           BlockHashAtHeightFn block_hash_at_height, TxIncludedAtFn tx_included_at)
    : backend_{std::move(backend)},
      ledger_{config.ledger_caps},
      deposit_flow_{&ledger_, std::move(config.confirmation_policy), config.shadow_mode},
      reorg_watcher_{&deposit_flow_, std::move(block_hash_at_height), std::move(tx_included_at)},
      withdrawals_{&ledger_, backend_.get(), config.withdrawal_caps, config.withdrawal_policy} {}

void VaultService::recordDeposit(const std::array<uint8_t, 32>& txid, uint32_t vout,
                                 const AccountId& account, UnaAmount amount, uint64_t height,
                                 const std::array<uint8_t, 32>& block_hash) {
    std::lock_guard<std::mutex> lock(mu_);
    OutpointId op;
    op.txid_raw = txid;
    op.vout = vout;
    deposit_flow_.observe(op, account, amount, height);
    reorg_watcher_.recordObservation(op, block_hash);
}

void VaultService::tipChanged(uint64_t height) {
    std::lock_guard<std::mutex> lock(mu_);
    // Phase 1: deposit-flow advancement. Cap pressure / lifecycle
    // errors are wrapped in DepositFlowError; we let those propagate
    // so the caller sees them, but log + continue with phase 2.
    try {
        deposit_flow_.tipChanged(height);
    } catch (const DepositFlowError&) {
        // Continue; per-deposit errors don't poison phases 2-3.
    }
    // Phase 2: reorg detection. UNRECORDED_OBSERVATION means the
    // wiring is broken; let it propagate.
    reorg_watcher_.tipChanged(height);
    // Phase 3: withdrawal settlement.
    try {
        withdrawals_.tipChanged(height);
    } catch (const WithdrawalQueueError&) {
        // Continue.
    }
}

WithdrawalId VaultService::enqueueWithdrawal(const AccountId& account, UnaAmount amount,
                                             const std::vector<uint8_t>& destination_script_pub_key) {
    std::lock_guard<std::mutex> lock(mu_);
    return withdrawals_.enqueue(account, amount, destination_script_pub_key);
}

std::optional<WithdrawalId> VaultService::processNextWithdrawal() {
    std::lock_guard<std::mutex> lock(mu_);
    return withdrawals_.processNext();
}

void VaultService::markWithdrawalIncluded(const WithdrawalId& id, uint64_t height) {
    std::lock_guard<std::mutex> lock(mu_);
    withdrawals_.markBroadcastIncluded(id, height);
}

UnaAmount VaultService::accountSpendable(const AccountId& account) {
    std::lock_guard<std::mutex> lock(mu_);
    return ledger_.accountOr(account).spendable();
}

UnaAmount VaultService::accountConfirmed(const AccountId& account) {
    std::lock_guard<std::mutex> lock(mu_);
    return ledger_.accountOr(account).confirmed();
}

UnaAmount VaultService::accountPending(const AccountId& account) {
    std::lock_guard<std::mutex> lock(mu_);
    return ledger_.accountOr(account).pending();
}

UnaAmount VaultService::accountLocked(const AccountId& account) {
    std::lock_guard<std::mutex> lock(mu_);
    return ledger_.accountOr(account).locked();
}

UnaAmount VaultService::accountOperatorLoss(const AccountId& account) {
    std::lock_guard<std::mutex> lock(mu_);
    return ledger_.accountOr(account).operatorLoss();
}

UnaAmount VaultService::totalOpenCredits() {
    std::lock_guard<std::mutex> lock(mu_);
    return ledger_.totalOpenCredits();
}

UnaAmount VaultService::totalOperatorLoss() {
    std::lock_guard<std::mutex> lock(mu_);
    return ledger_.totalOperatorLoss();
}

uint64_t VaultService::ledgerNextSeq() {
    std::lock_guard<std::mutex> lock(mu_);
    return ledger_.nextSeq();
}

size_t VaultService::accountCount() {
    std::lock_guard<std::mutex> lock(mu_);
    return ledger_.accounts().size();
}

int VaultService::withdrawalQueueDepth() {
    std::lock_guard<std::mutex> lock(mu_);
    return withdrawals_.outstandingDepth();
}

std::vector<LedgerEntry> VaultService::entriesSince(LedgerSeq since, size_t limit) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<LedgerEntry> out;
    const auto& all = ledger_.entries();
    for (const auto& entry : all) {
        if (entrySeq(entry) < since) {
            continue;
        }
        out.push_back(entry);
        if (out.size() >= limit) {
            break;
        }
    }
    return out;
}

WithdrawalState VaultService::withdrawalState(const WithdrawalId& id) {
    std::lock_guard<std::mutex> lock(mu_);
    return withdrawals_.state(id);
}

HealthReport VaultService::backendHealth() {
    std::lock_guard<std::mutex> lock(mu_);
    return backend_->healthcheck();
}

}  // namespace dinero::vault
