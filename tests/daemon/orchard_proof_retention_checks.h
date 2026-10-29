#pragma once
namespace dinero {
TEST(OrchardProofRetention, BackendPolicyAndMissingResultRefuse){
    WalletServiceOwnerFixture f;auto use=WalletService::AcquireWalletUse(f.service);
#ifdef DINERO_TEST_ORCHARD_ORIGIN
    auto& jobs=use->OrchardProofs();EXPECT_FALSE(jobs.Query(orchard::Hash{1}));
#else
    EXPECT_THROW(use->OrchardProofs(),std::runtime_error);
#endif
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardProofRetention, IncompleteOrDifferentIntentNeverCollectsJob){
    OrchardServiceProofFixture f;auto request=f.Reserve();wallet::OrchardProofJobs idle;
    const auto before=f.Snapshot();
    EXPECT_THROW(idle.CopyResult(request.operation_id,request.committed_operations),std::runtime_error);
    idle.Submit(request.operation_id,request.committed_operations,std::move(request.plan),request.signing);
    EXPECT_EQ(idle.Query(request.operation_id),std::optional(wallet::OrchardProofJobs::State::Queued));
    EXPECT_THROW(idle.CopyResult(request.operation_id,request.committed_operations),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before); // Healthy queued executor, never started.
    EXPECT_TRUE(idle.Cancel(request.operation_id));
    EXPECT_THROW(idle.CopyResult(request.operation_id,request.committed_operations),std::runtime_error);
    EXPECT_EQ(idle.Query(request.operation_id),std::optional(wallet::OrchardProofJobs::State::Cancelled));
    idle.Forget(request.operation_id);EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardProofRetention, ExactProofSurvivesReadyWriteCommitAndLockedRefusal){
    OrchardServiceProofFixture f;auto request=f.Reserve(1,400000,true);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
    const auto empty=wallet::OrchardOperationQueue::Empty(f.Domain());
    const auto changed_context=orchard::SigningContext::Create(f.Domain(),1,{},request.transparent_outputs,request.fee_una);
    const auto other=empty.Reserve(request.operation_id,request.plan.Intent(changed_context));
    jobs.Submit(request.operation_id,request.committed_operations,std::move(request.plan),request.signing);
    ASSERT_EQ(ServiceProofTerminal(jobs,request.operation_id),wallet::OrchardProofJobs::State::Succeeded);
    const auto before=f.Snapshot();
    EXPECT_THROW(jobs.CopyResult(f.Operation(2),request.committed_operations),std::runtime_error);
    EXPECT_THROW(jobs.CopyResult(request.operation_id,empty),std::runtime_error);
    EXPECT_THROW(jobs.CopyResult(request.operation_id,other),std::runtime_error);
    auto proof=jobs.CopyResult(request.operation_id,f.Account(3).account.Operations());ASSERT_TRUE(proof);
    const auto exact=proof->Bytes();const auto authorization=f.Authorization(request,*proof);proof.reset();
    EXPECT_EQ(jobs.Query(request.operation_id),std::optional(wallet::OrchardProofJobs::State::Succeeded));
    f.Sql("CREATE TRIGGER refuse_retained_ready BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'Ready refusal'); END");
    EXPECT_THROW(f.Ready(1,request.revision,authorization),std::runtime_error);
    f.Sql("DROP TRIGGER refuse_retained_ready");EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(jobs.CopyResult(request.operation_id,f.Account(3).account.Operations())->Bytes(),exact);
    auto* db=f.Database();bool seen=false;sqlite3_commit_hook(db,[](void* p){*static_cast<bool*>(p)=true;return 1;},&seen);
    EXPECT_THROW(f.Ready(1,request.revision,authorization),std::runtime_error);
    sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_TRUE(seen);EXPECT_EQ(f.Snapshot(),before);
    use->Wallet().lockWallet();
    EXPECT_THROW(f.Ready(1,request.revision,authorization),std::runtime_error);
    EXPECT_EQ(jobs.Query(request.operation_id),std::optional(wallet::OrchardProofJobs::State::Succeeded));
    EXPECT_EQ(f.Snapshot(),before);use->Wallet().unlockWallet("canonical-fixture-pass",0);
    proof=jobs.CopyResult(request.operation_id,f.Account(3).account.Operations());ASSERT_TRUE(proof);EXPECT_EQ(proof->Bytes(),exact);
    const auto ready=f.Ready(1,request.revision,f.Authorization(request,*proof));
    EXPECT_EQ(ready.account.Operations().Entries().at(request.operation_id).transaction,authorization.Orchard().CanonicalBytes());
    EXPECT_EQ(jobs.CopyResult(request.operation_id,ready.account.Operations())->Bytes(),exact);
    // The original slot is collected only after the checked Ready commit.
    const auto collected=jobs.TakeResult(request.operation_id);ASSERT_TRUE(collected);EXPECT_EQ(collected->Bytes(),exact);
    EXPECT_FALSE(jobs.Query(request.operation_id));
    EXPECT_THROW(jobs.CopyResult(request.operation_id,ready.account.Operations()),std::runtime_error);
    const auto body=MempoolTransaction::FromOrchard(authorization.Transaction());const auto txid=body.GetTxid().AsUint256();(void)f.Mine(body);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    ASSERT_TRUE(f.f.db.getCoin(txid,0).ok());EXPECT_EQ(f.f.db.getCoin(txid,0)->amount,400000u);
}
#endif
} // namespace dinero
