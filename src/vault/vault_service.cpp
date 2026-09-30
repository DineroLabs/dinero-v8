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
#include <type_traits>
#include <stdexcept>
#include <limits>
#include <algorithm>

namespace dinero::vault {

// These copies own only candidate in-memory state. They neither persist nor
// invoke signing. Every internal pointer is rebound before candidate mutation.
struct VaultService::PreparedState {
    Ledger ledger;
    DepositFlowMachine deposits;
    ReorgWatcher watcher;
    WithdrawalQueue withdrawals;

    explicit PreparedState(VaultService& owner)
        : ledger(owner.ledger_), deposits(owner.deposit_flow_),
          watcher(&deposits,
              [&owner](uint64_t h) { return owner.reorg_watcher_.block_hash_at_height_(h); },
              [&owner](const OutpointId& op, uint64_t h, const std::array<uint8_t, 32>& hash) {
                  return owner.reorg_watcher_.tx_included_at_(op, h, hash);
              }), withdrawals(owner.withdrawals_) {
        owner.requireRevisionCapacity();
        deposits.ledger_ = &ledger;
        watcher.deposit_block_hashes_ = owner.reorg_watcher_.deposit_block_hashes_;
        withdrawals.ledger_ = &ledger;
    }
    PreparedState(const PreparedState&) = delete;
    PreparedState& operator=(const PreparedState&) = delete;
};

void VaultService::publish(PreparedState& state) noexcept {
    static_assert(std::is_nothrow_swappable_v<Ledger>);
    static_assert(noexcept(deposit_flow_.tracked_.swap(state.deposits.tracked_)));
    static_assert(noexcept(reorg_watcher_.deposit_block_hashes_.swap(state.watcher.deposit_block_hashes_)));
    static_assert(noexcept(withdrawals_.requests_.swap(state.withdrawals.requests_)));
    static_assert(noexcept(withdrawals_.states_.swap(state.withdrawals.states_)));
    // Swap values only: the live machines retain pointers to this service's
    // ledger/deposit members, while candidate pointers never escape.
    using std::swap;
    swap(ledger_, state.ledger);
    deposit_flow_.tracked_.swap(state.deposits.tracked_);
    reorg_watcher_.deposit_block_hashes_.swap(state.watcher.deposit_block_hashes_);
    withdrawals_.requests_.swap(state.withdrawals.requests_);
    withdrawals_.states_.swap(state.withdrawals.states_);
    ++revision_;
}

void VaultService::requireRevisionCapacity() const {
    if (revision_ == std::numeric_limits<uint64_t>::max())
        throw std::runtime_error("vault state revision exhausted");
}

VaultService::VaultService(std::unique_ptr<SigningBackend> backend, VaultServiceConfig config,
                           BlockHashAtHeightFn block_hash_at_height, TxIncludedAtFn tx_included_at,
                           VaultTipSnapshotFn capture_tip)
    : capture_tip_(std::move(capture_tip)), backend_{std::move(backend)},
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
    const auto existing = deposit_flow_.tracked().find(op);
    if (existing != deposit_flow_.tracked().end()) {
        const auto hash = reorg_watcher_.depositBlockHashes().find(op);
        const auto& prior = existing->second;
        if (prior.account != account || prior.amount != amount || prior.deposit_height != height ||
            hash == reorg_watcher_.depositBlockHashes().end() || hash->second != block_hash) {
            throw std::runtime_error("deposit observation conflicts with its existing owner");
        }
        return;
    }
    PreparedState state(*this);
    state.deposits.observe(op, account, amount, height);
    state.watcher.recordObservation(op, block_hash);
    publish(state);
}

void VaultService::tipChanged(uint64_t height) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!capture_tip_) {
        PreparedState state(*this);
        state.watcher.reconcileTracked();
        state.deposits.tipChanged(height);
        state.withdrawals.tipChanged(height);
        publish(state);
        return;
    }
    const auto captured_revision = revision_;
    std::vector<VaultDepositQuery> queries;
    queries.reserve(deposit_flow_.tracked().size());
    for (const auto& [outpoint, dep] : deposit_flow_.tracked()) {
        if (dep.stage != DepositStage::REVERTED)
            queries.push_back({outpoint, dep.deposit_height, dep.amount});
    }
    // Never acquire selected-chain ownership while holding the vault mutex.
    // The reader is immutable after construction and this method's caller
    // retains the service throughout this operation.
    lock.unlock();
    const auto captured = capture_tip_(height, queries);
    lock.lock();
    if (revision_ != captured_revision)
        throw std::runtime_error("vault state changed during canonical observation");
    const auto nonzero = [](const auto& hash) {
        return std::any_of(hash.begin(), hash.end(), [](uint8_t b) { return b != 0; });
    };
    if (captured.height != height || !nonzero(captured.block_hash) ||
        captured.deposits.size() != queries.size())
        throw std::runtime_error("incomplete vault canonical snapshot");
    // Validate every result before staging. Ordered exact input binding also
    // refuses missing, duplicate, foreign or reordered observation results.
    for (size_t i=0; i<queries.size(); ++i) {
        const auto& item = captured.deposits[i];
        if (item.query != queries[i] ||
            (item.block_hash && !nonzero(*item.block_hash)) ||
            (item.query.height <= height && !item.block_hash) ||
            (item.query.height > height && (item.block_hash || item.included)))
            throw std::runtime_error("vault canonical observation binding mismatch");
    }
    PreparedState state(*this);
    for (const auto& item : captured.deposits) {
        const auto& dep = state.deposits.tracked_.at(item.query.outpoint);
        auto& recorded = state.watcher.deposit_block_hashes_.at(item.query.outpoint);
        if (item.included) {
            recorded = *item.block_hash;
        } else {
            state.deposits.revert(item.query.outpoint, state.watcher.unrecoverableLoss(dep));
        }
    }
    state.deposits.tipChanged(captured.height);
    state.withdrawals.tipChanged(captured.height);
    publish(state);
}

WithdrawalId VaultService::enqueueWithdrawal(const AccountId& account, UnaAmount amount,
                                             const std::vector<uint8_t>& destination_script_pub_key) {
    std::lock_guard<std::mutex> lock(mu_);
    PreparedState state(*this);
    const auto id = state.withdrawals.enqueue(account, amount, destination_script_pub_key);
    publish(state);
    return id;
}

std::optional<WithdrawalId> VaultService::processNextWithdrawal() {
    std::lock_guard<std::mutex> lock(mu_);
    // This existing externally-effectful path may mutate before throwing.
    // Invalidate captured observations before entering it in either case.
    requireRevisionCapacity();
    ++revision_;
    return withdrawals_.processNext();
}

void VaultService::markWithdrawalIncluded(const WithdrawalId& id, uint64_t height) {
    std::lock_guard<std::mutex> lock(mu_);
    PreparedState state(*this);
    state.withdrawals.markBroadcastIncluded(id, height);
    publish(state);
}

VaultAccountMetrics VaultService::accountMetrics(const AccountId& account) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto found = ledger_.accounts().find(account);
    if (found == ledger_.accounts().end()) return {};
    const auto& state = found->second;
    return {state.spendable(), state.confirmed(), state.pending(), state.locked(), state.operatorLoss()};
}

VaultMetrics VaultService::metrics() {
    std::lock_guard<std::mutex> lock(mu_);
    return {ledger_.totalOpenCredits(), ledger_.totalOperatorLoss(), ledger_.nextSeq(),
            ledger_.accounts().size(), withdrawals_.outstandingDepth()};
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
