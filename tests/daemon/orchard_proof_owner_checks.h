#pragma once
namespace dinero {
TEST(OrchardProofOwner, BackendPolicyAndManagerTokensRemainExplicit){
    WalletServiceOwnerFixture f;WalletServiceOwnerFixture other;
    auto use=WalletService::AcquireWalletUse(f.service);auto second=WalletService::AcquireWalletUse(other.service);
    const auto token=use->Wallet().AcquireDatabaseLease()->InstanceToken();
    EXPECT_EQ(token,use->Wallet().AcquireDatabaseLease()->InstanceToken());
    EXPECT_NE(token,second->Wallet().AcquireDatabaseLease()->InstanceToken());
#ifdef DINERO_TEST_ORCHARD_ORIGIN
    EXPECT_FALSE(use->OrchardProofs().Query(orchard::Hash{1}));
#else
    EXPECT_THROW(use->OrchardProofs(),std::runtime_error);
#endif
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct OrchardProofOwnerFixture : OrchardProofCapacityFixture {
    auto Read(wallet::OrchardProofJobs& jobs,uint32_t account=3,uint64_t session=0){
        const auto view=f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());
        auto use=WalletService::AcquireWalletUse(wallet);
        return wallet::OrchardAccountDelivery::ReadCatalogProofForReplay(use->Wallet(),session?session:Session(),
            {Domain(),102,account},**view,Operation(1),jobs);
    }
};
}
TEST(OrchardProofOwner, AuthenticatedQueuedOwnerAndMissingJobNeverReleaseReservation){
    OrchardProofOwnerFixture f;wallet::OrchardProofJobs jobs,empty;auto request=f.Queue(jobs);ASSERT_TRUE(request->enqueued);
    const auto before=f.Snapshot();const auto state=f.Read(jobs);
    EXPECT_EQ(state.revision,request->revision);EXPECT_EQ(state.state,std::optional(wallet::OrchardProofJobs::State::Queued));EXPECT_FALSE(state.proof);
    const auto missing=f.Read(empty);EXPECT_FALSE(missing.state);EXPECT_FALSE(missing.proof);
    EXPECT_THROW(f.Read(jobs,17),std::runtime_error);
    EXPECT_THROW(f.Read(jobs,3,f.Session()+1),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(jobs.Query(f.Operation(1)),std::optional(wallet::OrchardProofJobs::State::Queued));
}
TEST(OrchardProofOwner, DifferentManagerAndReopenedSessionCannotCollectExistingJob){
    OrchardProofOwnerFixture f;wallet::OrchardProofJobs jobs;auto request=f.Queue(jobs);ASSERT_TRUE(request->enqueued);
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto before=f.Snapshot();
    {
        // Second ordinary manager opens the same isolated, already-committed
        // wallet database. Its persistent identity/keys/account are identical,
        // but its manager token is distinct. There are no concurrent writes.
        WalletManager twin(f.f.path/"wallet-runtime",f.f.logger.get());twin.open("canonical-recovery");twin.unlockWallet("canonical-fixture-pass",0);
        auto twin_lease=twin.AcquireDatabaseLease();auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_NE(twin_lease->InstanceToken(),use->Wallet().AcquireDatabaseLease()->InstanceToken());
        EXPECT_THROW(wallet::OrchardAccountDelivery::ReadCatalogProofForReplay(twin,twin_lease->Session(),
            {f.Domain(),102,3},**view,f.Operation(1),jobs),std::runtime_error);
    }
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.Read(jobs).state,std::optional(wallet::OrchardProofJobs::State::Queued));
    const auto old_session=f.Session();{auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    EXPECT_NE(f.Session(),old_session);
    EXPECT_THROW(f.Read(jobs),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(jobs.Query(f.Operation(1)),std::optional(wallet::OrchardProofJobs::State::Queued));
}
TEST(OrchardProofOwner, UnboundLegacyJobCannotBecomeAuthenticatedHostAuthority){
    OrchardProofOwnerFixture f;auto direct=f.Reserve();wallet::OrchardProofJobs legacy;
    legacy.Submit(direct.operation_id,direct.committed_operations,std::move(direct.plan),direct.signing);
    const auto before=f.Snapshot();
    EXPECT_THROW(f.Read(legacy),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(legacy.Query(direct.operation_id),std::optional(wallet::OrchardProofJobs::State::Queued));
}
TEST(OrchardProofOwner, EntireCatalogAndCheckedReadCommitRequiredBeforeResultReturn){
    OrchardProofOwnerFixture f;wallet::OrchardProofJobs jobs;auto request=f.Queue(jobs);ASSERT_TRUE(request->enqueued);const auto before=f.Snapshot();
    f.Sql("CREATE TEMP TABLE saved_proof_owner AS SELECT * FROM orchard_wallet_snapshots WHERE account=17; DELETE FROM orchard_wallet_snapshots WHERE account=17");
    const auto missing=f.Snapshot();
    EXPECT_THROW(f.Read(jobs),std::runtime_error);EXPECT_EQ(f.Snapshot(),missing);
    f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_proof_owner; DROP TABLE saved_proof_owner");EXPECT_EQ(f.Snapshot(),before);
    auto* db=f.Database();sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){return action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_retained"?SQLITE_DENY:SQLITE_OK;},nullptr);
    EXPECT_THROW(f.Read(jobs),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_EQ(f.Snapshot(),before);
    bool seen=false;sqlite3_set_authorizer(db,[](void* p,int action,const char* name,const char*,const char*,const char*){
        if(action==SQLITE_TRANSACTION&&name&&std::string_view(name)=="COMMIT"){*static_cast<bool*>(p)=true;return SQLITE_DENY;}return SQLITE_OK;},&seen);
    EXPECT_THROW(f.Read(jobs),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(seen);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.Read(jobs).state,std::optional(wallet::OrchardProofJobs::State::Queued));
}
TEST(OrchardProofOwner, CompletedProofCopyRequiresCurrentOwnerAndSurvivesFailedReadCommit){
    OrchardProofOwnerFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& jobs=use->OrchardProofs();auto request=f.Queue(jobs,true);ASSERT_TRUE(request->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,request->operation_id),wallet::OrchardProofJobs::State::Succeeded);
    const auto before=f.Snapshot();auto captured=f.Read(jobs);ASSERT_TRUE(captured.proof);EXPECT_EQ(captured.revision,request->revision);
    const auto exact=captured.proof->Bytes();captured.proof.reset();
    auto* db=f.Database();bool seen=false;sqlite3_set_authorizer(db,[](void* p,int action,const char* name,const char*,const char*,const char*){
        if(action==SQLITE_TRANSACTION&&name&&std::string_view(name)=="COMMIT"){*static_cast<bool*>(p)=true;return SQLITE_DENY;}return SQLITE_OK;},&seen);
    EXPECT_THROW(f.Read(jobs),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(seen);
    use->Wallet().lockWallet();
    EXPECT_THROW(f.Read(jobs),std::runtime_error);EXPECT_EQ(f.Snapshot(),before);
    use->Wallet().unlockWallet("canonical-fixture-pass",0);captured=f.Read(jobs);ASSERT_TRUE(captured.proof);EXPECT_EQ(captured.proof->Bytes(),exact);
    EXPECT_EQ(jobs.Query(request->operation_id),std::optional(wallet::OrchardProofJobs::State::Succeeded));
    const auto envelope=orchard::TransactionEnvelope::Create(0,{},request->transparent_outputs,request->fee_una,captured.proof->Bytes());
    const auto authorization=f.AuthorizeAtSelectedTip(envelope);
    const auto ready=f.Ready(1,request->revision,authorization);EXPECT_EQ(ready.account.Operations().Entries().at(request->operation_id).transaction,authorization.Orchard().CanonicalBytes());
    const auto after_ready=f.Read(jobs);ASSERT_TRUE(after_ready.proof);EXPECT_EQ(after_ready.proof->Bytes(),exact);
    const auto body=MempoolTransaction::FromOrchard(authorization.Transaction());const auto txid=body.GetTxid().AsUint256();(void)f.Mine(body);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);ASSERT_TRUE(f.f.db.getCoin(txid,0).ok());EXPECT_EQ(f.f.db.getCoin(txid,0)->amount,400000u);
}
#endif
} // namespace dinero
