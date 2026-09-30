#pragma once
#include "daemon/runtime_delivery_worker.h"
namespace dinero {
namespace {
using DeliveryWorker=RuntimeDeliveryWorker;
DeliveryWorker::Limits DeliveryWorkerFixtureLimits(size_t batch=1) {
    return {batch,std::chrono::minutes(1)};
}
template<typename Predicate> DeliveryWorker::Report WaitDeliveryWorker(DeliveryWorker& worker,Predicate predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    for(;;) {
        const auto report=worker.Snapshot();if(predicate(report))return report;
        if(std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("patched delivery worker did not produce required report");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
}
TEST(RuntimeDeliveryWorker, RequiredOwnersLimitsAndActualUnavailableSource) {
    EXPECT_THROW(DeliveryWorker({},{}),std::invalid_argument);
    auto source=std::make_shared<ChainstateService>();
    EXPECT_THROW(DeliveryWorker(source,{},DeliveryWorkerFixtureLimits(0)),std::invalid_argument);
    EXPECT_THROW(DeliveryWorker(source,{},DeliveryWorkerFixtureLimits(1025)),std::invalid_argument);
    DeliveryWorker worker(source,{},DeliveryWorkerFixtureLimits());
    EXPECT_FALSE(worker.Snapshot().running);worker.RequestReplay();worker.Start();
    EXPECT_THROW(worker.Start(),std::logic_error);
    auto report=WaitDeliveryWorker(worker,[](const auto& r){return r.slices>0;});
    EXPECT_TRUE(report.source_deferred);EXPECT_FALSE(report.reorg_eof);
    EXPECT_EQ(report.wallet,DeliveryWorker::WalletOutcome::ExplicitlyAbsent);
    EXPECT_FALSE(report.wallet_head);EXPECT_EQ(report.after_intent.sequence,0u);
    worker.Stop();EXPECT_FALSE(worker.Snapshot().running);EXPECT_NO_THROW(worker.Stop());
    worker.Start();report=WaitDeliveryWorker(worker,[](const auto& r){return r.slices>0;});
    EXPECT_TRUE(report.source_deferred);worker.Stop();
}
TEST(RuntimeDeliveryWorker, HistoricalWalletNoLogAndStoppedOwnerRemainDistinct) {
    CanonicalRecoveryHistoricalFixture f;DeliveryWorker worker(f.source,f.wallet,DeliveryWorkerFixtureLimits());
    worker.Start();auto report=WaitDeliveryWorker(worker,[](const auto& r){return r.slices>0;});worker.Stop();
    EXPECT_EQ(report.wallet,DeliveryWorker::WalletOutcome::NoLog);EXPECT_FALSE(report.wallet_head);
    EXPECT_EQ(f.wallet->getCurrentWalletName(),"historical-recovery");
    f.wallet->Stop();worker.Start();report=WaitDeliveryWorker(worker,[](const auto& r){return r.slices>0;});worker.Stop();
    EXPECT_EQ(report.wallet,DeliveryWorker::WalletOutcome::Deferred);EXPECT_FALSE(report.wallet_head);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(RuntimeDeliveryWorker, StartupAndRestartRecoverEncryptedEarnedWalletPrefix) {
    CanonicalRecoveryFixture f(true);f.MineAndAdopt();
    DeliveryWorker worker(f.f.service,f.wallet,DeliveryWorkerFixtureLimits());worker.Start();
    auto report=WaitDeliveryWorker(worker,[](const auto& r){return r.slices>0;});worker.Stop();
    ASSERT_EQ(report.wallet,DeliveryWorker::WalletOutcome::AppliedPrefix);ASSERT_TRUE(report.wallet_head);
    EXPECT_EQ(report.wallet_head->sequence,1u);EXPECT_EQ(f.ReadAccount().account.Scan().BalanceUna(),500000u);
    EXPECT_EQ(f.ReadAccount().account.Delivery().sequence,1u);
    f.MineEmpty();{auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().lockWallet();}
    worker.Start();report=WaitDeliveryWorker(worker,[](const auto& r){return r.slices>0;});worker.Stop();
    EXPECT_EQ(report.wallet,DeliveryWorker::WalletOutcome::Deferred);EXPECT_FALSE(report.wallet_head);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    worker.Start();report=WaitDeliveryWorker(worker,[](const auto& r){return r.slices>0;});worker.Stop();
    ASSERT_EQ(report.wallet,DeliveryWorker::WalletOutcome::AppliedPrefix);ASSERT_TRUE(report.wallet_head);
    EXPECT_EQ(report.wallet_head->sequence,2u);EXPECT_EQ(f.ReadAccount().account.Delivery().sequence,2u);
    EXPECT_EQ(f.ReadAccount().account.Scan().BalanceUna(),500000u);
}
TEST(RuntimeDeliveryWorker, MissingBaselineRemainsDeferredWithoutEnrollment) {
    CanonicalRecoveryFixture f;f.EnrollAccount();const auto keys=f.Keys();const auto [body,bundle]=f.Shield(keys);(void)f.Mine(body);
    DeliveryWorker worker(f.f.service,f.wallet,DeliveryWorkerFixtureLimits());worker.Start();
    const auto report=WaitDeliveryWorker(worker,[](const auto& r){return r.slices>0;});worker.Stop();
    EXPECT_EQ(report.wallet,DeliveryWorker::WalletOutcome::Deferred);EXPECT_FALSE(report.wallet_head);
    EXPECT_EQ(f.ReadAccount().account.Delivery().sequence,0u);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    const auto session=use->Wallet().AcquireDatabaseLease()->Session();
    EXPECT_FALSE(RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session));
    EXPECT_FALSE(RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),session));
}
TEST(RuntimeDeliveryWorker, BoundedSlicesDrainRetainedPlansWithoutCallbackAndRestartFromOrigin) {
    ReorgReadmissionFixture f;const auto block=f.Build();ASSERT_TRUE(block);ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());
    auto* tip=f.f.service->GetActiveTip();f.Prepare({tip});f.Prepare({tip});const auto last=f.Prepare({tip});
    const auto saved=f.IntentHead();ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,tip));
    DeliveryWorker worker(f.f.service,{},DeliveryWorkerFixtureLimits());worker.Start();
    auto report=WaitDeliveryWorker(worker,[](const auto& r){return r.reorg_eof;});worker.Stop();
    EXPECT_FALSE(report.source_deferred);EXPECT_GE(report.slices,4u);EXPECT_EQ(report.intents,3u);
    EXPECT_EQ(report.admission_attempts,3u);EXPECT_EQ(report.present_after_attempt,3u);EXPECT_EQ(report.after_intent,last);
    EXPECT_EQ(f.f.ingress->mempool().size(),1u);EXPECT_EQ(f.IntentHead(),saved);
    worker.Start();report=WaitDeliveryWorker(worker,[](const auto& r){return r.reorg_eof;});worker.Stop();
    EXPECT_EQ(report.intents,3u);EXPECT_EQ(report.admission_attempts,3u);EXPECT_EQ(report.present_after_attempt,3u);
    EXPECT_EQ(report.after_intent,last);EXPECT_EQ(f.f.ingress->mempool().size(),1u);EXPECT_EQ(f.IntentHead(),saved);
}
TEST(RuntimeDeliveryWorker, LaterCommittedPrefixIsRevisitedAfterWake) {
    ReorgReadmissionFixture f;const auto first=f.Build();ASSERT_TRUE(first);ASSERT_TRUE(f.Submit(first->WireBytes()).accepted());
    auto* lower=f.f.service->GetActiveTip();BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    const auto second=assembler.CreateOrchardBlock(OrchardMiningPayout);ASSERT_TRUE(second);ASSERT_TRUE(f.Submit(second->WireBytes()).accepted());
    auto* upper=f.f.service->GetActiveTip();const auto retained=f.Prepare({upper,lower});const auto saved=f.IntentHead();
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,upper));
    DeliveryWorker worker(f.f.service,{},DeliveryWorkerFixtureLimits());worker.Start();
    auto report=WaitDeliveryWorker(worker,[](const auto& r){return r.reorg_eof;});
    EXPECT_EQ(report.after_intent,retained);EXPECT_EQ(report.intents,1u);EXPECT_EQ(report.admission_attempts,0u);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,lower));
    const auto prior_slices=report.slices;worker.RequestReplay();
    report=WaitDeliveryWorker(worker,[&](const auto& r){return r.reorg_eof&&r.slices>prior_slices;});worker.Stop();
    EXPECT_EQ(report.after_intent,retained);EXPECT_EQ(report.admission_attempts,1u);EXPECT_EQ(report.present_after_attempt,1u);
    EXPECT_EQ(f.f.ingress->mempool().size(),1u);EXPECT_EQ(f.IntentHead(),saved);
}
#endif
} // namespace dinero
