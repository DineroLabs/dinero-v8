#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero::wallet {
struct OrchardDetachedSpendReservationTestAccess {
    using Points=std::function<OrchardAccountDelivery::RestorePoint(RuntimeOutboxCursor)>;
    static auto Queue(WalletManager& wallet,uint64_t session,const OrchardAccountDelivery::Profile& profile,
            uint64_t revision,const RuntimeAccountReplay& view,const orchard::Hash& id,
            std::span<const orchard::WalletPayment> payments,std::span<const orchard::TransparentOutput> outputs,
            uint64_t fee,OrchardProofJobs& jobs,const Points& points){
        return OrchardAccountDelivery::ReserveSpendWithRestorePoints(wallet,session,profile,revision,view,
            id,payments,outputs,fee,&jobs,true,points).queued;
    }
};
}
namespace dinero {
// Test-only actual-write observation also works for WITHOUT ROWID snapshots.
namespace {
struct DetachedSpendSnapshotWriteObserver {
    sqlite3* db;
    DetachedSpendSnapshotWriteObserver(sqlite3* database,void* state,void (*callback)(sqlite3_context*,int,sqlite3_value**)):db(database){
        OrchardAdmissionFixture::Require(sqlite3_create_function_v2(db,"observe_detached_spend_snapshot_write",0,SQLITE_UTF8,state,
            callback,nullptr,nullptr,nullptr)==SQLITE_OK);
        if(sqlite3_exec(db,"CREATE TEMP TRIGGER observe_detached_spend_snapshot_write AFTER UPDATE ON main.orchard_wallet_snapshots BEGIN SELECT observe_detached_spend_snapshot_write(); END",nullptr,nullptr,nullptr)!=SQLITE_OK){
            (void)Close();throw std::runtime_error("Snapshot write observer trigger failed");
        }
    }
    DetachedSpendSnapshotWriteObserver(const DetachedSpendSnapshotWriteObserver&)=delete;
    DetachedSpendSnapshotWriteObserver& operator=(const DetachedSpendSnapshotWriteObserver&)=delete;
    ~DetachedSpendSnapshotWriteObserver(){(void)Close();}
    bool Close() noexcept {
        if(!db)return true;
        const int drop=sqlite3_exec(db,"DROP TRIGGER IF EXISTS temp.observe_detached_spend_snapshot_write",nullptr,nullptr,nullptr);
        const int unregister=sqlite3_create_function_v2(db,"observe_detached_spend_snapshot_write",0,SQLITE_UTF8,nullptr,nullptr,nullptr,nullptr,nullptr);
        db=nullptr;return drop==SQLITE_OK&&unregister==SQLITE_OK;
    }
};
} // namespace
TEST(OrchardDetachedSpendReservation, ReleasedRestorationAndCommitBeforeProofPublication){
    OrchardSpendRequestFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    wallet::OrchardProofJobs jobs;const auto source=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(source.ok());
    const auto view=*source;const auto revision=f.Account(3).revision;const auto session=f.Session();
    const auto payments=f.Payments();auto* db=wallet.getCurrentDatabase();size_t points=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){
        ++points;
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
        EXPECT_TRUE(sqlite3_get_autocommit(db));
        EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
        return view->Point(cursor);
    };
    struct Commit {wallet::OrchardProofJobs& jobs;orchard::Hash id;size_t seen=0;bool hidden=true;} commit{jobs,f.Operation(1)};
    sqlite3_commit_hook(db,[](void* p){auto& o=*static_cast<Commit*>(p);++o.seen;o.hidden&=!o.jobs.Query(o.id);return 0;},&commit);
    auto result=wallet::OrchardDetachedSpendReservationTestAccess::Queue(wallet,session,{f.Domain(),102,3},revision,
        *view,f.Operation(1),payments,{},100000,jobs,provider);
    sqlite3_commit_hook(db,nullptr,nullptr);
    ASSERT_TRUE(result);ASSERT_TRUE(result->durable);
    EXPECT_GT(points,0u);EXPECT_GT(commit.seen,0u);EXPECT_TRUE(commit.hidden);
    EXPECT_TRUE(result->enqueued);EXPECT_FALSE(result->existing_request);
    EXPECT_TRUE(result->durable->request_commitment);
    EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
    EXPECT_EQ(jobs.Query(f.Operation(1)),std::optional(wallet::OrchardProofJobs::State::Queued));
    EXPECT_EQ(f.Account(3).account.Operations().Entries().at(f.Operation(1)).message,result->durable->message);
    const auto reserved=f.Snapshot();wallet.open("canonical-recovery");wallet.unlockWallet("canonical-fixture-pass",0);
    EXPECT_EQ(f.Snapshot(),reserved);
    EXPECT_EQ(f.Account(3).account.Operations().Entries().at(f.Operation(1)).nullifiers,result->durable->nullifiers);
}
TEST(OrchardDetachedSpendReservation, CaptureRecheckWriteAndCommitRefusalsPublishNothing){
    OrchardSpendRequestFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    wallet::OrchardProofJobs jobs;const auto source=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(source.ok());
    const auto view=*source;const auto revision=f.Account(3).revision;const auto session=f.Session();
    const auto payments=f.Payments();auto* db=wallet.getCurrentDatabase();const auto before=f.Snapshot();
    for(int fault:{0,1,2,3,4}){
        struct Fault {int kind;size_t callbacks=0;bool writes=false,denied=false,commit=false;} observed{fault};
        const auto provider=[&](RuntimeOutboxCursor cursor){
            ++observed.callbacks;
            EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
            return view->Point(cursor);
        };
        sqlite3_set_authorizer(db,[](void* p,int action,const char* name,const char*,const char*,const char*){
            auto& o=*static_cast<Fault*>(p);
            if(o.kind==0&&action==SQLITE_TRANSACTION&&name&&std::string_view(name)=="COMMIT"){
                o.denied=true;return SQLITE_DENY;
            }
            if(action==SQLITE_READ&&name&&std::string_view(name)=="orchard_wallet_retained"&&
               ((o.kind==1&&o.callbacks>0)||(o.kind==4&&o.writes))){o.denied=true;return SQLITE_DENY;}
            return SQLITE_OK;
        },&observed);
        DetachedSpendSnapshotWriteObserver observer(db,&observed,[](sqlite3_context* context,int,sqlite3_value**){
            static_cast<Fault*>(sqlite3_user_data(context))->writes=true;
            sqlite3_result_null(context);
        });
        if(fault==2)f.Sql("CREATE TRIGGER refuse_detached_spend BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'reservation refusal'); END");
        if(fault==3)sqlite3_commit_hook(db,[](void* p){auto& o=*static_cast<Fault*>(p);
            if(o.writes){o.commit=true;return 1;}return 0;},&observed);
        EXPECT_THROW((void)wallet::OrchardDetachedSpendReservationTestAccess::Queue(wallet,session,{f.Domain(),102,3},revision,
            *view,f.Operation(1),payments,{},100000,jobs,provider),std::runtime_error);
        sqlite3_set_authorizer(db,nullptr,nullptr);sqlite3_commit_hook(db,nullptr,nullptr);ASSERT_TRUE(observer.Close());
        if(fault==2)f.Sql("DROP TRIGGER refuse_detached_spend");
        else if(fault==3){EXPECT_TRUE(observed.writes);EXPECT_TRUE(observed.commit);}
        else EXPECT_TRUE(observed.denied);
        if(fault==0)EXPECT_EQ(observed.callbacks,0u);
        else EXPECT_GT(observed.callbacks,0u);
        if(fault==4)EXPECT_TRUE(observed.writes);
        EXPECT_EQ(f.Snapshot(),before);EXPECT_TRUE(sqlite3_get_autocommit(db));
        EXPECT_FALSE(jobs.Query(f.Operation(1)));f.ExpectAllSlotsFree(jobs);
    }
    EXPECT_TRUE(f.Request(jobs)->enqueued);
}
TEST(OrchardDetachedSpendReservation, ChangedOtherAccountDuringRestorationRefusesCapturedOwner){
    OrchardSpendRequestFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    wallet::OrchardProofJobs jobs;const auto source=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(source.ok());
    const auto view=*source;const auto revision=f.Account(3).revision;const auto session=f.Session();const auto payments=f.Payments();
    bool changed=false;
    const auto provider=[&](RuntimeOutboxCursor cursor){
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
        if(!changed){changed=true;(void)wallet::OrchardAccountDelivery::IssueCatalogReceiverForReplay(wallet,session,
            {f.Domain(),102,17},*view,orchard::WalletScope::External);}
        return view->Point(cursor);
    };
    const auto other_before=f.Account(17).revision;
    EXPECT_THROW((void)wallet::OrchardDetachedSpendReservationTestAccess::Queue(wallet,session,{f.Domain(),102,3},revision,
        *view,f.Operation(1),payments,{},100000,jobs,provider),wallet::OrchardAccountDelivery::CatalogChanged);
    EXPECT_TRUE(changed);EXPECT_EQ(f.Account(17).revision,other_before+1);EXPECT_EQ(f.Account(3).revision,revision);
    EXPECT_TRUE(f.Account(3).account.Operations().Entries().empty());EXPECT_FALSE(jobs.Query(f.Operation(1)));
    f.ExpectAllSlotsFree(jobs);EXPECT_TRUE(f.Request(jobs)->enqueued);
}
TEST(OrchardDetachedSpendReservation, BorrowedOwnersLockAndUnavailableSourcePreserveWallet){
    OrchardSpendRequestFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    wallet::OrchardProofJobs jobs;const auto source=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(source.ok());
    const auto view=*source;const auto revision=f.Account(3).revision;const auto session=f.Session();const auto payments=f.Payments();
    auto* db=wallet.getCurrentDatabase();const auto before=f.Snapshot();size_t callbacks=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){++callbacks;return view->Point(cursor);};
    const auto invoke=[&]{return wallet::OrchardDetachedSpendReservationTestAccess::Queue(wallet,session,{f.Domain(),102,3},revision,
        *view,f.Operation(1),payments,{},100000,jobs,provider);};
    {
        auto lease=wallet.AcquireDatabaseLease();
        EXPECT_THROW((void)invoke(),std::runtime_error);
        ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_THROW((void)invoke(),std::runtime_error);EXPECT_FALSE(sqlite3_get_autocommit(db));
        ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    }
    EXPECT_EQ(callbacks,0u);wallet.lockWallet();
    EXPECT_THROW((void)invoke(),std::runtime_error);wallet.unlockWallet("canonical-fixture-pass",0);
    const auto unavailable=[&](RuntimeOutboxCursor)->wallet::OrchardAccountDelivery::RestorePoint{
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        throw std::runtime_error("source refused");
    };
    EXPECT_THROW((void)wallet::OrchardDetachedSpendReservationTestAccess::Queue(wallet,session,{f.Domain(),102,3},revision,
        *view,f.Operation(1),payments,{},100000,jobs,unavailable),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(jobs.Query(f.Operation(1)));f.ExpectAllSlotsFree(jobs);
    EXPECT_TRUE(invoke()->enqueued);
}
TEST(OrchardDetachedSpendReservation, ExactRetryWithStoppedExecutorDoesNotWriteOrReconcile){
    OrchardSpendRequestFixture f;wallet::OrchardProofJobs jobs,stopped;const auto revision=f.Account(3).revision;
    auto first=f.Request(jobs);ASSERT_TRUE(first->enqueued);ASSERT_TRUE(first->durable);
    stopped.RequestStop();auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    const auto source=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(source.ok());const auto view=*source;
    const auto session=f.Session();const auto payments=f.Payments();auto* db=wallet.getCurrentDatabase();const auto before=f.Snapshot();
    size_t callbacks=0;bool attempted_write=false;
    const auto provider=[&](RuntimeOutboxCursor cursor){++callbacks;
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        return view->Point(cursor);
    };
    sqlite3_set_authorizer(db,[](void* p,int action,const char*,const char*,const char*,const char*){
        if(action==SQLITE_INSERT||action==SQLITE_UPDATE||action==SQLITE_DELETE){*static_cast<bool*>(p)=true;return SQLITE_DENY;}
        return SQLITE_OK;
    },&attempted_write);
    auto retry=wallet::OrchardDetachedSpendReservationTestAccess::Queue(wallet,session,{f.Domain(),102,3},revision,
        *view,f.Operation(1),payments,{},100000,stopped,provider);
    sqlite3_set_authorizer(db,nullptr,nullptr);
    ASSERT_TRUE(retry);ASSERT_TRUE(retry->durable);EXPECT_TRUE(retry->existing_request);EXPECT_FALSE(retry->enqueued);
    EXPECT_EQ(retry->revision,first->revision);EXPECT_EQ(retry->durable->message,first->durable->message);
    EXPECT_EQ(retry->durable->nullifiers,first->durable->nullifiers);
    EXPECT_EQ(retry->durable->request_commitment,first->durable->request_commitment);
    EXPECT_FALSE(attempted_write);EXPECT_GT(callbacks,0u);EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(stopped.Query(f.Operation(1)));
    auto changed=payments;++changed[0].amount_una;
    EXPECT_THROW((void)wallet::OrchardDetachedSpendReservationTestAccess::Queue(wallet,session,{f.Domain(),102,3},revision,
        *view,f.Operation(1),changed,{},100000,stopped,provider),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardDetachedSpendReservation, OtherAccountArchiveReactivationRollsBackOnCapacityRefusal){
    OrchardSpendRequestFixture f;
    // Actual first transfer leaves distinct notes in accounts 3 and 17.
    auto first=f.Reserve();const auto first_auth=f.Prove(first);
    (void)f.Ready(1,first.revision,first_auth);
    (void)f.Mine(MempoolTransaction::FromOrchard(first_auth.Transaction()));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();
    const auto source_before=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(source_before.ok());
    const auto other=f.Account(17);ASSERT_EQ(other.account.Scan().BalanceUna(),200000u);
    const std::vector<orchard::WalletPayment> second_payments{{100000,f.AccountKeys(3).Receiver(orchard::WalletScope::External,{})}};
    auto second=wallet::OrchardAccountDelivery::ReserveCatalogSpendForReplay(wallet,session,{f.Domain(),102,17},
        other.revision,**source_before,f.Operation(2),second_payments,{},100000);
    const auto second_auth=f.Prove(second);
    (void)wallet::OrchardAccountDelivery::ReadyCatalogSpendForReplay(wallet,session,{f.Domain(),102,17},
        second.revision,**source_before,f.Operation(2),second_auth);
    (void)f.Mine(MempoolTransaction::FromOrchard(second_auth.Transaction()));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    f.ArchiveCompleted(2,17);
    const auto before3=f.Account(3),before17=f.Account(17);
    ASSERT_TRUE(before17.account.Operations().Entries().empty());
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    const auto source=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(source.ok());const auto view=*source;
    const auto event=view->Event(view->Head().sequence);const auto block=view->Block(view->Head().sequence);
    ASSERT_EQ(event->direction,RuntimeBlockDirection::Disconnect);
    // Exercise actual low-level undo, which intentionally leaves archival
    // reconciliation for the next composite owner. No snapshot bytes are edited.
    for(const auto& [number,before]:std::vector<std::pair<uint32_t,wallet::OrchardAccountDelivery::Applied>>{{3,before3},{17,before17}}){
        const auto receipt=before.account.Delivery();
        const auto reverted=wallet::OrchardAccountDelivery::Disconnect(wallet,session,{f.Domain(),102,number},before.revision,
            view->Point({receipt.sequence,receipt.digest}),*event,*block,view->Point(event->cursor));
        EXPECT_EQ(reverted.account.Delivery().sequence,view->Head().sequence);
    }
    const auto undone3=f.Account(3),undone17=f.Account(17);const auto baseline=f.Snapshot();
    ASSERT_EQ(undone3.account.Scan().BalanceUna(),200000u);
    ASSERT_TRUE(undone17.account.Operations().Entries().empty());
    wallet::OrchardProofJobs jobs;
    std::vector<std::unique_ptr<wallet::OrchardProofJobs::Submission>> slots;
    for(uint8_t id=80;id<84;++id)slots.push_back(f.Ticket(jobs,id));
    const std::vector<orchard::WalletPayment> payments{{50000,f.AccountKeys(17).Receiver(orchard::WalletScope::External,{})}};
    auto* db=wallet.getCurrentDatabase();size_t callbacks=0,writes=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){++callbacks;
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
        return view->Point(cursor);
    };
    DetachedSpendSnapshotWriteObserver observer(db,&writes,[](sqlite3_context* context,int,sqlite3_value**){
            ++*static_cast<size_t*>(sqlite3_user_data(context));
            sqlite3_result_null(context);
        });
    std::string failure;
    try{(void)wallet::OrchardDetachedSpendReservationTestAccess::Queue(wallet,session,{f.Domain(),102,3},undone3.revision,
        *view,f.Operation(3),payments,{},100000,jobs,provider);}
    catch(const std::runtime_error& error){failure=error.what();}

    ASSERT_TRUE(observer.Close());
    // The exact capacity refusal must follow the real account-17 update;
    // an earlier fixture/source/selection failure is not accepted.
    EXPECT_EQ(failure,"Orchard proof job rejected");EXPECT_EQ(writes,1u);EXPECT_GT(callbacks,0u);
    EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Snapshot(),baseline);
    EXPECT_TRUE(f.Account(17).account.Operations().Entries().empty());EXPECT_FALSE(jobs.Query(f.Operation(3)));
    slots.clear();f.ExpectAllSlotsFree(jobs);
    auto result=wallet::OrchardDetachedSpendReservationTestAccess::Queue(wallet,session,{f.Domain(),102,3},undone3.revision,
        *view,f.Operation(3),payments,{},100000,jobs,provider);
    ASSERT_TRUE(result);ASSERT_TRUE(result->durable);EXPECT_TRUE(result->enqueued);
    const auto restored=f.Account(17);
    EXPECT_EQ(restored.revision,undone17.revision+1);
    EXPECT_EQ(restored.account.Operations().Entries().at(f.Operation(2)).transaction,second_auth.Orchard().CanonicalBytes());
    EXPECT_EQ(f.Account(3).account.Operations().Entries().at(f.Operation(3)).message,result->durable->message);
}
} // namespace dinero
#endif
