#pragma once
namespace dinero {
TEST(OrchardProofFinalization, BackendPolicyRemainsExplicit){
    WalletServiceOwnerFixture f;auto use=WalletService::AcquireWalletUse(f.service);
#ifdef DINERO_TEST_ORCHARD_ORIGIN
    EXPECT_FALSE(use->OrchardProofs().Query(orchard::Hash{1}));
#else
    EXPECT_THROW(use->OrchardProofs(),std::runtime_error);
#endif
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct OrchardProofFinalizationFixture : OrchardProofOwnerFixture {
    auto Authorization(const wallet::OrchardAccountDelivery::QueuedSpend& request,
            const orchard::ProvedWalletBundle& proof){
        const auto envelope=orchard::TransactionEnvelope::Create(0,{},request.transparent_outputs,request.fee_una,proof.Bytes());
        return AuthorizeAtSelectedTip(envelope);
    }
    auto Finish(wallet::OrchardProofJobs& jobs,const consensus::VerifiedOrchardAuthorizations& authorization,
            uint32_t account=3,uint64_t session=0){
        const auto view=f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());
        auto use=WalletService::AcquireWalletUse(wallet);
        return wallet::OrchardAccountDelivery::FinalizeCatalogProofForReplay(use->Wallet(),session?session:Session(),
            {Domain(),102,account},**view,Operation(1),authorization,jobs);
    }
};
}
TEST(OrchardProofFinalization, CommitsExactReadyBeforeRetirementAndReopenRetry){
    OrchardProofFinalizationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
    auto request=f.Queue(jobs,true);ASSERT_TRUE(request->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,request->operation_id),wallet::OrchardProofJobs::State::Succeeded);
    auto proof=f.Read(jobs);ASSERT_TRUE(proof.proof);const auto authorization=f.Authorization(*request,*proof.proof);
    struct Observe{wallet::OrchardProofJobs* jobs;orchard::Hash id;bool seen=false;bool retained=false;} observed{&jobs,request->operation_id};
    sqlite3_commit_hook(f.Database(),[](void* p){auto& o=*static_cast<Observe*>(p);o.seen=true;o.retained=o.jobs->Query(o.id)==wallet::OrchardProofJobs::State::Succeeded;return 0;},&observed);
    const auto result=f.Finish(jobs,authorization);sqlite3_commit_hook(f.Database(),nullptr,nullptr);
    EXPECT_TRUE(observed.seen);EXPECT_TRUE(observed.retained);EXPECT_TRUE(result.retired_job);EXPECT_FALSE(jobs.Query(request->operation_id));
    const auto& entry=result.state.account.Operations().Entries().at(request->operation_id);
    EXPECT_EQ(entry.phase,wallet::OrchardOperationQueue::Phase::Ready);EXPECT_EQ(entry.transaction,authorization.Orchard().CanonicalBytes());
    const auto before=f.Snapshot();const auto retry=f.Finish(jobs,authorization);
    EXPECT_FALSE(retry.retired_job);EXPECT_EQ(retry.state.revision,result.state.revision);EXPECT_EQ(f.Snapshot(),before);
    use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);
    const auto reopened=f.Finish(jobs,authorization);EXPECT_FALSE(reopened.retired_job);EXPECT_EQ(reopened.state.revision,result.state.revision);EXPECT_EQ(f.Snapshot(),before);
    // Reopen clears the live script map. Match the checked wallet.open
    // binding owner before resuming canonical delivery after mining.
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    const auto body=MempoolTransaction::FromOrchard(authorization.Transaction());const auto txid=body.GetTxid().AsUint256();(void)f.Mine(body);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    ASSERT_TRUE(f.f.db.getCoin(txid,0).ok());EXPECT_EQ(f.f.db.getCoin(txid,0)->amount,400000u);
}
TEST(OrchardProofFinalization, FailedReadyWriteAndCommitRetainOriginalResultAndReservation){
    OrchardProofFinalizationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();
    auto request=f.Queue(jobs,true);ASSERT_TRUE(request->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,request->operation_id),wallet::OrchardProofJobs::State::Succeeded);
    auto proof=f.Read(jobs);ASSERT_TRUE(proof.proof);const auto bytes=proof.proof->Bytes();const auto authorization=f.Authorization(*request,*proof.proof);const auto before=f.Snapshot();
    f.Sql("CREATE TRIGGER refuse_final_ready BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'Ready refusal'); END");
    EXPECT_THROW(f.Finish(jobs,authorization),std::runtime_error);
    f.Sql("DROP TRIGGER refuse_final_ready");EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.Read(jobs).proof->Bytes(),bytes);
    bool seen=false;sqlite3_commit_hook(f.Database(),[](void* p){*static_cast<bool*>(p)=true;return 1;},&seen);
    EXPECT_THROW(f.Finish(jobs,authorization),std::runtime_error);
    sqlite3_commit_hook(f.Database(),nullptr,nullptr);EXPECT_TRUE(seen);EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(jobs.Query(request->operation_id),std::optional(wallet::OrchardProofJobs::State::Succeeded));EXPECT_EQ(f.Read(jobs).proof->Bytes(),bytes);
    use->Wallet().lockWallet();
    EXPECT_THROW(f.Finish(jobs,authorization),std::runtime_error);
    use->Wallet().unlockWallet("canonical-fixture-pass",0);EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.Read(jobs).proof->Bytes(),bytes);
    EXPECT_TRUE(f.Finish(jobs,authorization).retired_job);EXPECT_FALSE(jobs.Query(request->operation_id));
}
TEST(OrchardProofFinalization, MissingJobForeignOwnerAndIncompleteCatalogCannotPromoteReserved){
    OrchardProofFinalizationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();wallet::OrchardProofJobs empty;
    auto request=f.Queue(jobs,true);ASSERT_TRUE(request->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,request->operation_id),wallet::OrchardProofJobs::State::Succeeded);
    auto proof=f.Read(jobs);ASSERT_TRUE(proof.proof);const auto authorization=f.Authorization(*request,*proof.proof);const auto before=f.Snapshot();
    EXPECT_THROW(f.Finish(empty,authorization),std::runtime_error);
    EXPECT_THROW(f.Finish(jobs,authorization,17),std::runtime_error);
    EXPECT_THROW(f.Finish(jobs,authorization,3,f.Session()+1),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(jobs.Query(request->operation_id),std::optional(wallet::OrchardProofJobs::State::Succeeded));
    f.Sql("CREATE TEMP TABLE saved_final_owner AS SELECT * FROM orchard_wallet_snapshots WHERE account=17; DELETE FROM orchard_wallet_snapshots WHERE account=17");
    const auto missing=f.Snapshot();
    EXPECT_THROW(f.Finish(jobs,authorization),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),missing);EXPECT_EQ(jobs.Query(request->operation_id),std::optional(wallet::OrchardProofJobs::State::Succeeded));
    f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_final_owner; DROP TABLE saved_final_owner");EXPECT_EQ(f.Snapshot(),before);
    EXPECT_TRUE(f.Finish(jobs,authorization).retired_job);
}
#endif
} // namespace dinero
