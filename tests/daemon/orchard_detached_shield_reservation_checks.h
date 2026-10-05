#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero::wallet {
struct OrchardDetachedShieldReservationTestAccess {
    using Mode=OrchardAccountDelivery::ShieldRequestMode;
    using Points=std::function<OrchardAccountDelivery::RestorePoint(RuntimeOutboxCursor)>;
    static auto Prepare(WalletManager& wallet,uint64_t session,const OrchardAccountDelivery::Profile& profile,
            uint64_t revision,const RuntimeAccountReplay& view,const orchard::Hash& id,
            std::span<const orchard::ResolvedInput> inputs,std::span<const orchard::WalletPayment> payments,
            std::span<const orchard::TransparentOutput> outputs,uint64_t fee,Mode mode,bool existing_only,const Points& points){
        return OrchardAccountDelivery::PrepareShieldReservationWithPoints(wallet,session,profile,revision,view,id,
            inputs,payments,outputs,fee,mode,existing_only,points);
    }
    static auto Retry(WalletManager& wallet,uint64_t session,OrchardCatalogRecoveryPlan& plan){
        return OrchardAccountDelivery::TakePreparedShieldRetry(wallet,session,plan);
    }
    static auto Candidates(WalletManager& wallet,uint64_t session,const OrchardAccountDelivery::Profile& profile,
            uint64_t revision,const OrchardCatalogRecoveryPlan& plan){
        return OrchardAccountDelivery::ReadPreparedShieldCandidates(wallet,session,profile,revision,plan);
    }
    static auto Commit(WalletManager& wallet,uint64_t session,std::unique_ptr<OrchardCatalogRecoveryPlan> plan,
            std::span<const orchard::ResolvedInput> inputs,std::span<const orchard::TransparentOutput> outputs,
            OrchardProofJobs& jobs){
        return OrchardAccountDelivery::CommitShieldReservation(wallet,session,std::move(plan),inputs,outputs,&jobs).queued;
    }
};
}
namespace dinero {
// Test-only actual-write observation also works for WITHOUT ROWID snapshots.
namespace {
struct DetachedShieldSnapshotWriteObserver {
    sqlite3* db;
    DetachedShieldSnapshotWriteObserver(sqlite3* database,void* state,void (*callback)(sqlite3_context*,int,sqlite3_value**)):db(database){
        OrchardAdmissionFixture::Require(sqlite3_create_function_v2(db,"observe_detached_shield_snapshot_write",0,SQLITE_UTF8,state,
            callback,nullptr,nullptr,nullptr)==SQLITE_OK);
        if(sqlite3_exec(db,"CREATE TEMP TRIGGER observe_detached_shield_snapshot_write AFTER UPDATE ON main.orchard_wallet_snapshots BEGIN SELECT observe_detached_shield_snapshot_write(); END",nullptr,nullptr,nullptr)!=SQLITE_OK){
            (void)Close();throw std::runtime_error("Snapshot write observer trigger failed");
        }
    }
    DetachedShieldSnapshotWriteObserver(const DetachedShieldSnapshotWriteObserver&)=delete;
    DetachedShieldSnapshotWriteObserver& operator=(const DetachedShieldSnapshotWriteObserver&)=delete;
    ~DetachedShieldSnapshotWriteObserver(){(void)Close();}
    bool Close() noexcept {
        if(!db)return true;
        const int drop=sqlite3_exec(db,"DROP TRIGGER IF EXISTS temp.observe_detached_shield_snapshot_write",nullptr,nullptr,nullptr);
        const int unregister=sqlite3_create_function_v2(db,"observe_detached_shield_snapshot_write",0,SQLITE_UTF8,nullptr,nullptr,nullptr,nullptr,nullptr);
        db=nullptr;return drop==SQLITE_OK&&unregister==SQLITE_OK;
    }
};
} // namespace
namespace {
using DetachedShield=wallet::OrchardDetachedShieldReservationTestAccess;
}
TEST(OrchardDetachedShieldReservation, RestorationReleasedSelectedCommitThenExplicitPublication){
    ShieldReservationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    wallet::OrchardProofJobs jobs;const auto view=f.View();const auto session=f.Selected().session;
    const auto revision=f.Account(3).revision;const auto inputs=f.Inputs();const auto payments=f.Payments();
    const std::vector<orchard::TransparentOutput> outputs{{70000,f.script}};
    const auto before=f.Snapshot();auto* db=wallet.getCurrentDatabase();size_t points=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){++points;
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));return view->Point(cursor);
    };
    auto plan=DetachedShield::Prepare(wallet,session,{f.Domain(),102,3},revision,*view,orchard::Hash{81},
        inputs,payments,outputs,10000,DetachedShield::Mode::Exact,false,provider);
    ASSERT_TRUE(plan);EXPECT_GT(points,0u);EXPECT_EQ(f.Snapshot(),before);const auto prepared_points=points;
    struct Commit {ChainstateService& source;wallet::OrchardProofJobs& jobs;size_t seen=0;bool selected=false,hidden=false;} observed{*f.f.service,jobs};
    sqlite3_commit_hook(db,[](void* p){auto& o=*static_cast<Commit*>(p);++o.seen;
        o.selected=ShieldedStateStartupTestAccess::VaultSelectedHeld(o.source);o.hidden=!o.jobs.Query(orchard::Hash{81});return 0;},&observed);
    wallet::OrchardShieldRequest pending;
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        pending=DetachedShield::Commit(wallet,session,std::move(plan),inputs,outputs,jobs);
        ASSERT_TRUE(pending.result);EXPECT_FALSE(pending.result->enqueued);EXPECT_FALSE(jobs.Query(orchard::Hash{81}));
    }
    sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_EQ(observed.seen,1u);EXPECT_TRUE(observed.selected);EXPECT_TRUE(observed.hidden);EXPECT_EQ(points,prepared_points);
    EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
    const auto committed=f.Snapshot();auto result=std::move(pending).Publish();ASSERT_TRUE(result);EXPECT_TRUE(result->enqueued);
    EXPECT_EQ(jobs.Query(orchard::Hash{81}),std::optional(wallet::OrchardProofJobs::State::Queued));
    EXPECT_FALSE(f.Pay().success);EXPECT_EQ(f.Snapshot(),committed);f.Reopen();EXPECT_EQ(f.Snapshot(),committed);
}
TEST(OrchardDetachedShieldReservation, CaptureWriterAndCommitRefusalsPreserveAllOwners){
    ShieldReservationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    wallet::OrchardProofJobs jobs;const auto view=f.View();const auto session=f.Selected().session;
    const auto revision=f.Account(3).revision;const auto inputs=f.Inputs();const auto payments=f.Payments();
    const std::vector<orchard::TransparentOutput> outputs{{70000,f.script}};
    const auto before=f.Snapshot();auto* db=wallet.getCurrentDatabase();size_t calls=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){++calls;
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));return view->Point(cursor);
    };
    const auto prepare=[&]{return DetachedShield::Prepare(wallet,session,{f.Domain(),102,3},revision,*view,orchard::Hash{81},
        inputs,payments,outputs,10000,DetachedShield::Mode::Exact,false,provider);};
    bool capture_commit=false;
    sqlite3_set_authorizer(db,[](void* p,int action,const char* name,const char*,const char*,const char*){
        if(action==SQLITE_TRANSACTION&&name&&std::string_view(name)=="COMMIT"){*static_cast<bool*>(p)=true;return SQLITE_DENY;}return SQLITE_OK;
    },&capture_commit);
    EXPECT_THROW((void)prepare(),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_TRUE(capture_commit);EXPECT_EQ(calls,0u);EXPECT_EQ(f.Snapshot(),before);
    for(int fault:{0,1,2,3}){
        auto plan=prepare();const auto prepared_calls=calls;
        struct Fault {int kind;bool updated=false,denied=false,commit=false;} observed{fault};
        sqlite3_set_authorizer(db,[](void* p,int action,const char* table,const char*,const char*,const char*){
            auto& o=*static_cast<Fault*>(p);
            if(action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_retained"&&
                (o.kind==0||(o.kind==3&&o.updated))){o.denied=true;return SQLITE_DENY;}return SQLITE_OK;
        },&observed);
        DetachedShieldSnapshotWriteObserver observer(db,&observed,[](sqlite3_context* context,int,sqlite3_value**){
            static_cast<Fault*>(sqlite3_user_data(context))->updated=true;
            sqlite3_result_null(context);
        });
        if(fault==1)f.Sql("CREATE TRIGGER refuse_detached_shield BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'shield refusal'); END");
        if(fault==2)sqlite3_commit_hook(db,[](void* p){static_cast<Fault*>(p)->commit=true;return 1;},&observed);
        {
            auto selected=f.f.service->AcquireBlockIngressActivationLock();
            EXPECT_THROW((void)DetachedShield::Commit(wallet,session,std::move(plan),inputs,outputs,jobs),std::runtime_error);
        }
        sqlite3_set_authorizer(db,nullptr,nullptr);sqlite3_commit_hook(db,nullptr,nullptr);ASSERT_TRUE(observer.Close());
        if(fault==1)f.Sql("DROP TRIGGER refuse_detached_shield");
        else if(fault==2){EXPECT_TRUE(observed.updated);EXPECT_TRUE(observed.commit);}
        else EXPECT_TRUE(observed.denied);
        if(fault==3)EXPECT_TRUE(observed.updated);
        EXPECT_EQ(calls,prepared_calls);EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Snapshot(),before);
        EXPECT_FALSE(jobs.Query(orchard::Hash{81}));
    }
    EXPECT_TRUE(f.Queue(jobs)->enqueued);
}
TEST(OrchardDetachedShieldReservation, ChangedCatalogOrdinaryReservationAndRequestInputsRefuse){
    for(int change:{0,1,2}){
        ShieldReservationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
        wallet::OrchardProofJobs jobs;const auto view=f.View();const auto session=f.Selected().session;
        const auto revision=f.Account(3).revision;const auto inputs=f.Inputs();const auto payments=f.Payments();
        const std::vector<orchard::TransparentOutput> outputs{{70000,f.script}};
        auto plan=DetachedShield::Prepare(wallet,session,{f.Domain(),102,3},revision,*view,orchard::Hash{81},inputs,payments,
            outputs,10000,DetachedShield::Mode::Exact,false,[&](RuntimeOutboxCursor c){return view->Point(c);});
        if(change==0)(void)wallet::OrchardAccountDelivery::IssueCatalogReceiverForReplay(wallet,session,{f.Domain(),102,17},*view,orchard::WalletScope::External);
        if(change==1)ASSERT_TRUE(f.Pay().success);
        auto incoming=inputs;if(change==2)++incoming[0].amount_una;const auto baseline=f.Snapshot();
        {
            auto selected=f.f.service->AcquireBlockIngressActivationLock();
            EXPECT_THROW((void)DetachedShield::Commit(wallet,session,std::move(plan),incoming,outputs,jobs),std::runtime_error);
        }
        EXPECT_EQ(f.Snapshot(),baseline);EXPECT_FALSE(jobs.Query(orchard::Hash{81}));
        EXPECT_TRUE(f.Account(3).account.Operations().Entries().empty());
        if(change!=1)EXPECT_TRUE(f.Queue(jobs)->enqueued);
    }
}
TEST(OrchardDetachedShieldReservation, BorrowedOwnershipLockedWalletAndSourceFailureRefuse){
    ShieldReservationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    wallet::OrchardProofJobs jobs;const auto view=f.View();const auto session=f.Selected().session;
    const auto revision=f.Account(3).revision;const auto inputs=f.Inputs();const auto payments=f.Payments();
    const std::vector<orchard::TransparentOutput> outputs{{70000,f.script}};auto* db=wallet.getCurrentDatabase();
    const auto before=f.Snapshot();size_t calls=0;
    const auto provider=[&](RuntimeOutboxCursor c){++calls;return view->Point(c);};
    const auto prepare=[&]{return DetachedShield::Prepare(wallet,session,{f.Domain(),102,3},revision,*view,orchard::Hash{81},
        inputs,payments,outputs,10000,DetachedShield::Mode::Exact,false,provider);};
    {
        auto lease=wallet.AcquireDatabaseLease();EXPECT_THROW((void)prepare(),std::runtime_error);
        ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_THROW((void)prepare(),std::runtime_error);EXPECT_FALSE(sqlite3_get_autocommit(db));
        ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    }
    EXPECT_EQ(calls,0u);wallet.lockWallet();EXPECT_THROW((void)prepare(),std::runtime_error);
    wallet.unlockWallet("canonical-fixture-pass",0);
    const auto unavailable=[&](RuntimeOutboxCursor)->wallet::OrchardAccountDelivery::RestorePoint{
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));throw std::runtime_error("source refused");
    };
    EXPECT_THROW((void)DetachedShield::Prepare(wallet,session,{f.Domain(),102,3},revision,*view,orchard::Hash{81},inputs,payments,
        outputs,10000,DetachedShield::Mode::Exact,false,unavailable),std::runtime_error);
    auto plan=prepare();
    {auto lease=wallet.AcquireDatabaseLease();
        EXPECT_THROW((void)DetachedShield::Commit(wallet,session,std::move(plan),inputs,outputs,jobs),std::runtime_error);}
    EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(jobs.Query(orchard::Hash{81}));EXPECT_TRUE(f.Queue(jobs)->enqueued);
}
TEST(OrchardDetachedShieldReservation, StoredRetryAndCandidateReadUseRestoredCaptureWithoutCallbacks){
    ShieldReservationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    wallet::OrchardProofJobs jobs;const auto view=f.View();const auto session=f.Selected().session;
    const auto revision=f.Account(3).revision;const auto payments=f.Payments();auto* db=wallet.getCurrentDatabase();size_t points=0;
    const auto provider=[&](RuntimeOutboxCursor c){++points;
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));return view->Point(c);
    };
    auto fresh=DetachedShield::Prepare(wallet,session,{f.Domain(),102,3},revision,*view,orchard::Hash{81},{},payments,{},10000,
        DetachedShield::Mode::Stored,false,provider);const auto prepared_points=points;
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        EXPECT_FALSE(DetachedShield::Retry(wallet,session,*fresh));
        const auto candidates=DetachedShield::Candidates(wallet,session,{f.Domain(),102,3},revision,*fresh);
        ASSERT_FALSE(candidates.empty());EXPECT_EQ(points,prepared_points);
        EXPECT_TRUE(std::any_of(candidates.begin(),candidates.end(),[&](const auto& c){
            return c.txid_wire==f.Inputs()[0].txid_wire&&c.output_index==f.Inputs()[0].output_index;}));
    }
    fresh.reset();auto first=f.Queue(jobs);ASSERT_TRUE(first->durable);const auto baseline=f.Snapshot();
    auto retry=DetachedShield::Prepare(wallet,session,{f.Domain(),102,3},revision,*view,orchard::Hash{81},{},payments,{},10000,
        DetachedShield::Mode::Stored,false,provider);const auto retry_points=points;
    bool wrote=false;sqlite3_set_authorizer(db,[](void* p,int action,const char*,const char*,const char*,const char*){
        if(action==SQLITE_INSERT||action==SQLITE_UPDATE||action==SQLITE_DELETE){*static_cast<bool*>(p)=true;return SQLITE_DENY;}return SQLITE_OK;
    },&wrote);
    std::unique_ptr<wallet::OrchardQueuedSpend> result;
    {auto selected=f.f.service->AcquireBlockIngressActivationLock();result=DetachedShield::Retry(wallet,session,*retry);}
    sqlite3_set_authorizer(db,nullptr,nullptr);
    ASSERT_TRUE(result);ASSERT_TRUE(result->durable);EXPECT_TRUE(result->existing_request);EXPECT_FALSE(result->enqueued);
    EXPECT_EQ(result->durable->message,first->durable->message);
    ASSERT_EQ(result->transparent_outputs.size(),first->transparent_outputs.size());
    for(size_t i=0;i<result->transparent_outputs.size();++i){
        EXPECT_EQ(result->transparent_outputs[i].amount_una,first->transparent_outputs[i].amount_una);
        EXPECT_EQ(result->transparent_outputs[i].script_pub_key,first->transparent_outputs[i].script_pub_key);
    }
    EXPECT_FALSE(wrote);EXPECT_EQ(points,retry_points);EXPECT_EQ(f.Snapshot(),baseline);
}
TEST(OrchardDetachedShieldReservation, ActualAutomaticSelectionIssuesChangeThenCommitsAndRetriesExactly){
    ShieldReservationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    auto source=ChainstateService::AcquireWalletIndexUse(f.f.service);wallet::OrchardProofJobs jobs,stopped;
    const auto session=f.Selected().session;const auto revision=f.Account(3).revision;const auto payments=f.Payments();
    auto* db=wallet.getCurrentDatabase();
    struct Commit {ChainstateService& source;wallet::OrchardProofJobs& jobs;bool updated=false,selected=false,hidden=false;size_t reservations=0;} observed{*f.f.service,jobs};
    DetachedShieldSnapshotWriteObserver observer(db,&observed,[](sqlite3_context* context,int,sqlite3_value**){
            static_cast<Commit*>(sqlite3_user_data(context))->updated=true;
            sqlite3_result_null(context);
        });
    sqlite3_commit_hook(db,[](void* p){auto& o=*static_cast<Commit*>(p);if(o.updated){++o.reservations;
        o.selected=ShieldedStateStartupTestAccess::VaultSelectedHeld(o.source);o.hidden=!o.jobs.Query(orchard::Hash{81});}return 0;
    },&observed);
    auto queued=f.f.service->queueRuntimeWalletShieldPayment(wallet,session,3,revision,orchard::Hash{81},payments,10000,jobs);
    sqlite3_commit_hook(db,nullptr,nullptr);ASSERT_TRUE(observer.Close());
    ASSERT_TRUE(queued);ASSERT_TRUE(queued->durable);EXPECT_TRUE(queued->enqueued);
    EXPECT_EQ(observed.reservations,1u);EXPECT_TRUE(observed.selected);EXPECT_TRUE(observed.hidden);
    ASSERT_EQ(queued->transparent_outputs.size(),1u);EXPECT_EQ(queued->transparent_outputs[0].amount_una,70000u);
    EXPECT_NE(queued->transparent_outputs[0].script_pub_key,f.script);EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
    const auto baseline=f.Snapshot();stopped.RequestStop();
    auto retry=f.f.service->queueRuntimeWalletShieldPayment(wallet,session,3,revision,orchard::Hash{81},payments,10000,stopped);
    ASSERT_TRUE(retry);ASSERT_TRUE(retry->durable);EXPECT_TRUE(retry->existing_request);EXPECT_FALSE(retry->enqueued);
    EXPECT_EQ(retry->durable->message,queued->durable->message);
    ASSERT_EQ(retry->transparent_outputs.size(),queued->transparent_outputs.size());
    for(size_t i=0;i<retry->transparent_outputs.size();++i){
        EXPECT_EQ(retry->transparent_outputs[i].amount_una,queued->transparent_outputs[i].amount_una);
        EXPECT_EQ(retry->transparent_outputs[i].script_pub_key,queued->transparent_outputs[i].script_pub_key);
    }
    EXPECT_EQ(f.Snapshot(),baseline);EXPECT_FALSE(stopped.Query(orchard::Hash{81}));
}
TEST(OrchardDetachedShieldReservation, ArchiveReactivationAndConflictingNewInputRollBackTogether){
    ShieldReservationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto& service_jobs=use->OrchardProofs();
    auto queued=f.QueueSelected(service_jobs,f.Account(3).revision,f.Inputs());ASSERT_TRUE(queued->enqueued);
    ASSERT_EQ(ServiceProofTerminal(service_jobs,orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);
    const auto body=f.Finish(service_jobs);const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);
    ASSERT_TRUE(f.Mine(MempoolTransaction::FromOrchard(envelope)));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);f.ArchiveConfirmedShield();
    const auto before3=f.Account(3),before17=f.Account(17);const auto session=f.Selected().session;
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    const auto view=f.View();const auto event=view->Event(view->Head().sequence);const auto block=view->Block(view->Head().sequence);
    ASSERT_EQ(event->direction,RuntimeBlockDirection::Disconnect);
    for(const auto& [number,before]:std::vector<std::pair<uint32_t,wallet::OrchardAccountDelivery::Applied>>{{3,before3},{17,before17}}){
        const auto receipt=before.account.Delivery();
        (void)wallet::OrchardAccountDelivery::Disconnect(wallet,session,{f.Domain(),102,number},before.revision,
            view->Point({receipt.sequence,receipt.digest}),*event,*block,view->Point(event->cursor));
    }
    const auto undone3=f.Account(3);const auto revision=f.Account(17).revision;
    ASSERT_TRUE(undone3.account.Operations().Entries().empty());const auto baseline=f.Snapshot();
    const auto inputs=f.Inputs();const auto payments=f.Payments();const std::vector<orchard::TransparentOutput> outputs{{70000,f.script}};
    auto* db=wallet.getCurrentDatabase();size_t points=0;
    auto plan=DetachedShield::Prepare(wallet,session,{f.Domain(),102,17},revision,*view,orchard::Hash{82},inputs,payments,outputs,10000,
        DetachedShield::Mode::Exact,false,[&](RuntimeOutboxCursor cursor){++points;
            EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
            EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));return view->Point(cursor);});
    EXPECT_GT(points,0u);EXPECT_EQ(f.Snapshot(),baseline);const auto prepared_points=points;
    size_t writes=0;DetachedShieldSnapshotWriteObserver observer(db,&writes,[](sqlite3_context* context,int,sqlite3_value**){
            ++*static_cast<size_t*>(sqlite3_user_data(context));
            sqlite3_result_null(context);
        });
    wallet::OrchardProofJobs jobs;std::string failure;
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        try{(void)DetachedShield::Commit(wallet,session,std::move(plan),inputs,outputs,jobs);}
        catch(const std::runtime_error& e){failure=e.what();}
    }

    ASSERT_TRUE(observer.Close());
    EXPECT_EQ(failure,"Orchard account delivery ownership or state mismatch");EXPECT_EQ(writes,1u);EXPECT_EQ(points,prepared_points);
    EXPECT_EQ(f.Snapshot(),baseline);EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_FALSE(jobs.Query(orchard::Hash{82}));
    const auto restored=wallet::OrchardAccountDelivery::ReconcileForReplay(wallet,session,{f.Domain(),102,3},undone3.revision,*view);
    EXPECT_EQ(restored.revision,undone3.revision+1);
    EXPECT_EQ(restored.account.Operations().Entries().at(orchard::Hash{81}).transaction,body);
    EXPECT_EQ(f.Account(17).revision,revision);
}
} // namespace dinero
#endif
