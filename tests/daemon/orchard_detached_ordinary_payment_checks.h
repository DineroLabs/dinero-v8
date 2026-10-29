#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero::wallet {
struct OrchardDetachedOrdinaryPaymentTestAccess {
    using Points=std::function<OrchardAccountDelivery::RestorePoint(RuntimeOutboxCursor)>;
    static auto Prepare(WalletManager& wallet,const WalletSigningIdentity& selected,
            const RuntimeAccountReplay& view,const Points& points){
        return OrchardAccountDelivery::PrepareOrdinaryPaymentWithPoints(wallet,selected,view,points);
    }
    static auto Commit(WalletManager& wallet,const WalletSigningIdentity& selected,
            std::unique_ptr<OrchardCatalogRecoveryPlan> plan,const UnsignedTransaction& input,
            const PendingPaymentIntent& intent){
        return OrchardAccountDelivery::CommitOrdinaryPaymentForReplay(wallet,selected,std::move(plan),input,intent);
    }
};
}
namespace dinero {
TEST(OrchardDetachedOrdinaryPayment, ReleasedRestorationAndSelectedCommitPreservePaymentIdentity){
    for(bool populated:{false,true}){
        SharedPaymentFixture f;if(populated){f.Create(3);f.Create(17);}
        auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
        const auto view=f.View();const auto selected=f.Selected();const auto before=f.Snapshot();size_t points=0;
        const auto provider=[&](RuntimeOutboxCursor cursor){
            ++points;
            EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
            EXPECT_TRUE(sqlite3_get_autocommit(wallet.getCurrentDatabase()));
            EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
            return view->Point(cursor);
        };
        auto plan=wallet::OrchardDetachedOrdinaryPaymentTestAccess::Prepare(wallet,selected,*view,provider);
        ASSERT_TRUE(plan);
        EXPECT_GT(points,0u);
        EXPECT_EQ(f.Snapshot(),before);
        const auto calls=points;SignResult result;
        struct Commit {ChainstateService& service;size_t count=0;bool selected=false;} observed{*f.f.service};
        auto* db=wallet.getCurrentDatabase();
        sqlite3_commit_hook(db,[](void* p){auto& o=*static_cast<Commit*>(p);++o.count;
            o.selected=ShieldedStateStartupTestAccess::VaultSelectedHeld(o.service);return 0;},&observed);
        {
            auto chain=f.f.service->AcquireBlockIngressActivationLock();
            result=wallet::OrchardDetachedOrdinaryPaymentTestAccess::Commit(wallet,selected,std::move(plan),f.Input(),f.Intent());
        }
        sqlite3_commit_hook(db,nullptr,nullptr);
        ASSERT_TRUE(result.success)<<result.error;
        EXPECT_EQ(observed.count,1u);
        EXPECT_TRUE(observed.selected);
        EXPECT_EQ(points,calls);
        const auto pending=wallet.getPendingPayments();
        ASSERT_EQ(pending.size(),1u);
        EXPECT_EQ(pending[0].signed_body,result.signed_tx.tx.Serialize(TxSerializationMode::WithWitness));
        EXPECT_EQ(pending[0].intent,f.Intent());
        EXPECT_FALSE(f.Pay().success);
        const auto committed=f.Snapshot();f.Reopen();
        EXPECT_EQ(f.Snapshot(),committed);
        EXPECT_EQ(wallet.getPendingPayments()[0].signed_body,pending[0].signed_body);
    }
    // Exercise the actual selected service wrapper as well as the split seam.
    SharedPaymentFixture f;f.Create(3);f.Create(17);
    const auto result=f.Pay();
    ASSERT_TRUE(result.success)<<result.error;
    EXPECT_EQ(f.wallet->get().getPendingPayments()[0].signed_body,result.signed_tx.tx.Serialize(TxSerializationMode::WithWitness));
}
TEST(OrchardDetachedOrdinaryPayment, RequiredCaptureReadHistoryAndCommitRefusalsPublishNothing){
    SharedPaymentFixture f;f.Create(3);f.Create(17);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto* db=wallet.getCurrentDatabase();
    const auto view=f.View();const auto selected=f.Selected();const auto before=f.Snapshot();size_t calls=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){++calls;return view->Point(cursor);};
    const auto prepare=[&]{return wallet::OrchardDetachedOrdinaryPaymentTestAccess::Prepare(wallet,selected,*view,provider);};
    bool denied=false;
    sqlite3_set_authorizer(db,[](void* p,int action,const char* name,const char*,const char*,const char*){
        if(action==SQLITE_TRANSACTION&&name&&std::string_view(name)=="COMMIT"){
            *static_cast<bool*>(p)=true;return SQLITE_DENY;
        }return SQLITE_OK;
    },&denied);
    EXPECT_THROW((void)prepare(),std::runtime_error);
    sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_TRUE(denied);
    EXPECT_EQ(calls,0u);
    EXPECT_EQ(f.Snapshot(),before);
    for(int fault:{0,1,2,3}){
        auto plan=prepare();const auto prepared_calls=calls;bool triggered=false;SignResult result;
        struct LateRead {bool staged=false,denied=false;} late;
        if(fault==0)sqlite3_set_authorizer(db,[](void* p,int action,const char* name,const char*,const char*,const char*){
            if(action==SQLITE_READ&&name&&std::string_view(name)=="orchard_wallet_retained"){
                *static_cast<bool*>(p)=true;return SQLITE_DENY;
            }return SQLITE_OK;
        },&triggered);
        if(fault==1)f.Sql("CREATE TRIGGER refuse_detached_ordinary BEFORE INSERT ON transactions BEGIN SELECT RAISE(ABORT,'ordinary history refusal'); END");
        if(fault==2)sqlite3_commit_hook(db,[](void* p){*static_cast<bool*>(p)=true;return 1;},&triggered);
        if(fault==3)sqlite3_set_authorizer(db,[](void* p,int action,const char* table,const char* column,const char*,const char*){
            auto& o=*static_cast<LateRead*>(p);
            if(action==SQLITE_UPDATE&&table&&column&&std::string_view(table)=="wallet_meta"&&
               std::string_view(column)=="pending_payment_owner")o.staged=true;
            if(o.staged&&action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_retained"){
                o.denied=true;return SQLITE_DENY;
            }return SQLITE_OK;
        },&late);
        {
            auto chain=f.f.service->AcquireBlockIngressActivationLock();
            EXPECT_THROW(result=wallet::OrchardDetachedOrdinaryPaymentTestAccess::Commit(wallet,selected,std::move(plan),f.Input(),f.Intent()),std::runtime_error);
        }
        sqlite3_set_authorizer(db,nullptr,nullptr);sqlite3_commit_hook(db,nullptr,nullptr);
        if(fault==1)f.Sql("DROP TRIGGER refuse_detached_ordinary");
        else if(fault==3){EXPECT_TRUE(late.staged);EXPECT_TRUE(late.denied);}
        else EXPECT_TRUE(triggered);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(calls,prepared_calls);
        EXPECT_TRUE(sqlite3_get_autocommit(db));
        EXPECT_EQ(f.Snapshot(),before);
        EXPECT_TRUE(wallet.getPendingPayments().empty());
    }
    EXPECT_TRUE(f.Pay().success);
}
TEST(OrchardDetachedOrdinaryPayment, ChangedOtherAccountAndSessionRefusePreparedOwner){
    SharedPaymentFixture f;f.Create(3);f.Create(17);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    const auto view=f.View();const auto selected=f.Selected();const auto provider=[&](RuntimeOutboxCursor c){return view->Point(c);};
    const auto prepare=[&]{return wallet::OrchardDetachedOrdinaryPaymentTestAccess::Prepare(wallet,selected,*view,provider);};
    auto plan=prepare();
    (void)wallet::OrchardAccountDelivery::IssueCatalogReceiverForReplay(wallet,selected.session,
        {f.Domain(),102,17},*view,orchard::WalletScope::External);
    const auto changed=f.Snapshot();SignResult result;
    {
        auto chain=f.f.service->AcquireBlockIngressActivationLock();
        EXPECT_THROW(result=wallet::OrchardDetachedOrdinaryPaymentTestAccess::Commit(wallet,selected,std::move(plan),f.Input(),f.Intent()),wallet::OrchardAccountDelivery::CatalogChanged);
    }
    EXPECT_FALSE(result.success);
    EXPECT_EQ(f.Snapshot(),changed);
    EXPECT_TRUE(wallet.getPendingPayments().empty());
    plan=prepare();auto wrong=selected;++wrong.session;
    {
        auto chain=f.f.service->AcquireBlockIngressActivationLock();
        EXPECT_THROW(result=wallet::OrchardDetachedOrdinaryPaymentTestAccess::Commit(wallet,wrong,std::move(plan),f.Input(),f.Intent()),std::runtime_error);
    }
    EXPECT_EQ(f.Snapshot(),changed);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(f.Pay().success);
}
TEST(OrchardDetachedOrdinaryPayment, CallerOwnershipLockAndSourceRefusalPreserveWallet){
    SharedPaymentFixture f;f.Create(3);f.Create(17);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();auto* db=wallet.getCurrentDatabase();
    const auto view=f.View();const auto selected=f.Selected();const auto before=f.Snapshot();size_t calls=0;
    const auto provider=[&](RuntimeOutboxCursor c){++calls;return view->Point(c);};
    const auto prepare=[&]{return wallet::OrchardDetachedOrdinaryPaymentTestAccess::Prepare(wallet,selected,*view,provider);};
    {
        auto lease=wallet.AcquireDatabaseLease();
        EXPECT_THROW((void)prepare(),std::runtime_error);
        ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_THROW((void)prepare(),std::runtime_error);
        EXPECT_FALSE(sqlite3_get_autocommit(db));
        ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    }
    EXPECT_EQ(calls,0u);
    wallet.lockWallet();
    EXPECT_THROW((void)prepare(),std::runtime_error);
    wallet.unlockWallet("canonical-fixture-pass",0);
    const auto unavailable=[&](RuntimeOutboxCursor)->wallet::OrchardAccountDelivery::RestorePoint{
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));throw std::runtime_error("source refused");
    };
    EXPECT_THROW((void)wallet::OrchardDetachedOrdinaryPaymentTestAccess::Prepare(wallet,selected,*view,unavailable),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);
    EXPECT_TRUE(wallet.getPendingPayments().empty());
    EXPECT_TRUE(f.Pay().success);
}
TEST(OrchardDetachedOrdinaryPayment, PreparedPaymentRechecksBothReservationSetsBeforeSigning){
    SharedPaymentFixture f;f.Create(3);f.Create(17);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    const auto view=f.View();const auto selected=f.Selected();const auto provider=[&](RuntimeOutboxCursor c){return view->Point(c);};
    auto plan=wallet::OrchardDetachedOrdinaryPaymentTestAccess::Prepare(wallet,selected,*view,provider);
    // A separately committed real ordinary payment is not part of the catalog;
    // the writer must still inspect its current durable reservation set.
    ASSERT_TRUE(f.Pay().success);const auto committed=f.Snapshot();SignResult result;
    {
        auto chain=f.f.service->AcquireBlockIngressActivationLock();
        EXPECT_THROW(result=wallet::OrchardDetachedOrdinaryPaymentTestAccess::Commit(wallet,selected,std::move(plan),f.Input(),f.Intent()),std::runtime_error);
    }
    EXPECT_FALSE(result.success);
    EXPECT_EQ(f.Snapshot(),committed);
    EXPECT_EQ(wallet.getPendingPayments().size(),1u);
    SharedPaymentFixture orchard;orchard.Create(3);orchard.Create(17);orchard.ReserveFunding(17);
    const auto reserved=orchard.Snapshot();
    EXPECT_FALSE(orchard.Pay().success);
    EXPECT_EQ(orchard.Snapshot(),reserved);
    EXPECT_TRUE(orchard.wallet->get().getPendingPayments().empty());
}
TEST(OrchardDetachedOrdinaryPayment, ArchivedReactivationRollsBackWhenOrdinarySigningRefuses){
    OrchardSpendRequestFixture f;auto request=f.Reserve();const auto authorization=f.Prove(request);
    (void)f.Ready(1,request.revision,authorization);
    (void)f.Mine(MempoolTransaction::FromOrchard(authorization.Transaction()));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    f.ArchiveCompleted();
    const auto before3=f.Account(3),before17=f.Account(17);
    ASSERT_TRUE(before3.account.Operations().Entries().empty());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    const auto selected=CaptureWalletSigningIdentity(wallet,"canonical-recovery");
    auto* db=wallet.getCurrentDatabase();orchard::WalletStorageIdentity archive_identity;
    {
        auto lease=wallet.AcquireDatabaseLease();auto seed=lease->CopyRecoverySeed(selected.session);InventoryTestTransaction tx(db);
        const auto inventory=wallet::OrchardOwnershipInventory::Read(db,seed->Bytes());
        ASSERT_EQ(inventory.accounts.size(),2u);
        ASSERT_EQ(inventory.accounts[0].entry.account,3u);
        ASSERT_EQ(inventory.accounts[0].current.archive.size(),1u);
        archive_identity=inventory.accounts[0].current.archive[0].identity;tx.Commit();
    }
    const auto archive_bytes=[&]{
        auto lease=wallet.AcquireDatabaseLease();struct Statement{sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} q;
        OrchardAdmissionFixture::Require(sqlite3_prepare_v2(db,"SELECT revision,sealed FROM orchard_wallet_snapshots WHERE wallet_id=? AND account=?",-1,&q.p,nullptr)==SQLITE_OK);
        OrchardAdmissionFixture::Require(sqlite3_bind_blob(q.p,1,archive_identity.wallet_id.data(),32,SQLITE_TRANSIENT)==SQLITE_OK);
        OrchardAdmissionFixture::Require(sqlite3_bind_int64(q.p,2,archive_identity.account)==SQLITE_OK);
        OrchardAdmissionFixture::Require(sqlite3_step(q.p)==SQLITE_ROW);
        const auto revision=sqlite3_column_int64(q.p,0);const auto* bytes=static_cast<const char*>(sqlite3_column_blob(q.p,1));
        OrchardAdmissionFixture::Require(bytes&&sqlite3_column_bytes(q.p,1)>0);
        const std::string sealed(bytes,sqlite3_column_bytes(q.p,1));
        OrchardAdmissionFixture::Require(sqlite3_step(q.p)==SQLITE_DONE);
        return std::pair{revision,sealed};
    };
    const auto original_archive=archive_bytes();
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    const auto source=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(source.ok());const auto view=*source;
    const auto event=view->Event(view->Head().sequence);const auto block=view->Block(view->Head().sequence);
    ASSERT_EQ(event->direction,RuntimeBlockDirection::Disconnect);
    // Existing low-level undo deliberately does not reconcile archives. Apply
    // its real authenticated event/parent to both actual accounts, leaving a
    // caught-up legacy owner whose completed archival cause just disappeared.
    for(const auto& [number,before]:std::vector<std::pair<uint32_t,wallet::OrchardAccountDelivery::Applied>>{{3,before3},{17,before17}}){
        const auto receipt=before.account.Delivery();
        const auto reverted=wallet::OrchardAccountDelivery::Disconnect(wallet,selected.session,{f.Domain(),102,number},
            before.revision,view->Point({receipt.sequence,receipt.digest}),*event,*block,view->Point(event->cursor));
        EXPECT_EQ(reverted.account.Delivery().sequence,view->Head().sequence);
        EXPECT_EQ(reverted.revision,before.revision+1);
    }
    const auto undone=f.Account(3);const auto baseline=f.Snapshot();
    ASSERT_TRUE(undone.account.Operations().Entries().empty());
    EXPECT_EQ(undone.account.Archive(),before3.account.Archive());
    EXPECT_EQ(archive_bytes(),original_archive);
    size_t callbacks=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){
        ++callbacks;
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
        EXPECT_TRUE(sqlite3_get_autocommit(db));
        EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
        return view->Point(cursor);
    };
    auto plan=wallet::OrchardDetachedOrdinaryPaymentTestAccess::Prepare(wallet,selected,*view,provider);
    ASSERT_TRUE(plan);
    EXPECT_GT(callbacks,0u);
    EXPECT_EQ(f.Snapshot(),baseline);
    const auto prepared_callbacks=callbacks;
    struct Writes {
        size_t accounts=0;bool selected=false,identity=true,in_transaction=true;
        uint64_t expected;sqlite3* db;ChainstateService& service;
    } writes{0,false,true,true,undone.revision,db,*f.f.service};
    // SQLite update hooks exclude WITHOUT ROWID tables. This AFTER trigger
    // observes an actual row update, including one later rolled back. Its
    // callback runs no SQL and checks the exact account/revision transition.
    ASSERT_EQ(sqlite3_create_function_v2(db,"observe_ordinary_reactivation",3,SQLITE_UTF8,&writes,
        [](sqlite3_context* context,int argc,sqlite3_value** values){
            auto& o=*static_cast<Writes*>(sqlite3_user_data(context));++o.accounts;
            o.selected=ShieldedStateStartupTestAccess::VaultSelectedHeld(o.service);
            o.in_transaction=o.in_transaction&&!sqlite3_get_autocommit(o.db);
            o.identity=o.identity&&argc==3&&sqlite3_value_int64(values[0])==3&&
                sqlite3_value_int64(values[1])==static_cast<sqlite3_int64>(o.expected)&&
                sqlite3_value_int64(values[2])==static_cast<sqlite3_int64>(o.expected+1);
            sqlite3_result_null(context);
        },nullptr,nullptr,nullptr),SQLITE_OK);
    struct ObserverCleanup {
        sqlite3* db;
        ~ObserverCleanup(){
            sqlite3_exec(db,"DROP TRIGGER IF EXISTS temp.observe_ordinary_reactivation",nullptr,nullptr,nullptr);
            sqlite3_create_function_v2(db,"observe_ordinary_reactivation",3,SQLITE_UTF8,nullptr,nullptr,nullptr,nullptr,nullptr);
        }
    } cleanup{db};
    f.Sql("CREATE TEMP TRIGGER observe_ordinary_reactivation AFTER UPDATE ON main.orchard_wallet_snapshots BEGIN SELECT observe_ordinary_reactivation(NEW.account,OLD.revision,NEW.revision); END");
    UnsignedTransaction invalid;PendingPaymentIntent intent;SignResult result;std::string failure;
    {
        auto chain=f.f.service->AcquireBlockIngressActivationLock();
        try{result=wallet::OrchardDetachedOrdinaryPaymentTestAccess::Commit(wallet,selected,std::move(plan),invalid,intent);}
        catch(const std::runtime_error& e){failure=e.what();}
    }
    f.Sql("DROP TRIGGER temp.observe_ordinary_reactivation");
    EXPECT_EQ(sqlite3_create_function_v2(db,"observe_ordinary_reactivation",3,SQLITE_UTF8,nullptr,nullptr,nullptr,nullptr,nullptr),SQLITE_OK);
    // This exact existing signer refusal happens after catalog reactivation;
    // a source/fixture refusal before the write is not an accepted regression.
    EXPECT_EQ(failure,"Signing input metadata is incomplete");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(writes.accounts,1u);
    EXPECT_TRUE(writes.selected);
    EXPECT_TRUE(writes.identity);
    EXPECT_TRUE(writes.in_transaction);
    EXPECT_EQ(callbacks,prepared_callbacks);
    EXPECT_TRUE(sqlite3_get_autocommit(db));
    EXPECT_EQ(f.Snapshot(),baseline);
    EXPECT_EQ(archive_bytes(),original_archive);
    EXPECT_TRUE(wallet.getPendingPayments().empty());
    EXPECT_TRUE(f.Account(3).account.Operations().Entries().empty());
    // Independently exercise the established explicit reconciliation owner on
    // the same preserved archive after rollback, without inventing a payment.
    const auto restored=wallet::OrchardAccountDelivery::ReconcileForReplay(wallet,selected.session,
        {f.Domain(),102,3},undone.revision,*view);
    EXPECT_EQ(restored.revision,undone.revision+1);
    ASSERT_EQ(restored.account.Operations().Entries().size(),1u);
    EXPECT_EQ(restored.account.Operations().Entries().at(f.Operation(1)).transaction,authorization.Orchard().CanonicalBytes());
    EXPECT_EQ(archive_bytes(),original_archive);
    EXPECT_EQ(f.Account(17).revision,before17.revision+1);
}
} // namespace dinero
#endif
