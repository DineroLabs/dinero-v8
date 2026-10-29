#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero::wallet {
struct OrchardDetachedFinalizationTestAccess {
    using Points=std::function<OrchardAccountDelivery::RestorePoint(RuntimeOutboxCursor)>;
    static auto Prepare(WalletManager& w,uint64_t session,const OrchardAccountDelivery::Profile& p,
            const RuntimeAccountReplay& view,const orchard::Hash& id,OrchardProofJobs* jobs,
            const Points& points,std::optional<uint64_t> expected={}){
        return OrchardAccountDelivery::PrepareFinalizationWithRestorePoints(w,session,p,view,id,expected,jobs,false,
            {},{},{},0,points);
    }
    static auto Shield(WalletManager& w,uint64_t session,const OrchardAccountDelivery::Profile& p,
            const RuntimeAccountReplay& view,const orchard::Hash& id,OrchardProofJobs& jobs,
            std::span<const orchard::ResolvedInput> inputs,std::span<const orchard::WalletPayment> payments,
            std::span<const orchard::TransparentOutput> outputs,uint64_t fee,const Points& points){
        return OrchardAccountDelivery::PrepareFinalizationWithRestorePoints(w,session,p,view,id,{},&jobs,true,
            inputs,payments,outputs,fee,points);
    }
    static auto Commit(WalletManager& w,uint64_t session,std::unique_ptr<OrchardCatalogFinalizationPlan> plan,
            const consensus::VerifiedOrchardAuthorizations& authorization){
        return OrchardAccountDelivery::CommitCatalogFinalization(w,session,std::move(plan),authorization);
    }
};
}
namespace dinero {
TEST(OrchardDetachedFinalization, RestoresWithoutOwnersAndCommitsBeforeRetiringExactProof){
    OrchardProofFinalizationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto& jobs=use->OrchardProofs();
    const auto request=f.Queue(jobs,true);ASSERT_TRUE(request->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,request->operation_id),wallet::OrchardProofJobs::State::Succeeded);
    const auto proof=f.Read(jobs);ASSERT_TRUE(proof.proof);const auto authorization=f.Authorization(*request,*proof.proof);
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto session=f.Session();auto* db=f.Database();const auto before=f.Snapshot();
    size_t points=0,origins=0;const auto provider=[&](RuntimeOutboxCursor cursor){
        ++points;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));auto point=(*view)->Point(cursor);const auto original=point.lookups.origin;
        point.lookups.origin=[&,original](uint32_t height,const uint256& hash,const orchard::Hash& txid){
            ++origins;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
            EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));return original(height,hash,txid);};return point;
    };
    auto plan=wallet::OrchardDetachedFinalizationTestAccess::Prepare(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),&jobs,provider);
    EXPECT_GT(points,2u);EXPECT_GT(origins,0u);EXPECT_EQ(f.Snapshot(),before);const auto restored_points=points;
    struct Observe{wallet::OrchardProofJobs& jobs;orchard::Hash id;bool commit=false,retained=false;} observed{jobs,f.Operation(1)};
    sqlite3_commit_hook(db,[](void* data){auto& o=*static_cast<Observe*>(data);o.commit=true;
        o.retained=o.jobs.Query(o.id)==wallet::OrchardProofJobs::State::Succeeded;return 0;},&observed);
    const auto result=wallet::OrchardDetachedFinalizationTestAccess::Commit(wallet,session,std::move(plan),authorization);
    sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_FALSE(plan);EXPECT_TRUE(observed.commit);EXPECT_TRUE(observed.retained);
    EXPECT_EQ(points,restored_points);EXPECT_TRUE(result.retired_job);EXPECT_FALSE(jobs.Query(f.Operation(1)));
    EXPECT_EQ(result.state.account.Operations().Entries().at(f.Operation(1)).transaction,authorization.Orchard().CanonicalBytes());
    const auto ready=f.Snapshot();const auto retry=f.Finish(jobs,authorization);EXPECT_EQ(retry.state.revision,result.state.revision);
    EXPECT_FALSE(retry.retired_job);EXPECT_EQ(f.Snapshot(),ready);
    wallet.open("canonical-recovery");wallet.unlockWallet("canonical-fixture-pass",0);
    EXPECT_EQ(f.Finish(jobs,authorization).state.revision,result.state.revision);EXPECT_EQ(f.Snapshot(),ready);
}
TEST(OrchardDetachedFinalization, CaptureReadWriteAndCommitRefusalsRetainReservationAndProof){
    OrchardProofFinalizationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto& jobs=use->OrchardProofs();
    const auto request=f.Queue(jobs,true);ASSERT_TRUE(request->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,request->operation_id),wallet::OrchardProofJobs::State::Succeeded);
    const auto proof=f.Read(jobs);ASSERT_TRUE(proof.proof);const auto exact=proof.proof->Bytes();const auto authorization=f.Authorization(*request,*proof.proof);
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto session=f.Session();auto* db=f.Database();const auto before=f.Snapshot();
    size_t points=0;const auto provider=[&](RuntimeOutboxCursor cursor){++points;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));return (*view)->Point(cursor);};
    const auto prepare=[&]{return wallet::OrchardDetachedFinalizationTestAccess::Prepare(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),&jobs,provider);};
    bool denied=false;sqlite3_set_authorizer(db,[](void* p,int action,const char* name,const char*,const char*,const char*){
        if(action==SQLITE_TRANSACTION&&name&&std::string_view(name)=="COMMIT"){*static_cast<bool*>(p)=true;return SQLITE_DENY;}return SQLITE_OK;},&denied);
    EXPECT_THROW((void)prepare(),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(denied);EXPECT_EQ(points,0u);
    for(int fault:{0,1}){
        DetachedCatalogSqlHooks hooks(db);auto plan=prepare();EXPECT_EQ(hooks.commits,1u);
        hooks.deny_final_read=fault==0;hooks.refuse_final_commit=fault==1;
        EXPECT_THROW((void)wallet::OrchardDetachedFinalizationTestAccess::Commit(wallet,session,std::move(plan),authorization),std::runtime_error);
        EXPECT_FALSE(plan);EXPECT_EQ(hooks.commits,fault==0?1u:2u);
    }
    f.Sql("CREATE TRIGGER detached_ready_refusal BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'isolated Ready refusal'); END");
    EXPECT_THROW((void)f.Finish(jobs,authorization),std::runtime_error);f.Sql("DROP TRIGGER detached_ready_refusal");
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.Read(jobs).proof->Bytes(),exact);
    EXPECT_EQ(jobs.Query(f.Operation(1)),std::optional(wallet::OrchardProofJobs::State::Succeeded));
    EXPECT_TRUE(f.Finish(jobs,authorization).retired_job);
}
TEST(OrchardDetachedFinalization, WholeCatalogChangeAndLateAuthenticationFailureRollBackReady){
    OrchardProofFinalizationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto& jobs=use->OrchardProofs();
    const auto request=f.Queue(jobs,true);ASSERT_TRUE(request->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,request->operation_id),wallet::OrchardProofJobs::State::Succeeded);
    const auto proof=f.Read(jobs);ASSERT_TRUE(proof.proof);const auto authorization=f.Authorization(*request,*proof.proof);
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto session=f.Session();
    const auto provider=[&](RuntimeOutboxCursor cursor){EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));return (*view)->Point(cursor);};
    const auto original=f.Account(3);auto plan=wallet::OrchardDetachedFinalizationTestAccess::Prepare(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),&jobs,provider);
    (void)wallet::OrchardAccountDelivery::IssueCatalogReceiverForReplay(wallet,session,{f.Domain(),102,17},**view,orchard::WalletScope::External);
    const auto changed=f.Snapshot();
    EXPECT_THROW((void)wallet::OrchardDetachedFinalizationTestAccess::Commit(wallet,session,std::move(plan),authorization),wallet::OrchardAccountDelivery::CatalogChanged);
    EXPECT_FALSE(plan);EXPECT_EQ(f.Snapshot(),changed);EXPECT_EQ(f.Account(3).revision,original.revision);
    f.Sql("CREATE TRIGGER detached_late_corruption AFTER UPDATE ON orchard_wallet_snapshots WHEN NEW.account=3 BEGIN UPDATE orchard_wallet_snapshots SET sealed=zeroblob(length(sealed)) WHERE account=17; END");
    EXPECT_THROW((void)f.Finish(jobs,authorization),std::runtime_error);f.Sql("DROP TRIGGER detached_late_corruption");
    EXPECT_EQ(f.Snapshot(),changed);EXPECT_EQ(f.Read(jobs).proof->Bytes(),proof.proof->Bytes());
    EXPECT_TRUE(f.Finish(jobs,authorization).retired_job);
}
TEST(OrchardDetachedFinalization, CallerOwnershipForeignManagerAndSourceRefusalPreserveProof){
    OrchardProofFinalizationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto& jobs=use->OrchardProofs();
    const auto request=f.Queue(jobs,true);ASSERT_TRUE(request->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,request->operation_id),wallet::OrchardProofJobs::State::Succeeded);
    const auto proof=f.Read(jobs);ASSERT_TRUE(proof.proof);const auto authorization=f.Authorization(*request,*proof.proof);
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto session=f.Session();auto* db=f.Database();const auto before=f.Snapshot();
    size_t points=0;const auto provider=[&](RuntimeOutboxCursor cursor){++points;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));return (*view)->Point(cursor);};
    const auto prepare=[&]{return wallet::OrchardDetachedFinalizationTestAccess::Prepare(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),&jobs,provider);};
    {auto lease=wallet.AcquireDatabaseLease();EXPECT_THROW((void)prepare(),std::runtime_error);}
    ASSERT_EQ(sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr),SQLITE_OK);EXPECT_THROW((void)prepare(),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(db));ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);EXPECT_EQ(points,0u);
    auto plan=prepare();wallet.lockWallet();
    EXPECT_THROW((void)wallet::OrchardDetachedFinalizationTestAccess::Commit(wallet,session,std::move(plan),authorization),std::runtime_error);
    wallet.unlockWallet("canonical-fixture-pass",0);EXPECT_FALSE(plan);
    plan=prepare();{auto lease=wallet.AcquireDatabaseLease();
        EXPECT_THROW((void)wallet::OrchardDetachedFinalizationTestAccess::Commit(wallet,session,std::move(plan),authorization),std::runtime_error);}
    plan=prepare();ASSERT_EQ(sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW((void)wallet::OrchardDetachedFinalizationTestAccess::Commit(wallet,session,std::move(plan),authorization),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(db));ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    plan=prepare();{
        WalletManager twin(f.f.path/"wallet-runtime",f.f.logger.get());twin.open("canonical-recovery");twin.unlockWallet("canonical-fixture-pass",0);
        uint64_t other;{auto lease=twin.AcquireDatabaseLease();other=lease->Session();}
        EXPECT_THROW((void)wallet::OrchardDetachedFinalizationTestAccess::Commit(twin,other,std::move(plan),authorization),std::runtime_error);
    }
    const auto refused=[&](RuntimeOutboxCursor)->wallet::OrchardAccountDelivery::RestorePoint{
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));throw std::runtime_error("isolated unavailable source");};
    EXPECT_THROW((void)wallet::OrchardDetachedFinalizationTestAccess::Prepare(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),&jobs,refused),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.Read(jobs).proof->Bytes(),proof.proof->Bytes());
    EXPECT_TRUE(f.Finish(jobs,authorization).retired_job);
}
TEST(OrchardDetachedFinalization, PreviouslyCollectedResultCannotPromoteReserved){
    OrchardProofFinalizationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto& jobs=use->OrchardProofs();
    const auto request=f.Queue(jobs,true);ASSERT_TRUE(request->enqueued);
    ASSERT_EQ(ServiceProofTerminal(jobs,request->operation_id),wallet::OrchardProofJobs::State::Succeeded);
    const auto proof=f.Read(jobs);ASSERT_TRUE(proof.proof);const auto authorization=f.Authorization(*request,*proof.proof);
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto session=f.Session();const auto before=f.Snapshot();
    auto plan=wallet::OrchardDetachedFinalizationTestAccess::Prepare(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),&jobs,
        [&](RuntimeOutboxCursor cursor){return (*view)->Point(cursor);});
    auto collected=jobs.TakeResult(f.Operation(1));ASSERT_TRUE(collected);EXPECT_EQ(collected->Bytes(),proof.proof->Bytes());
    EXPECT_THROW((void)wallet::OrchardDetachedFinalizationTestAccess::Commit(wallet,session,std::move(plan),authorization),std::runtime_error);
    EXPECT_FALSE(plan);EXPECT_FALSE(jobs.Query(f.Operation(1)));EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(f.Account(3).account.Operations().Entries().at(f.Operation(1)).phase,wallet::OrchardOperationQueue::Phase::Reserved);
}
TEST(OrchardDetachedFinalization, DirectReadyPreservesExpectedRevisionAndIdenticalRetry){
    OrchardSpendOwnerFixture f;auto request=f.Reserve();const auto authorization=f.Prove(request);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto before=f.Snapshot();size_t points=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){++points;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));return (*view)->Point(cursor);};
    EXPECT_THROW((void)wallet::OrchardDetachedFinalizationTestAccess::Prepare(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),nullptr,provider,request.revision+1),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);
    auto plan=wallet::OrchardDetachedFinalizationTestAccess::Prepare(wallet,session,{f.Domain(),102,3},**view,f.Operation(1),nullptr,provider,request.revision);
    EXPECT_GT(points,2u);const auto result=wallet::OrchardDetachedFinalizationTestAccess::Commit(wallet,session,std::move(plan),authorization);
    EXPECT_FALSE(result.retired_job);EXPECT_EQ(result.state.revision,request.revision+1);const auto ready=f.Snapshot();
    EXPECT_EQ(f.Ready(1,result.state.revision,authorization).revision,result.state.revision);EXPECT_EQ(f.Snapshot(),ready);
}
TEST(OrchardDetachedFinalization, ShieldPreparationAndSelectedHistoryCommitKeepOriginalOwners){
    ShieldHistoryFixture f;f.Complete();auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto& jobs=use->OrchardProofs();
    const auto view=f.View();const auto session=f.Selected().session;const auto before=f.Snapshot();const auto proof=f.ReadShield(jobs);ASSERT_TRUE(proof.proof);
    const auto payments=f.Payments();const auto inputs=f.Inputs();const std::vector<orchard::TransparentOutput> outputs{{70000,f.script}};size_t points=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){++points;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
        EXPECT_TRUE(sqlite3_get_autocommit(wallet.getCurrentDatabase()));EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));return view->Point(cursor);};
    {auto prepared=wallet::OrchardDetachedFinalizationTestAccess::Shield(wallet,session,{f.Domain(),102,3},*view,orchard::Hash{81},jobs,inputs,payments,outputs,10000,provider);EXPECT_TRUE(prepared);}
    EXPECT_GT(points,2u);EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.ReadShield(jobs).proof->Bytes(),proof.proof->Bytes());
    f.Sql("CREATE TRIGGER detached_shield_history_refusal BEFORE INSERT ON transactions WHEN NEW.category='shield' BEGIN SELECT RAISE(ABORT,'isolated shield history refusal'); END");
    EXPECT_THROW((void)f.Finish(jobs),std::runtime_error);f.Sql("DROP TRIGGER detached_shield_history_refusal");
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.ReadShield(jobs).proof->Bytes(),proof.proof->Bytes());
    struct Observe{ChainstateService& source;wallet::OrchardProofJobs& jobs;bool write=false,selected=false,retained=false;int commits=0;} observed{*f.f.service,jobs};
    auto* db=wallet.getCurrentDatabase();sqlite3_set_authorizer(db,[](void* p,int action,const char* table,const char*,const char*,const char*){
        if(action==SQLITE_UPDATE&&table&&std::string_view(table)=="orchard_wallet_snapshots")static_cast<Observe*>(p)->write=true;return SQLITE_OK;},&observed);
    sqlite3_commit_hook(db,[](void* p){auto& o=*static_cast<Observe*>(p);if(!o.write)return 0;++o.commits;
        o.selected=ShieldedStateStartupTestAccess::VaultSelectedHeld(o.source);o.retained=o.jobs.Query(orchard::Hash{81})==wallet::OrchardProofJobs::State::Succeeded;return 0;},&observed);
    const auto body=f.Finish(jobs);sqlite3_commit_hook(db,nullptr,nullptr);sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_EQ(observed.commits,1);EXPECT_TRUE(observed.selected);EXPECT_TRUE(observed.retained);EXPECT_FALSE(jobs.Query(orchard::Hash{81}));
    const auto row=f.History(f.Txid(body));const auto ready=f.Snapshot();const auto entry=f.Account(3).account.Operations().Entries().at(orchard::Hash{81});
    ASSERT_TRUE(entry.shield_ready_time);EXPECT_EQ(std::stoull(row[4]),*entry.shield_ready_time);
    EXPECT_EQ(f.Finish(jobs),body);EXPECT_EQ(f.Snapshot(),ready);EXPECT_EQ(f.History(f.Txid(body)),row);
}
} // namespace dinero
#endif
