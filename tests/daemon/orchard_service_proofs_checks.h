#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "wallet/orchard_proof_jobs.h"
#endif
namespace dinero {
TEST(OrchardServiceProofs, MissingOwnersAndBackendPolicyRefuseOrShareOneExecutor){
    auto empty=std::make_shared<WalletService>();
    EXPECT_THROW(WalletService::AcquireWalletUse(empty),std::runtime_error);
    WalletServiceOwnerFixture f;auto first=WalletService::AcquireWalletUse(f.service);auto nested=WalletService::AcquireWalletUse(f.service);
#ifdef DINERO_TEST_ORCHARD_ORIGIN
    auto& jobs=first->OrchardProofs();EXPECT_EQ(&nested->OrchardProofs(),&jobs);EXPECT_FALSE(jobs.Query(orchard::Hash{1}));
    EXPECT_THROW(jobs.Start(),std::runtime_error); // Already started once by the actual service.
#else
    EXPECT_THROW(first->OrchardProofs(),std::runtime_error);
    EXPECT_THROW(nested->OrchardProofs(),std::runtime_error);
#endif
    EXPECT_THROW(f.service->Stop(),std::logic_error);nested.reset();first.reset();
    EXPECT_NO_THROW(f.service->Stop());
    EXPECT_THROW(WalletService::AcquireWalletUse(f.service),std::runtime_error);
}
TEST(OrchardServiceProofs, OrdinaryStopReinitAndIdleScopeExitPreserveEncryptedWallet){
    WalletServiceOwnerFixture f;std::string address;
    {auto use=WalletService::AcquireWalletUse(f.service);address=use->Wallet().getPrimaryAddress();use->Wallet().encryptWallet("proof-service-fixture");use->Wallet().unlockWallet("proof-service-fixture",0);
#ifdef DINERO_TEST_ORCHARD_ORIGIN
     EXPECT_FALSE(use->OrchardProofs().Query(orchard::Hash{1}));
#else
     EXPECT_THROW(use->OrchardProofs(),std::runtime_error);
#endif
    }
    EXPECT_NO_THROW(f.service->Stop());ASSERT_TRUE(f.service->Init(f.context));
    {auto use=WalletService::AcquireWalletUse(f.service);use->Wallet().setUTXOIndex(f.index.get());use->Wallet().open("owner");EXPECT_TRUE(use->Wallet().isLocked());use->Wallet().unlockWallet("proof-service-fixture",0);EXPECT_EQ(use->Wallet().getPrimaryAddress(),address);
#ifdef DINERO_TEST_ORCHARD_ORIGIN
     EXPECT_FALSE(use->OrchardProofs().Query(orchard::Hash{1}));
#endif
    }
    // Healthy idle destruction: no active proof, churn, race or removed guard.
    std::weak_ptr<WalletService> previous=f.service;f.context.wallet.reset();f.service.reset();EXPECT_TRUE(previous.expired());
    f.service=std::make_shared<WalletService>();f.context.wallet=f.service;ASSERT_TRUE(f.service->Init(f.context));
    {auto use=WalletService::AcquireWalletUse(f.service);use->Wallet().setUTXOIndex(f.index.get());use->Wallet().open("owner");EXPECT_TRUE(use->Wallet().isLocked());use->Wallet().unlockWallet("proof-service-fixture",0);EXPECT_EQ(use->Wallet().getPrimaryAddress(),address);}
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
wallet::OrchardProofJobs::State ServiceProofTerminal(wallet::OrchardProofJobs& jobs,const orchard::Hash& id){
    const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(90);
    for(;;){const auto state=jobs.Query(id);OrchardAdmissionFixture::Require(bool(state));
        if(*state==wallet::OrchardProofJobs::State::Succeeded||*state==wallet::OrchardProofJobs::State::Failed||*state==wallet::OrchardProofJobs::State::Cancelled)return *state;
        if(std::chrono::steady_clock::now()>=limit)throw std::runtime_error("ordinary proof completion deadline");
        (void)jobs.WaitForChange(id,*state,std::chrono::seconds(10));
    }
}
struct OrchardServiceProofFixture : OrchardSpendOwnerFixture {
    auto Authorization(const wallet::OrchardAccountDelivery::PreparedSpend& request,const orchard::ProvedWalletBundle& bundle){
        const auto envelope=orchard::TransactionEnvelope::Create(0,{},request.transparent_outputs,request.fee_una,bundle.Bytes());
        return AuthorizeAtSelectedTip(envelope);
    }
};
}
TEST(OrchardServiceProofs, ActualBoundReservationProvesThenCommitsBeforeTransferAdmission){
    OrchardServiceProofFixture f;auto request=f.Reserve();auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
    const auto reserved=f.Account(3);ASSERT_EQ(reserved.revision,request.revision);EXPECT_EQ(reserved.account.Operations().Entries().at(request.operation_id).phase,wallet::OrchardOperationQueue::Phase::Reserved);
    jobs.Submit(request.operation_id,request.committed_operations,std::move(request.plan),request.signing);
    ASSERT_EQ(ServiceProofTerminal(jobs,request.operation_id),wallet::OrchardProofJobs::State::Succeeded);EXPECT_EQ(f.Account(3).revision,reserved.revision);
    auto bundle=jobs.TakeResult(request.operation_id);ASSERT_TRUE(bundle);EXPECT_FALSE(jobs.Query(request.operation_id));const auto authorization=f.Authorization(request,*bundle);
    const auto ready=f.Ready(1,request.revision,authorization);EXPECT_EQ(ready.account.Operations().Entries().at(request.operation_id).transaction,authorization.Orchard().CanonicalBytes());
    (void)f.Mine(MempoolTransaction::FromOrchard(authorization.Transaction()));EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(3).account.Scan().BalanceUna(),200000u);EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),200000u);
}
TEST(OrchardServiceProofs, CompletedProofDoesNotUnlockOrPublishAndReadyCanRetryAfterUnlock){
    OrchardServiceProofFixture f;auto request=f.Reserve(1,400000,true);auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
    jobs.Submit(request.operation_id,request.committed_operations,std::move(request.plan),request.signing);
    ASSERT_EQ(ServiceProofTerminal(jobs,request.operation_id),wallet::OrchardProofJobs::State::Succeeded);const auto before=f.Snapshot();
    use->Wallet().lockWallet();EXPECT_TRUE(use->Wallet().isLocked());EXPECT_EQ(jobs.Query(request.operation_id),std::optional(wallet::OrchardProofJobs::State::Succeeded));
    auto bundle=jobs.TakeResult(request.operation_id);ASSERT_TRUE(bundle);const auto authorization=f.Authorization(request,*bundle);
    EXPECT_THROW(f.Ready(1,request.revision,authorization),std::runtime_error);EXPECT_TRUE(use->Wallet().isLocked());EXPECT_EQ(f.Snapshot(),before);
    use->Wallet().unlockWallet("canonical-fixture-pass",0);const auto ready=f.Ready(1,request.revision,authorization);
    EXPECT_EQ(ready.account.Operations().Entries().at(request.operation_id).transaction,authorization.Orchard().CanonicalBytes());EXPECT_FALSE(jobs.Query(request.operation_id));
    const auto body=MempoolTransaction::FromOrchard(authorization.Transaction());const auto txid=body.GetTxid().AsUint256();(void)f.Mine(body);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);ASSERT_TRUE(f.f.db.getCoin(txid,0).ok());EXPECT_EQ(f.f.db.getCoin(txid,0)->amount,400000u);
}
#endif
} // namespace dinero
