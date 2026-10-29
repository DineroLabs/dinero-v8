#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero::wallet {
struct OrchardDetachedProofReadTestAccess {
    using Points=std::function<OrchardAccountDelivery::RestorePoint(RuntimeOutboxCursor)>;
    static auto Any(WalletManager& w,uint64_t session,const OrchardAccountDelivery::Profile& p,
            const RuntimeAccountReplay& replay,const orchard::Hash& id,OrchardProofJobs& jobs,const Points& points){
        return OrchardAccountDelivery::ReadCatalogProofWithRestorePoints(w,session,p,replay,
            OrchardAccountDelivery::CatalogProofRequest::Any,id,{},{},{},0,jobs,points);
    }
    static auto Spend(WalletManager& w,uint64_t session,const OrchardAccountDelivery::Profile& p,
            const RuntimeAccountReplay& replay,const orchard::Hash& id,
            std::span<const orchard::WalletPayment> payments,std::span<const orchard::TransparentOutput> outputs,
            uint64_t fee,OrchardProofJobs& jobs,const Points& points){
        return OrchardAccountDelivery::ReadCatalogProofWithRestorePoints(w,session,p,replay,
            OrchardAccountDelivery::CatalogProofRequest::Spend,id,{},payments,outputs,fee,jobs,points);
    }
    static auto Shield(WalletManager& w,uint64_t session,const OrchardAccountDelivery::Profile& p,
            const RuntimeAccountReplay& replay,const orchard::Hash& id,std::span<const orchard::ResolvedInput> inputs,
            std::span<const orchard::WalletPayment> payments,std::span<const orchard::TransparentOutput> outputs,
            uint64_t fee,OrchardProofJobs& jobs,const Points& points){
        return OrchardAccountDelivery::ReadCatalogProofWithRestorePoints(w,session,p,replay,
            OrchardAccountDelivery::CatalogProofRequest::Shield,id,inputs,payments,outputs,fee,jobs,points);
    }
};
}
namespace dinero {
TEST(OrchardDetachedProofRead, QueuedAndMissingJobsRestoreWithoutOwners){
    OrchardProofOwnerFixture f;wallet::OrchardProofJobs jobs,missing;auto queued=f.Queue(jobs);ASSERT_TRUE(queued->enqueued);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();auto* db=f.Database();
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto before=f.Snapshot();
    size_t points=0,origins=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){
        ++points;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
        auto point=(*view)->Point(cursor);const auto original=point.lookups.origin;
        point.lookups.origin=[&,original](uint32_t height,const uint256& hash,const orchard::Hash& txid){
            ++origins;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
            EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));return original(height,hash,txid);
        };return point;
    };
    for(auto* executor:{&jobs,&missing}){
        DetachedCatalogSqlHooks hooks(db);
        const auto result=wallet::OrchardDetachedProofReadTestAccess::Any(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),*executor,provider);
        EXPECT_EQ(hooks.commits,2u);EXPECT_EQ(result.revision,queued->revision);EXPECT_FALSE(result.proof);
        if(executor==&jobs)EXPECT_EQ(result.state,std::optional(wallet::OrchardProofJobs::State::Queued));
        else EXPECT_FALSE(result.state);
    }
    EXPECT_GT(points,2u);EXPECT_GT(origins,0u);EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(f.Read(jobs).state,std::optional(wallet::OrchardProofJobs::State::Queued));
    EXPECT_EQ(jobs.Query(f.Operation(1)),std::optional(wallet::OrchardProofJobs::State::Queued));
}
TEST(OrchardDetachedProofRead, SpendAndShieldRequestsKeepExactBindings){
    {
        OrchardSpendRequestFixture f;wallet::OrchardProofJobs jobs;const auto queued=f.Request(jobs);ASSERT_TRUE(queued->enqueued);ASSERT_TRUE(queued->durable);
        const auto payments=f.Payments();const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
        auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();const auto before=f.Snapshot();
        size_t points=0;const auto provider=[&](RuntimeOutboxCursor cursor){++points;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));return (*view)->Point(cursor);};
        const auto result=wallet::OrchardDetachedProofReadTestAccess::Spend(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),payments,{},100000,jobs,provider);
        EXPECT_GT(points,2u);EXPECT_EQ(result.revision,queued->revision);EXPECT_EQ(result.durable.request_commitment,queued->durable->request_commitment);
        const auto actual=wallet::OrchardAccountDelivery::ReadCatalogRequestProofForReplay(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),payments,{},100000,jobs);
        EXPECT_EQ(actual.durable.message,result.durable.message);EXPECT_EQ(actual.state,result.state);
        auto changed=payments;changed[0].memo[511]^=1;
        EXPECT_THROW((void)wallet::OrchardDetachedProofReadTestAccess::Spend(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),changed,{},100000,jobs,provider),std::runtime_error);
        EXPECT_THROW((void)wallet::OrchardDetachedProofReadTestAccess::Spend(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),payments,{},100001,jobs,provider),std::runtime_error);
        EXPECT_EQ(f.Snapshot(),before);
    }
    {
        ShieldReservationFixture f;wallet::OrchardProofJobs jobs;const auto queued=f.Queue(jobs);ASSERT_TRUE(queued->enqueued);
        const auto payments=f.Payments();const auto inputs=f.Inputs();const std::vector<orchard::TransparentOutput> outputs{{70000,f.script}};
        const auto view=f.View();auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Selected().session;const auto before=f.Snapshot();
        size_t points=0;const auto provider=[&](RuntimeOutboxCursor cursor){++points;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));return view->Point(cursor);};
        const auto result=wallet::OrchardDetachedProofReadTestAccess::Shield(wallet,session,{f.Domain(),102,3},*view,orchard::Hash{81},inputs,payments,outputs,10000,jobs,provider);
        EXPECT_GT(points,2u);EXPECT_EQ(result.revision,queued->revision);EXPECT_TRUE(result.durable.request_commitment);
        EXPECT_EQ(f.ReadShield(jobs).durable.request_commitment,result.durable.request_commitment);
        auto changed=inputs;changed[0].amount_una+=1;
        EXPECT_THROW((void)wallet::OrchardDetachedProofReadTestAccess::Shield(wallet,session,{f.Domain(),102,3},*view,orchard::Hash{81},changed,payments,outputs,10000,jobs,provider),std::runtime_error);
        EXPECT_THROW((void)wallet::OrchardDetachedProofReadTestAccess::Shield(wallet,session,{f.Domain(),102,3},*view,orchard::Hash{81},inputs,payments,outputs,10001,jobs,provider),std::runtime_error);
        EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(jobs.Query(orchard::Hash{81}),std::optional(wallet::OrchardProofJobs::State::Queued));
    }
}
TEST(OrchardDetachedProofRead, CompletedProofSurvivesCaptureFinalReadAndCommitRefusal){
    OrchardProofOwnerFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto& jobs=use->OrchardProofs();
    const auto queued=f.Queue(jobs,true);ASSERT_TRUE(queued->enqueued);ASSERT_EQ(ServiceProofTerminal(jobs,queued->operation_id),wallet::OrchardProofJobs::State::Succeeded);
    const auto before=f.Snapshot();const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto session=f.Session();auto* db=f.Database();
    const auto initial=f.Read(jobs);ASSERT_TRUE(initial.proof);const auto exact=initial.proof->Bytes();size_t points=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){++points;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));return (*view)->Point(cursor);};
    const auto call=[&]{return wallet::OrchardDetachedProofReadTestAccess::Any(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),jobs,provider);};
    bool refused=false;ASSERT_EQ(sqlite3_set_authorizer(db,[](void* data,int action,const char* name,const char*,const char*,const char*){
        if(action==SQLITE_TRANSACTION&&name&&std::string_view(name)=="COMMIT"){*static_cast<bool*>(data)=true;return SQLITE_DENY;}return SQLITE_OK;},&refused),SQLITE_OK);
    EXPECT_THROW((void)call(),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(refused);EXPECT_EQ(points,0u);
    {DetachedCatalogSqlHooks hooks(db);hooks.deny_final_read=true;EXPECT_THROW((void)call(),std::runtime_error);EXPECT_EQ(hooks.commits,1u);}
    {DetachedCatalogSqlHooks hooks(db);hooks.refuse_final_commit=true;EXPECT_THROW((void)call(),std::runtime_error);EXPECT_EQ(hooks.commits,2u);}
    const auto retry=call();ASSERT_TRUE(retry.proof);EXPECT_EQ(retry.proof->Bytes(),exact);EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(jobs.Query(queued->operation_id),std::optional(wallet::OrchardProofJobs::State::Succeeded));EXPECT_TRUE(sqlite3_get_autocommit(db));
}
TEST(OrchardDetachedProofRead, OtherAccountChangeLockAndCallerOwnershipRefuseWithoutJobEffects){
    OrchardProofOwnerFixture f;wallet::OrchardProofJobs jobs;const auto queued=f.Queue(jobs);ASSERT_TRUE(queued->enqueued);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();auto* db=f.Database();
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto requested=f.Account(3);bool changed=false;std::string changed_bytes;
    const auto change=[&](RuntimeOutboxCursor cursor){
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
        if(!changed){changed=true;(void)wallet::OrchardAccountDelivery::IssueCatalogReceiverForReplay(wallet,session,{f.Domain(),102,17},**view,orchard::WalletScope::External);changed_bytes=f.Snapshot();}
        return (*view)->Point(cursor);
    };
    EXPECT_THROW((void)wallet::OrchardDetachedProofReadTestAccess::Any(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),jobs,change),wallet::OrchardAccountDelivery::CatalogChanged);
    EXPECT_TRUE(changed);EXPECT_EQ(f.Snapshot(),changed_bytes);EXPECT_EQ(f.Account(3).revision,requested.revision);
    EXPECT_EQ(OrchardCreationBytes(f.Account(3).account),OrchardCreationBytes(requested.account));
    size_t points=0;const auto provider=[&](RuntimeOutboxCursor cursor){++points;return (*view)->Point(cursor);};
    const auto call=[&]{return wallet::OrchardDetachedProofReadTestAccess::Any(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),jobs,provider);};
    {auto lease=wallet.AcquireDatabaseLease();EXPECT_THROW((void)call(),std::runtime_error);EXPECT_TRUE(sqlite3_get_autocommit(db));}
    ASSERT_EQ(sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr),SQLITE_OK);EXPECT_THROW((void)call(),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(db));ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);EXPECT_EQ(points,0u);
    bool locked=false;const auto lock=[&](RuntimeOutboxCursor cursor){EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));if(!locked){locked=true;wallet.lockWallet();}return (*view)->Point(cursor);};
    bool denied=false;try{(void)wallet::OrchardDetachedProofReadTestAccess::Any(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),jobs,lock);}
    catch(const wallet::OrchardAccountDelivery::CatalogChanged&){ADD_FAILURE()<<"Wallet lock mislabeled as catalog change";}
    catch(const std::runtime_error&){denied=true;}
    EXPECT_TRUE(locked);EXPECT_TRUE(denied);wallet.unlockWallet("canonical-fixture-pass",0);
    EXPECT_EQ(call().state,std::optional(wallet::OrchardProofJobs::State::Queued));EXPECT_EQ(f.Snapshot(),changed_bytes);
    EXPECT_EQ(jobs.Query(f.Operation(1)),std::optional(wallet::OrchardProofJobs::State::Queued));
}
TEST(OrchardDetachedProofRead, DifferentManagerAndSourceRefusalPreserveQueuedOwner){
    OrchardProofOwnerFixture f;wallet::OrchardProofJobs jobs;const auto queued=f.Queue(jobs);ASSERT_TRUE(queued->enqueued);
    const auto before=f.Snapshot();const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();size_t points=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){++points;return (*view)->Point(cursor);};
    {
        WalletManager twin(f.f.path/"wallet-runtime",f.f.logger.get());twin.open("canonical-recovery");twin.unlockWallet("canonical-fixture-pass",0);
        uint64_t other_session;{auto lease=twin.AcquireDatabaseLease();other_session=lease->Session();}
        EXPECT_THROW((void)wallet::OrchardDetachedProofReadTestAccess::Any(twin,other_session,{f.Domain(),102,3},**view,f.Operation(1),jobs,provider),std::runtime_error);
        EXPECT_GT(points,0u);EXPECT_TRUE(WalletDetachedReadTestAccess::Released(twin));
    }
    const auto refused=[&](RuntimeOutboxCursor)->wallet::OrchardAccountDelivery::RestorePoint{EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));throw std::runtime_error("Isolated source refusal");};
    EXPECT_THROW((void)wallet::OrchardDetachedProofReadTestAccess::Any(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),jobs,refused),std::runtime_error);
    EXPECT_EQ(f.Read(jobs).state,std::optional(wallet::OrchardProofJobs::State::Queued));EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(jobs.Query(f.Operation(1)),std::optional(wallet::OrchardProofJobs::State::Queued));
}
} // namespace dinero
#endif
