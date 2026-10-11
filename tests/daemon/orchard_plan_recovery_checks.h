#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
TEST(OrchardPlanRecovery, ReopenTransferAndUnshieldKeepOriginalIntent){
    for(bool withdraw:{false,true}){
        OrchardSpendRequestFixture f;
        std::optional<wallet::OrchardOperationQueue::Entry> original;
        {wallet::OrchardProofJobs old;auto queued=f.Request(old,withdraw);ASSERT_TRUE(queued->enqueued);original=queued->durable;}
        ASSERT_TRUE(original);ASSERT_TRUE(original->recovery);
        const auto before=f.Snapshot();
        {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unload();use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
        wallet::OrchardProofJobs jobs;const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
        auto use=WalletService::AcquireWalletUse(f.wallet);
        const auto read=[&](bool resume){return wallet::OrchardAccountDelivery::ReadStoredCatalogRequestProofForReplay(
            use->Wallet(),f.Session(),{f.Domain(),102,3},**view,f.Operation(1),false,jobs,resume);};
        EXPECT_FALSE(read(false).state);EXPECT_FALSE(jobs.Query(f.Operation(1)));
        auto resumed=read(true);EXPECT_EQ(resumed.state,std::optional(wallet::OrchardProofJobs::State::Queued));
        EXPECT_EQ(resumed.durable.message,original->message);EXPECT_EQ(resumed.durable.nullifiers,original->nullifiers);
        ASSERT_TRUE(resumed.durable.recovery);
        EXPECT_TRUE(std::equal(original->recovery->Bytes().begin(),original->recovery->Bytes().end(),
            resumed.durable.recovery->Bytes().begin(),resumed.durable.recovery->Bytes().end()));
        EXPECT_EQ(f.Snapshot(),before);jobs.Start();
        ASSERT_EQ(ServiceProofTerminal(jobs,f.Operation(1)),wallet::OrchardProofJobs::State::Succeeded);
        auto completed=read(true);ASSERT_TRUE(completed.proof);
        EXPECT_EQ(completed.proof->Authorization().SigningDigest(),original->message);
        EXPECT_EQ(f.Snapshot(),before);
        const auto& details=*completed.durable.spend_request;
        auto envelope=orchard::TransactionEnvelope::Create(0,{},details.outputs,details.fee_una,completed.proof->Bytes());
        auto authorization=f.AuthorizeAtSelectedTip(envelope);(void)f.Ready(1,completed.revision,authorization);
        wallet::OrchardProofJobs absent;
        const auto ready=wallet::OrchardAccountDelivery::ReadStoredCatalogRequestProofForReplay(
            use->Wallet(),f.Session(),{f.Domain(),102,3},**view,f.Operation(1),false,absent,true);
        EXPECT_EQ(ready.durable.phase,wallet::OrchardOperationQueue::Phase::Ready);
        EXPECT_EQ(ready.durable.transaction,authorization.Orchard().CanonicalBytes());EXPECT_FALSE(absent.Query(f.Operation(1)));
    }
}
TEST(OrchardPlanRecovery, ShieldResumeAndFinalReadCommitRefusalKeepReservation){
    ShieldReservationFixture f;std::optional<wallet::OrchardOperationQueue::Entry> original;
    {wallet::OrchardProofJobs old;auto queued=f.Queue(old);ASSERT_TRUE(queued->enqueued);original=queued->durable;}
    ASSERT_TRUE(original);ASSERT_TRUE(original->recovery);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& manager=use->Wallet();
    const auto view=f.View();const auto payments=f.Payments();const auto inputs=f.Inputs();
    const std::vector<orchard::TransparentOutput> outputs{{70000,f.script}};
    wallet::OrchardProofJobs jobs;
    const auto call=[&]{return wallet::OrchardAccountDelivery::ReadCatalogShieldRequestProofForReplay(
        manager,f.Selected().session,{f.Domain(),102,3},*view,orchard::Hash{81},inputs,payments,outputs,10000,jobs,true);};
    sqlite3* db;{auto lease=manager.AcquireDatabaseLease();db=lease->Database();}
    struct Commits{unsigned seen=0;};Commits commits;
    ASSERT_EQ(sqlite3_set_authorizer(db,[](void* p,int action,const char* name,const char*,const char*,const char*){
        auto& c=*static_cast<Commits*>(p);
        if(action==SQLITE_TRANSACTION&&name&&std::string_view(name)=="COMMIT"&&++c.seen==3)return SQLITE_DENY;
        return SQLITE_OK;},&commits),SQLITE_OK);
    EXPECT_THROW((void)call(),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_EQ(commits.seen,3u);EXPECT_FALSE(jobs.Query(orchard::Hash{81}));EXPECT_TRUE(sqlite3_get_autocommit(db));
    auto resumed=call();EXPECT_EQ(resumed.state,std::optional(wallet::OrchardProofJobs::State::Queued));
    EXPECT_EQ(resumed.durable.message,original->message);EXPECT_EQ(resumed.durable.nullifiers,original->nullifiers);
    jobs.Start();ASSERT_EQ(ServiceProofTerminal(jobs,orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
    auto complete=call();ASSERT_TRUE(complete.proof);EXPECT_EQ(complete.proof->Authorization().SigningDigest(),original->message);
}
TEST(OrchardPlanRecovery, MissingLegacyAndAuthenticatedDamagedCapsulesRefuse){
    for(bool legacy:{false,true}){
        OrchardSpendRequestFixture f;wallet::OrchardProofJobs old,missing;auto queued=f.Request(old);
        ASSERT_TRUE(queued->durable);ASSERT_TRUE(queued->durable->recovery);
        const auto queue=f.Account(3).account.Operations().Encode();
        std::vector<uint8_t> replacement;
        if(legacy){auto predecessor=f.BoundPredecessor(*queued->durable,f.Domain(),f.Operation(1));
            replacement.assign(predecessor.Bytes().begin(),predecessor.Bytes().end());}
        else{replacement.assign(queue.Bytes().begin(),queue.Bytes().end());replacement.back()^=1;}
        f.StoreQueueFixture(replacement,false);const auto before=f.Snapshot();
        auto use=WalletService::AcquireWalletUse(f.wallet);const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
        const auto payments=f.Payments();
        EXPECT_THROW((void)wallet::OrchardAccountDelivery::ReadCatalogRequestProofForReplay(
            use->Wallet(),f.Session(),{f.Domain(),102,3},**view,f.Operation(1),payments,{},100000,missing,true),std::runtime_error);
        EXPECT_FALSE(missing.Query(f.Operation(1)));EXPECT_EQ(f.Snapshot(),before);
    }
}
} // namespace dinero
#endif
