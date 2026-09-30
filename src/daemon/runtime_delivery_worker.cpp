#include "daemon/runtime_delivery_worker.h"
#include "daemon/runtime_reorg_readmission.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/wallet_service.h"
#ifdef DINERO_HAS_ORCHARD_RUNTIME_READER
#include "wallet/runtime_wallet_recovery.h"
#endif
#include <limits>
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
        std::lock_guard lock(mutex_);
        stopping_=false;requested_=true;report_=Report{};report_.running=true;
    }
    try { thread_=std::thread([this]{Run();}); }
    catch (...) {
        std::lock_guard lock(mutex_);stopping_=true;requested_=false;report_.running=false;throw;
    }
}
void RuntimeDeliveryWorker::Stop() {
    std::lock_guard lifecycle(lifecycle_);
    {std::lock_guard lock(mutex_);stopping_=true;requested_=false;}
    wake_.notify_one();
    // No consumer mutex or selected/wallet owner is held while joining.
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);report_.running=false;
}
void RuntimeDeliveryWorker::RequestReplay() noexcept {
    {std::lock_guard lock(mutex_);if(stopping_)return;requested_=true;}
    wake_.notify_one();
}
RuntimeDeliveryWorker::Report RuntimeDeliveryWorker::Snapshot() const {
    std::lock_guard lock(mutex_);return report_;
}
bool RuntimeDeliveryWorker::Stopping() const {
    std::lock_guard lock(mutex_);return stopping_;
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
    Report pass;bool continue_pass=false;uint64_t slices=0;
    try {
        for(;;) {
            bool restart=false;
            {
                std::unique_lock lock(mutex_);
                if (!continue_pass) wake_.wait_for(lock,limits_.retry_interval,[&]{return stopping_||requested_;});
                if (stopping_) break;
                restart=requested_||!continue_pass;requested_=false;
            }
            if (restart) {pass=Report{};RecoverWallet(pass);}
            try {continue_pass=ReconcileSlice(pass);}
            catch (...) {pass.source_deferred=true;pass.reorg_eof=false;continue_pass=false;}
            if (slices!=std::numeric_limits<uint64_t>::max())++slices;
            pass.slices=slices;pass.running=true;
            {std::lock_guard lock(mutex_);report_=pass;}
        }
    } catch (...) {
        std::lock_guard lock(mutex_);report_.source_deferred=true;report_.reorg_eof=false;
    }
    std::lock_guard lock(mutex_);report_.running=false;
}
} // namespace dinero
