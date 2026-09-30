// Copyright (c) 2026 Dinero Labs.
//
// Top-level vault orchestrator. Owns Ledger + 3 state machines +
// SigningBackend. Single-actor through a mutex.

#include "vault/vault_service.h"
#include "vault/state_snapshot.h"

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
    : operator_binding_(std::move(config.operator_binding)),
      capture_tip_(std::move(capture_tip)), backend_{std::move(backend)},
      ledger_{config.ledger_caps},
      deposit_flow_{&ledger_, std::move(config.confirmation_policy), config.shadow_mode},
      reorg_watcher_{&deposit_flow_, std::move(block_hash_at_height), std::move(tx_included_at)},
      withdrawals_{&ledger_, backend_.get(), config.withdrawal_caps, config.withdrawal_policy} {
    if (operator_binding_) ValidateVaultOperatorBinding(*operator_binding_);
}

std::optional<VaultOperatorBinding> VaultService::operatorBinding() const {
    return operator_binding_;
}

VaultStateSnapshot VaultService::captureState() {
    std::lock_guard<std::mutex> lock(mu_);
    return captureStateLocked();
}

VaultStateSnapshot VaultService::captureStateLocked() const {
    VaultStateSnapshot result;
    result.revision = revision_;
    result.config.operator_binding = operator_binding_;
    result.config.ledger_caps = ledger_.caps();
    result.config.confirmation_policy = deposit_flow_.policy_;
    result.config.shadow_mode = deposit_flow_.shadow_mode_;
    result.config.withdrawal_caps = withdrawals_.caps_;
    result.config.withdrawal_policy = withdrawals_.policy_;
    result.entries = ledger_.entries();
    if (deposit_flow_.tracked_.size() != reorg_watcher_.deposit_block_hashes_.size() ||
        withdrawals_.requests_.size() != withdrawals_.states_.size())
        throw std::runtime_error("incomplete vault state inventory");
    result.deposits.reserve(deposit_flow_.tracked_.size());
    for (const auto& [outpoint, deposit] : deposit_flow_.tracked_) {
        if (outpoint != deposit.outpoint)
            throw std::runtime_error("vault deposit inventory binding mismatch");
        const auto hash = reorg_watcher_.deposit_block_hashes_.find(outpoint);
        if (hash == reorg_watcher_.deposit_block_hashes_.end())
            throw std::runtime_error("missing vault deposit observation");
        result.deposits.push_back({deposit, hash->second});
    }
    result.withdrawals.reserve(withdrawals_.requests_.size());
    for (const auto& [id, request] : withdrawals_.requests_) {
        if (id != request.request_id)
            throw std::runtime_error("vault withdrawal inventory binding mismatch");
        const auto state = withdrawals_.states_.find(id);
        if (state == withdrawals_.states_.end())
            throw std::runtime_error("missing vault withdrawal state");
        result.withdrawals.push_back({request, state->second});
    }
    return result;
}

void VaultService::requireCurrentOwner(const VaultStateWrite* transaction) const {
    if (!state_owner_) return;
    if (!transaction || EncodeVaultState(transaction->Base())!=EncodeVaultState(captureStateLocked()))
        throw std::runtime_error("vault durable state differs from the live owner");
}

void VaultService::commitAndPublish(PreparedState& state, VaultStateWrite* transaction) {
    if (state_owner_) {
        if (!transaction) throw std::runtime_error("vault durable transaction required");
        VaultStateSnapshot saved;
        saved.revision=revision_+1;
        saved.config.operator_binding=operator_binding_;
        saved.config.ledger_caps=state.ledger.caps();
        saved.config.confirmation_policy=state.deposits.policy_;
        saved.config.shadow_mode=state.deposits.shadow_mode_;
        saved.config.withdrawal_caps=state.withdrawals.caps_;
        saved.config.withdrawal_policy=state.withdrawals.policy_;
        saved.entries=state.ledger.entries();
        if (state.deposits.tracked_.size()!=state.watcher.deposit_block_hashes_.size() ||
            state.withdrawals.requests_.size()!=state.withdrawals.states_.size())
            throw std::runtime_error("incomplete candidate vault state");
        for (const auto& [outpoint,deposit]:state.deposits.tracked_)
            saved.deposits.push_back({deposit,state.watcher.deposit_block_hashes_.at(outpoint)});
        for (const auto& [id,request]:state.withdrawals.requests_)
            saved.withdrawals.push_back({request,state.withdrawals.states_.at(id)});
        // Replay and all allocations precede the checked durable commit.
        (void)ReplayVaultStateLedger(saved);
        transaction->Commit(saved);
    }
    publish(state);
}

std::shared_ptr<VaultService> VaultService::RestorePrepared(
    const VaultStateSnapshot& saved,std::shared_ptr<const VaultStateOwner> owner,
    std::unique_ptr<SigningBackend> backend,BlockHashAtHeightFn hash_at,TxIncludedAtFn included,
    VaultTipSnapshotFn capture_tip,std::shared_ptr<VaultWithdrawalDispatcher> dispatcher) {
    if (!owner || !backend || !capture_tip)
        throw std::runtime_error("vault durable service requires complete owners");
    auto ledger=ReplayVaultStateLedger(saved);
    auto service=std::make_shared<VaultService>(std::move(backend),saved.config,
        std::move(hash_at),std::move(included),std::move(capture_tip));
    service->ledger_=std::move(ledger);
    for (const auto& row:saved.deposits) {
        if (!service->deposit_flow_.tracked_.emplace(row.deposit.outpoint,row.deposit).second ||
            !service->reorg_watcher_.deposit_block_hashes_.emplace(row.deposit.outpoint,row.observed_block).second)
            throw std::runtime_error("duplicate restored vault deposit");
    }
    for (const auto& row:saved.withdrawals) {
        if (!service->withdrawals_.requests_.emplace(row.request.request_id,row.request).second ||
            !service->withdrawals_.states_.emplace(row.request.request_id,row.state).second)
            throw std::runtime_error("duplicate restored vault withdrawal");
    }
    service->revision_=saved.revision;
    service->state_owner_=std::move(owner);
    service->withdrawal_dispatcher_=std::move(dispatcher);
    return service;
}

void VaultService::recordDeposit(const std::array<uint8_t, 32>& txid, uint32_t vout,
                                 const AccountId& account, UnaAmount amount, uint64_t height,
                                 const std::array<uint8_t, 32>& block_hash) {
    auto transaction=state_owner_?state_owner_->Begin():nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    requireCurrentOwner(transaction.get());
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
    commitAndPublish(state,transaction.get());
}

void VaultService::tipChanged(uint64_t height) {
    std::unique_lock<std::mutex> lock(mu_);
    if (!capture_tip_) {
        if (state_owner_) throw std::runtime_error("vault durable canonical snapshot unavailable");
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
    auto transaction=state_owner_?state_owner_->Begin():nullptr;
    lock.lock();
    requireCurrentOwner(transaction.get());
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
    commitAndPublish(state,transaction.get());
}

WithdrawalId VaultService::enqueueWithdrawal(const AccountId& account, UnaAmount amount,
                                             const std::vector<uint8_t>& destination_script_pub_key) {
    auto transaction=state_owner_?state_owner_->Begin():nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    requireCurrentOwner(transaction.get());
    PreparedState state(*this);
    if(state_owner_)requirePendingCapacity(account,amount);
    const auto id = state.withdrawals.enqueue(account, amount, destination_script_pub_key);
    commitAndPublish(state,transaction.get());
    return id;
}

UnaAmount VaultService::pendingReservedLocked(const AccountId& account,UnaAmount spendable) const {
    UnaAmount reserved=0;
    for(const auto& [id,request]:withdrawals_.requests_) {
        if(request.account!=account)continue;
        const auto& state=withdrawals_.states_.at(id);
        if(!std::holds_alternative<WithdrawalPending>(state) &&
           !std::holds_alternative<WithdrawalSigning>(state))continue;
        if(reserved>spendable || request.amount>spendable-reserved)
            throw std::runtime_error("vault pending reservations exceed spendable funds");
        reserved+=request.amount;
    }
    return reserved;
}
void VaultService::requirePendingCapacity(const AccountId& account,UnaAmount additional) const {
    const auto spendable=ledger_.accountOr(account).spendable();
    const auto reserved=pendingReservedLocked(account,spendable);
    if(additional>spendable-reserved)
        throw std::runtime_error("vault withdrawal exceeds unreserved spendable funds");
}
VaultAccountMetrics VaultService::accountMetricsLocked(const AccountId& account) const {
    VaultAccountMetrics result;
    const auto found=ledger_.accounts().find(account);
    if(found!=ledger_.accounts().end()) {
        const auto& state=found->second;
        result={state.spendable(),state.confirmed(),state.pending(),state.locked(),state.operatorLoss()};
    }
    // Existing in-memory services keep their historical metrics. Durable
    // requests own Pending/Signing reservations before a body is retained.
    // Read the existing account in place; never copy its full lifecycle maps.
    const auto reserved=state_owner_?pendingReservedLocked(account,result.spendable):0;
    if(reserved>std::numeric_limits<UnaAmount>::max()-result.locked)
        throw std::runtime_error("vault locked reservation total exceeds range");
    result.spendable-=reserved;result.locked+=reserved;return result;
}

WithdrawalId VaultService::enqueueWithdrawal(const AccountId& account,UnaAmount amount,
    const std::vector<uint8_t>& script,const WithdrawalPaymentTerms& terms) {
    ValidateWithdrawalPaymentTerms(terms);
    if(!state_owner_)throw std::runtime_error("explicit durable withdrawal requires wallet state owner");
    auto transaction=state_owner_->Begin();
    std::lock_guard<std::mutex> lock(mu_);requireCurrentOwner(transaction.get());
    requirePendingCapacity(account,amount);
    PreparedState state(*this);const auto id=state.withdrawals.enqueue(account,amount,script);
    state.withdrawals.requests_.at(id).payment_terms=terms;
    commitAndPublish(state,transaction.get());return id;
}

std::optional<WithdrawalId> VaultService::processDurableWithdrawal() {
    if(!withdrawal_dispatcher_)throw std::runtime_error("vault durable withdrawal dispatcher unavailable");
    WithdrawalRequest request;bool new_dispatch=false;
    {
        auto transaction=state_owner_->Begin();
        std::lock_guard<std::mutex> lock(mu_);requireCurrentOwner(transaction.get());
        const WithdrawalRequest* selected=nullptr;
        for(const auto& [id,candidate]:withdrawals_.requests_) {
            const auto& state=withdrawals_.states_.at(id);
            if(!std::holds_alternative<WithdrawalPending>(state) &&
               !std::holds_alternative<WithdrawalSigning>(state))continue;
            if(!selected || candidate.created_at<selected->created_at ||
               (candidate.created_at==selected->created_at && id<selected->request_id))selected=&candidate;
        }
        if(!selected)return std::nullopt;
        if(!selected->payment_terms)throw std::runtime_error("vault historical withdrawal lacks explicit payment terms");
        ValidateWithdrawalPaymentTerms(*selected->payment_terms);
        // Copy all callback input before committing or publishing the transition.
        request=*selected;
        new_dispatch=std::holds_alternative<WithdrawalPending>(withdrawals_.states_.at(request.request_id));
        if(new_dispatch) {
            requirePendingCapacity(request.account,0);
            PreparedState state(*this);state.withdrawals.states_.at(request.request_id)=WithdrawalSigning{};
            commitAndPublish(state,transaction.get());
        }
    }
    // All vault/SQLite/recovery-key owners are gone before external work.
    // A thrown callback preserves Signing; retries never authorize a new body.
    const auto retained=new_dispatch?withdrawal_dispatcher_->DispatchNew(request):withdrawal_dispatcher_->Resolve(request);
    if(!retained)throw std::runtime_error("vault signing request has no retained wallet payment; request remains reserved");
    {
        auto transaction=state_owner_->Begin();
        std::lock_guard<std::mutex> lock(mu_);requireCurrentOwner(transaction.get());
        const auto& current=withdrawals_.requests_.at(request.request_id);
        const auto& current_state=withdrawals_.states_.at(request.request_id);
        if(current!=request)throw std::runtime_error("vault withdrawal payload changed during dispatch");
        if(const auto* prior=std::get_if<WithdrawalPaymentRetained>(&current_state)) {
            if(*prior!=*retained)throw std::runtime_error("conflicting retained vault payment");
            return request.request_id;
        }
        if(!std::holds_alternative<WithdrawalSigning>(current_state))
            throw std::runtime_error("vault withdrawal state changed during dispatch");
        requirePendingCapacity(request.account,0);
        PreparedState state(*this);
        state.ledger.append(WithdrawalInitiated{state.ledger.nextSeq(),WithdrawalQueue::now(),
            request.account,OutpointId{retained->txid,retained->vout},request.amount,backend_->backendId()});
        state.withdrawals.states_.at(request.request_id)=*retained;
        commitAndPublish(state,transaction.get());
    }
    return request.request_id;
}

std::optional<WithdrawalId> VaultService::processNextWithdrawal() {
    if(state_owner_)return processDurableWithdrawal();
    std::lock_guard<std::mutex> lock(mu_);
    // This existing externally-effectful path may mutate before throwing.
    // Invalidate captured observations before entering it in either case.
    requireRevisionCapacity();
    ++revision_;
    return withdrawals_.processNext();
}

void VaultService::markWithdrawalIncluded(const WithdrawalId& id, uint64_t height) {
    if(state_owner_)throw std::runtime_error("durable withdrawal inclusion requires canonical payment reconciliation");
    auto transaction=state_owner_?state_owner_->Begin():nullptr;
    std::lock_guard<std::mutex> lock(mu_);
    requireCurrentOwner(transaction.get());
    PreparedState state(*this);
    state.withdrawals.markBroadcastIncluded(id, height);
    commitAndPublish(state,transaction.get());
}

VaultAccountMetrics VaultService::accountMetrics(const AccountId& account) {
    std::lock_guard<std::mutex> lock(mu_);
    return accountMetricsLocked(account);
}

VaultMetrics VaultService::metrics() {
    std::lock_guard<std::mutex> lock(mu_);
    return {ledger_.totalOpenCredits(), ledger_.totalOperatorLoss(), ledger_.nextSeq(),
            ledger_.accounts().size(), withdrawals_.outstandingDepth()};
}

UnaAmount VaultService::accountSpendable(const AccountId& account) {
    std::lock_guard<std::mutex> lock(mu_);
    return accountMetricsLocked(account).spendable;
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
    return accountMetricsLocked(account).locked;
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
