#include "daemon/runtime_delivery_worker.h"
#include "daemon/runtime_reorg_readmission.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/wallet_service.h"
#include "storage/chain_db.h"
#include "vault/vault_runtime.h"
#ifdef DINERO_HAS_ORCHARD_RUNTIME_READER
#include "wallet/runtime_wallet_recovery.h"
#endif
#include <limits>
#include <algorithm>
#include <stdexcept>

namespace dinero {
RuntimeDeliveryWorker::RuntimeDeliveryWorker(std::shared_ptr<ChainstateService> source,
        std::shared_ptr<WalletService> wallet)
    : RuntimeDeliveryWorker(std::move(source),std::move(wallet),Limits{}) {}
RuntimeDeliveryWorker::RuntimeDeliveryWorker(std::shared_ptr<ChainstateService> source,
        std::shared_ptr<WalletService> wallet,Limits limits)
    : source_(std::move(source)),wallet_(std::move(wallet)),limits_(limits) {
    if (!source_ || !limits_.intents_per_slice || limits_.intents_per_slice>1024 ||
        limits_.retry_interval<std::chrono::milliseconds(1) ||
        limits_.retry_interval>std::chrono::minutes(1))
        throw std::invalid_argument("Invalid runtime delivery worker owner or limits");
}
RuntimeDeliveryWorker::~RuntimeDeliveryWorker() { Stop(); }
void RuntimeDeliveryWorker::Start() {
    std::lock_guard lifecycle(lifecycle_);
    if (thread_.joinable()) throw std::logic_error("Runtime delivery worker already started");
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping=false;state_->requested=true;state_->report=Report{};state_->report.running=true;
    }
    try { thread_=std::thread([this]{Run();}); }
    catch (...) {
        std::lock_guard lock(state_->mutex);state_->stopping=true;state_->requested=false;state_->report.running=false;throw;
    }
}
void RuntimeDeliveryWorker::Stop() {
    std::lock_guard lifecycle(lifecycle_);
    {std::lock_guard lock(state_->mutex);state_->stopping=true;state_->requested=false;}
    state_->wake.notify_one();
    // No consumer mutex or selected/wallet owner is held while joining.
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(state_->mutex);state_->report.running=false;
}
void RuntimeDeliveryWorker::RequestReplay() noexcept { CaptureWakeHandle().RequestReplay(); }
void RuntimeDeliveryWorker::WakeHandle::RequestReplay() const noexcept {
    {std::lock_guard lock(state_->mutex);if(state_->stopping)return;state_->requested=true;}
    state_->wake.notify_one();
}
bool RuntimeDeliveryWorker::WakeHandle::Running() const noexcept {
    std::lock_guard lock(state_->mutex);return state_->report.running && !state_->stopping;
}
RuntimeDeliveryWorker::Report RuntimeDeliveryWorker::Snapshot() const {
    std::lock_guard lock(state_->mutex);return state_->report;
}
bool RuntimeDeliveryWorker::Stopping() const {
    std::lock_guard lock(state_->mutex);return state_->stopping;
}
void RuntimeDeliveryWorker::ObserveVault(Report& report,VaultObservationCache& last) const {
    report.vault=VaultOutcome::Deferred;report.vault_tip={};report.vault_height=report.vault_revision=0;
    try {
        const auto service=vault::GetVaultRuntimeService();
        if(!service) {last={};report.vault=VaultOutcome::NoAttachedRuntime;return;}
        if(!service->hasWalletStateOwner()) {last={};return;}
        uint64_t height;std::array<uint8_t,32> hash{};
        {
            const auto lifetime=ChainstateService::AcquireWalletIndexUse(source_);
            const auto selected=source_->AcquireBlockIngressActivationLock();
            const auto* db=source_->GetChainDB();if(!db)throw std::runtime_error("vault worker source unavailable");
            const auto tip=db->getTip();if(!tip.ok() || tip->height<0 || uint64_t(tip->height)>UINT32_MAX)
                throw std::runtime_error("vault worker tip unavailable");
            const auto canonical=source_->getCanonicalBlockHash(static_cast<uint32_t>(tip->height));
            if(!canonical.ok() || *canonical!=tip->hash)throw std::runtime_error("vault worker tip incoherent");
            height=static_cast<uint64_t>(tip->height);std::copy(canonical->begin(),canonical->end(),hash.begin());
        }
        // Never acquire the vault/wallet/seed/SQLite owner under selected
        // ownership. tipChanged independently binds its actual captured tip.
        if(last.service.lock()==service && last.height==height && last.tip==hash &&
           last.revision==service->currentRevision()) {
            report.vault=VaultOutcome::UnchangedSinceObservation;
        } else {
            const auto applied=service->tipChanged(height);
            if(!std::any_of(applied.block_hash.begin(),applied.block_hash.end(),[](uint8_t v){return v!=0;}))
                throw std::runtime_error("vault worker observation identity absent");
            last={service,applied.height,applied.revision,applied.block_hash};
            report.vault=VaultOutcome::ObservedTip;
        }
        report.vault_height=last.height;report.vault_revision=last.revision;report.vault_tip=last.tip;
    } catch(...) {last={};/* Existing durable state remains owned; retry later. */}
}
void RuntimeDeliveryWorker::RecoverWallet(Report& report) const {
    report.wallet=WalletOutcome::Deferred;report.wallet_head.reset();
    if (!wallet_) {report.wallet=WalletOutcome::ExplicitlyAbsent;return;}
    try {
        auto use=WalletService::AcquireWalletUse(wallet_);
        uint64_t session;
        {
            auto lease=use->Wallet().AcquireDatabaseLease();
            if (!use->Wallet().hasActiveWallet()) {report.wallet=WalletOutcome::NoActiveWallet;return;}
            session=lease->Session();
        }
        auto index=ChainstateService::AcquireWalletIndexUse(source_);
        const auto recovered=source_->resumeRuntimeWalletRecoveryIfNeeded(use->Wallet(),index->Index(),session);
        if (!recovered.ok()) return;
        if (!*recovered) {report.wallet=WalletOutcome::NoLog;return;}
#ifdef DINERO_HAS_ORCHARD_RUNTIME_READER
        report.wallet=WalletOutcome::AppliedPrefix;report.wallet_head=(*recovered)->observed_head;
#endif
    } catch (...) { /* Durable store prefixes remain retryable; never enroll here. */ }
}
bool RuntimeDeliveryWorker::ReconcileSlice(Report& report) const {
    report.source_deferred=true;
    for(size_t i=0;i<limits_.intents_per_slice;++i) {
        if (Stopping()) return false;
        const auto result=source_->readmitRuntimeReorg(report.after_intent);
        if (!result.ok() || !*result) return false;
        const auto& read=**result;
        if (report.reorg_head && *report.reorg_head!=read.observed_head) return false;
        report.reorg_head=read.observed_head;
        if (!read.intent) {
            report.source_deferred=false;report.reorg_eof=true;return false;
        }
        if (read.intent->sequence<=report.after_intent.sequence) return false;
        if (report.intents==std::numeric_limits<size_t>::max() ||
            read.entries.size()>std::numeric_limits<size_t>::max()-report.admission_attempts) return false;
        ++report.intents;report.admission_attempts+=read.entries.size();
        for(const auto& entry:read.entries) if(entry.present_after_attempt) ++report.present_after_attempt;
        report.after_intent=*read.intent;
    }
    report.source_deferred=false;
    return true; // More retained plans may exist; continue without waiting for another callback.
}
void RuntimeDeliveryWorker::Run() noexcept {
    Report pass;VaultObservationCache vault_observation;bool continue_pass=false;uint64_t slices=0;
    try {
        for(;;) {
            bool restart=false;
            {
                std::unique_lock lock(state_->mutex);
                if (!continue_pass) state_->wake.wait_for(lock,limits_.retry_interval,[&]{return state_->stopping||state_->requested;});
                if (state_->stopping) break;
                restart=state_->requested||!continue_pass;state_->requested=false;
            }
            if (restart) {pass=Report{};RecoverWallet(pass);ObserveVault(pass,vault_observation);}
            try {continue_pass=ReconcileSlice(pass);}
            catch (...) {pass.source_deferred=true;pass.reorg_eof=false;continue_pass=false;}
            if (slices!=std::numeric_limits<uint64_t>::max())++slices;
            pass.slices=slices;pass.running=true;
            {std::lock_guard lock(state_->mutex);state_->report=pass;}
        }
    } catch (...) {
        std::lock_guard lock(state_->mutex);state_->report.source_deferred=true;state_->report.reorg_eof=false;
    }
    std::lock_guard lock(state_->mutex);state_->report.running=false;
}
} // namespace dinero
