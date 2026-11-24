#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero::wallet {
struct OrchardDetachedCatalogTestAccess {
    static auto Read(WalletManager& wallet,uint64_t session,const RuntimeAccountReplay& view,
            const std::function<OrchardAccountDelivery::RestorePoint(RuntimeOutboxCursor)>& points){
        return OrchardAccountDelivery::ReadCatalogWithRestorePoints(wallet,session,view,points);
    }
};
}
namespace dinero {
namespace {
struct DetachedCatalogSqlHooks {
    sqlite3* db;
    size_t commits=0;
    bool deny_final_read=false;
    bool refuse_final_commit=false;
    explicit DetachedCatalogSqlHooks(sqlite3* connection):db(connection){
        if(sqlite3_set_authorizer(db,[](void* value,int action,const char* table,const char*,const char*,const char*){
            auto& self=*static_cast<DetachedCatalogSqlHooks*>(value);
            if(action==SQLITE_TRANSACTION&&table&&std::string_view(table)=="COMMIT"){
                ++self.commits;
                if(self.refuse_final_commit&&self.commits==2)return SQLITE_DENY;
            }
            return self.deny_final_read&&self.commits>=1&&action==SQLITE_READ&&table&&
                std::string_view(table)=="orchard_wallet_snapshots"?SQLITE_DENY:SQLITE_OK;
        },this)!=SQLITE_OK)throw std::runtime_error("Cannot install detached catalog SQL fixture");
    }
    ~DetachedCatalogSqlHooks(){sqlite3_set_authorizer(db,nullptr,nullptr);}
};
}
TEST(OrchardDetachedCatalog, FundedNonconsecutiveAccountsPreserveExactSnapshots){
    OrchardCatalogRecoveryFixture f;
    ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));f.Adopt();
    const auto [body,bundle]=f.Shield(f.AccountKeys(17));(void)f.Mine(body);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto first=f.Account(3);const auto second=f.Account(17);const auto before=f.Snapshot();
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    uint64_t session;sqlite3* db;
    {auto lease=wallet.AcquireDatabaseLease();session=lease->Session();db=lease->Database();}
    {
        DetachedCatalogSqlHooks hooks(db);
        const auto result=wallet::OrchardAccountDelivery::ReadCatalogForReplay(wallet,session,**view);
        EXPECT_EQ(hooks.commits,2u);ASSERT_EQ(result.accounts.size(),2u);
        EXPECT_EQ(result.accounts[0].number,3u);EXPECT_EQ(result.accounts[1].number,17u);
        EXPECT_EQ(result.accounts[0].state.revision,first.revision);
        EXPECT_EQ(result.accounts[1].state.revision,second.revision);
        EXPECT_EQ(OrchardCreationBytes(result.accounts[0].state.account),OrchardCreationBytes(first.account));
        EXPECT_EQ(OrchardCreationBytes(result.accounts[1].state.account),OrchardCreationBytes(second.account));
        EXPECT_EQ(result.accounts[1].state.account.Scan().BalanceUna(),500000u);
    }
    EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
    EXPECT_EQ(f.Snapshot(),before);
}
// Measured on a v8.2.5 desktop (wallet Hotel, 2026-11-24): the page's Orchard
// reads held the wallet's database lease while they restored the whole
// catalog (account discovery did all of it inside one wallet transaction),
// and the node's delivery of new blocks waited behind them for every sample
// of a 30 s profile. A read now restores only when the wallet's bytes have
// changed, and never with the wallet held.
// A wallet lock clears the remembered observation. A read that captured the
// wallet before the lock and finishes restoring after it must not put the
// decrypted catalog back. The lock is simulated at the last moment before the
// store: on the commit of the read's own recheck.
TEST(OrchardDetachedCatalog, AnObservationThatOutlivesAForgetLeavesNothingRemembered){
    using Delivery=wallet::OrchardAccountDelivery;
    OrchardCatalogRecoveryFixture f;
    ASSERT_FALSE(f.Call(3).isMember("error"));f.Adopt();
    const auto [body,bundle]=f.Shield(f.AccountKeys(3));(void)f.Mine(body);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    Delivery::ForgetObservedCatalog();
    auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    uint64_t session;sqlite3* db;
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();session=lease->Session();db=lease->Database();}
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    const auto restores=Delivery::ObservedCatalogRestores();
    static int commits;commits=0;
    sqlite3_commit_hook(db,[](void*)->int{if(++commits==2)wallet::OrchardAccountDelivery::ForgetObservedCatalog();return 0;},nullptr);
    const auto raced=Delivery::ObserveCatalogForReplay(wallet,session,**view);
    sqlite3_commit_hook(db,nullptr,nullptr);
    ASSERT_EQ(commits,2);                      // capture, then the recheck where the forget landed
    ASSERT_TRUE(raced);                        // the reader itself is still answered
    EXPECT_EQ(Delivery::ObservedCatalogRestores(),restores+1);
    // Nothing was remembered: the next read restores for itself...
    const auto next=Delivery::ObserveCatalogForReplay(wallet,session,**view);
    EXPECT_NE(next.get(),raced.get());
    EXPECT_EQ(Delivery::ObservedCatalogRestores(),restores+2);
    // ...and that one, undisturbed, is remembered as before.
    EXPECT_EQ(Delivery::ObserveCatalogForReplay(wallet,session,**view).get(),next.get());
    EXPECT_EQ(Delivery::ObservedCatalogRestores(),restores+2);
    Delivery::ForgetObservedCatalog();
}
TEST(OrchardDetachedCatalog, ObservedCatalogIsRestoredOncePerWalletStateAndNotUnderTheLease){
    using Delivery=wallet::OrchardAccountDelivery;
    OrchardCatalogRecoveryFixture f;
    ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));f.Adopt();
    const auto [body,bundle]=f.Shield(f.AccountKeys(17));(void)f.Mine(body);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    Delivery::ForgetObservedCatalog();
    const auto second=f.Account(17);const auto before=f.Snapshot();
    auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    uint64_t session;sqlite3* db;
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();session=lease->Session();db=lease->Database();}
    const auto restores=Delivery::ObservedCatalogRestores();
    std::shared_ptr<const Delivery::CatalogEnrolled> first;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
        {
            DetachedCatalogSqlHooks hooks(db);
            first=Delivery::ObserveCatalogForReplay(wallet,session,**view);
            EXPECT_EQ(hooks.commits,2u);  // capture, then the recheck after restoring
        }
        ASSERT_TRUE(first);ASSERT_EQ(first->accounts.size(),2u);
        EXPECT_EQ(first->accounts[1].number,17u);EXPECT_EQ(first->accounts[1].state.revision,second.revision);
        EXPECT_EQ(first->accounts[1].state.account.Scan().BalanceUna(),500000u);
        EXPECT_EQ(Delivery::ObservedCatalogRestores(),restores+1);
        // The same wallet bytes: four more reads (a page's tick) restore nothing.
        for(int i=0;i<4;++i){
            DetachedCatalogSqlHooks hooks(db);
            const auto again=Delivery::ObserveCatalogForReplay(wallet,session,**view);
            EXPECT_EQ(again.get(),first.get());
            EXPECT_EQ(hooks.commits,1u);  // one capture to compare; no restore, no recheck
        }
        EXPECT_EQ(Delivery::ObservedCatalogRestores(),restores+1);
        // It reports what the standalone read reports.
        const auto direct=Delivery::ReadCatalogForReplay(wallet,session,**view);
        ASSERT_EQ(direct.accounts.size(),first->accounts.size());
        for(size_t i=0;i<direct.accounts.size();++i){
            EXPECT_EQ(direct.accounts[i].number,first->accounts[i].number);
            EXPECT_EQ(direct.accounts[i].state.revision,first->accounts[i].state.revision);
            EXPECT_EQ(OrchardCreationBytes(direct.accounts[i].state.account),OrchardCreationBytes(first->accounts[i].state.account));
        }
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
    }
    EXPECT_EQ(f.Snapshot(),before);

    // A block is delivered: the wallet's bytes change, so the next read
    // restores once and reports the new state; the old result is not served.
    f.MineEmpty();
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
        const auto after=Delivery::ObserveCatalogForReplay(wallet,session,**view);
        ASSERT_TRUE(after);EXPECT_NE(after.get(),first.get());
        EXPECT_EQ(Delivery::ObservedCatalogRestores(),restores+2);
        EXPECT_EQ(after->accounts[1].state.revision,f.Account(17).revision);
        EXPECT_GT(after->accounts[1].state.account.Delivery().sequence,first->accounts[1].state.account.Delivery().sequence);
        EXPECT_EQ(Delivery::ObserveCatalogForReplay(wallet,session,**view).get(),after.get());
        EXPECT_EQ(Delivery::ObservedCatalogRestores(),restores+2);
    }
    Delivery::ForgetObservedCatalog();
}
// The request the desktop page sends for account discovery. It used to
// restore the whole catalog inside ONE wallet transaction (one COMMIT around
// everything, the wallet held throughout). It now captures, restores with the
// wallet released, and rechecks: two short transactions on a first read, one
// on a repeat, and the answers are identical.
TEST(OrchardDetachedCatalog, AccountDiscoveryRequestDoesNotRestoreInsideAWalletTransaction){
    using Delivery=wallet::OrchardAccountDelivery;
    OrchardCatalogRecoveryFixture f;
    ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));f.Adopt();
    const auto [body,bundle]=f.Shield(f.AccountKeys(17));(void)f.Mine(body);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    Delivery::ForgetObservedCatalog();
    sqlite3* db;
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();db=lease->Database();}
    const auto restores=Delivery::ObservedCatalogRestores();
    din::Json first,again,balance;
    {DetachedCatalogSqlHooks hooks(db);first=rpc_context_wallet_orchard_listaccounts(f.RequestContext(),din::obj());
     EXPECT_EQ(hooks.commits,2u)<<first.toStyledString();}
    ASSERT_FALSE(first.isMember("error"))<<first.toStyledString();
    ASSERT_EQ(first["accounts"].size(),2u);
    EXPECT_EQ(Delivery::ObservedCatalogRestores(),restores+1);
    {DetachedCatalogSqlHooks hooks(db);again=rpc_context_wallet_orchard_listaccounts(f.RequestContext(),din::obj());
     EXPECT_EQ(hooks.commits,1u);}
    EXPECT_EQ(again,first);
    // The page's other reads for the same block reuse the same restored catalog.
    din::Json account=din::obj();account["account"]=17;
    {DetachedCatalogSqlHooks hooks(db);balance=rpc_context_wallet_orchard_getbalance(f.RequestContext(),account);
     EXPECT_EQ(hooks.commits,1u);}
    ASSERT_FALSE(balance.isMember("error"))<<balance.toStyledString();
    EXPECT_EQ(balance["confirmed_una"].asUInt64(),500000u);
    EXPECT_EQ(Delivery::ObservedCatalogRestores(),restores+1);
    Delivery::ForgetObservedCatalog();
}
// Readers that loop as fast as they can must not keep delivery from the
// wallet: with four of them running, every block still reaches the account.
TEST(OrchardDetachedCatalog, LoopingReadersDoNotKeepDeliveryFromTheWallet){
    using Delivery=wallet::OrchardAccountDelivery;
    OrchardCatalogRecoveryFixture f;
    ASSERT_FALSE(f.Call(3).isMember("error"));f.Adopt();
    const auto [body,bundle]=f.Shield(f.AccountKeys(3));(void)f.Mine(body);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    Delivery::ForgetObservedCatalog();
    const uint64_t session=f.Session();
    std::atomic<bool> stop{false};std::atomic<uint64_t> reads{0},refused{0};
    std::vector<std::thread> readers;
    for(int i=0;i<4;++i)readers.emplace_back([&]{
        while(!stop.load()){
            try{
                const auto view=f.f.service->getRuntimeAccountReplay();
                if(!view.ok()){++refused;continue;}
                auto use=WalletService::AcquireWalletUse(f.wallet);
                (void)Delivery::ObserveCatalogForReplay(use->Wallet(),session,**view);++reads;
            }catch(const std::exception&){++refused;}  // a read that overlapped a delivery is retried by its caller
        }
    });
    const auto started=std::chrono::steady_clock::now();
    const int blocks=6;uint64_t sequence=f.Account(3).account.Delivery().sequence;
    for(int i=0;i<blocks;++i){
        f.MineEmpty();
        // The worker's pass: deliver until the account is at the new block.
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
        while(f.Account(3).account.Delivery().sequence<=sequence){
            ASSERT_LT(std::chrono::steady_clock::now(),deadline)<<"delivery starved at block "<<i;
            (void)f.wallet->RecoverActiveWalletFromCanonicalSource();
        }
        sequence=f.Account(3).account.Delivery().sequence;
    }
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
    stop=true;for(auto& reader:readers)reader.join();
    EXPECT_GT(reads.load(),0u);
    std::cerr<<"[looping readers] "<<blocks<<" blocks delivered in "<<elapsed<<" ms with "<<reads.load()
             <<" reads ("<<refused.load()<<" refused) and "<<Delivery::ObservedCatalogRestores()<<" restores"<<std::endl;
    Delivery::ForgetObservedCatalog();
}
TEST(OrchardDetachedCatalog, FinalReadAndCommitRefusalReturnNoCatalog){
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));
    const auto before=f.Snapshot();const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    uint64_t session;sqlite3* db;
    {auto lease=wallet.AcquireDatabaseLease();session=lease->Session();db=lease->Database();}
    {
        DetachedCatalogSqlHooks hooks(db);hooks.deny_final_read=true;
        EXPECT_THROW((void)wallet::OrchardAccountDelivery::ReadCatalogForReplay(wallet,session,**view),std::runtime_error);
        EXPECT_EQ(hooks.commits,1u);
    }
    EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Snapshot(),before);
    {
        DetachedCatalogSqlHooks hooks(db);hooks.refuse_final_commit=true;
        EXPECT_THROW((void)wallet::OrchardAccountDelivery::ReadCatalogForReplay(wallet,session,**view),std::runtime_error);
        EXPECT_EQ(hooks.commits,2u);
    }
    EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Snapshot(),before);
    const auto retry=wallet::OrchardAccountDelivery::ReadCatalogForReplay(wallet,session,**view);
    EXPECT_EQ(retry.accounts.size(),2u);EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardDetachedCatalog, EmptyCatalogDoesNotCreateSchemaAndBorrowedTransactionRefuses){
    OrchardCatalogRecoveryFixture f;ASSERT_EQ(f.StorageTables(),0);const auto before=f.Snapshot();const auto catalog=f.Catalog();
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    uint64_t session;sqlite3* db;
    {auto lease=wallet.AcquireDatabaseLease();session=lease->Session();db=lease->Database();}
    {
        DetachedCatalogSqlHooks hooks(db);
        const auto result=wallet::OrchardAccountDelivery::ReadCatalogForReplay(wallet,session,**view);
        EXPECT_EQ(hooks.commits,2u);EXPECT_TRUE(result.accounts.empty());ASSERT_TRUE(catalog);EXPECT_EQ(result.catalog,*catalog);
    }
    EXPECT_EQ(f.StorageTables(),0);EXPECT_EQ(f.Snapshot(),before);
    ASSERT_EQ(sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW((void)wallet::OrchardAccountDelivery::ReadCatalogForReplay(wallet,session,**view),std::runtime_error);
    EXPECT_FALSE(sqlite3_get_autocommit(db));ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW((void)wallet::OrchardAccountDelivery::ReadCatalogForReplay(wallet,session+1,**view),std::runtime_error);
    EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardDetachedCatalog, RestorationObservesReleasedWalletSeedAndTransaction){
    OrchardCatalogRecoveryFixture f;
    ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));f.Adopt();
    const auto [body,bundle]=f.Shield(f.AccountKeys(17));(void)f.Mine(body);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto expected=f.Account(17);const auto before=f.Snapshot();
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    uint64_t session;sqlite3* db;
    {auto lease=wallet.AcquireDatabaseLease();session=lease->Session();db=lease->Database();}
    size_t points=0,origins=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){
        ++points;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        auto point=(*view)->Point(cursor);const auto original=point.lookups.origin;
        point.lookups.origin=[&,original](uint32_t height,const uint256& hash,const orchard::Hash& txid){
            ++origins;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
            return original(height,hash,txid);
        };
        return point;
    };
    const auto result=wallet::OrchardDetachedCatalogTestAccess::Read(wallet,session,**view,provider);
    EXPECT_GT(points,2u);EXPECT_GT(origins,0u);ASSERT_EQ(result.accounts.size(),2u);
    EXPECT_EQ(result.accounts[1].state.revision,expected.revision);
    EXPECT_EQ(OrchardCreationBytes(result.accounts[1].state.account),OrchardCreationBytes(expected.account));
    EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardDetachedCatalog, WalletLockDuringRestorationIsNotCatalogChange){
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));
    const auto before=f.Snapshot();const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();uint64_t session;
    {auto lease=wallet.AcquireDatabaseLease();session=lease->Session();}
    bool locked=false,refused=false;
    const auto provider=[&](RuntimeOutboxCursor cursor){
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
        if(!locked){wallet.lockWallet();locked=true;}
        return (*view)->Point(cursor);
    };
    try {(void)wallet::OrchardDetachedCatalogTestAccess::Read(wallet,session,**view,provider);}
    catch(const wallet::OrchardAccountDelivery::CatalogChanged&){ADD_FAILURE()<<"Wallet lock was classified as catalog change";}
    catch(const std::runtime_error&){refused=true;}
    EXPECT_TRUE(locked);EXPECT_TRUE(refused);EXPECT_EQ(f.Snapshot(),before);
    wallet.unlockWallet("canonical-fixture-pass",0);
    const auto stable=wallet::OrchardAccountDelivery::ReadCatalogForReplay(wallet,session,**view);
    EXPECT_EQ(stable.accounts.size(),2u);EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardDetachedCatalog, ReachedAndRetainedArchivesAuthenticateAndRecheckExactRevisions){
    OrchardSpendRequestFixture f;auto request=f.Reserve();const auto authorization=f.Prove(request);
    (void)f.Ready(1,request.revision,authorization);
    (void)f.Mine(MempoolTransaction::FromOrchard(authorization.Transaction()));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);f.ArchiveCompleted();
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    uint64_t session;sqlite3* db;orchard::WalletStorageIdentity archived_identity;
    {
        auto lease=wallet.AcquireDatabaseLease();session=lease->Session();db=lease->Database();
        auto seed=lease->CopyRecoverySeed(session);InventoryTestTransaction tx(db);
        const auto inventory=wallet::OrchardOwnershipInventory::Read(db,seed->Bytes());
        ASSERT_EQ(inventory.accounts[0].current.archive.size(),1u);
        archived_identity=inventory.accounts[0].current.archive[0].identity;
        orchard::WalletSnapshotStore store(db,archived_identity,seed->Bytes());auto record=store.Read();ASSERT_TRUE(record);
        (void)store.StageReplaceRetaining(record->revision,record->state);tx.Commit();
    }
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    const auto read=[&]{return wallet::OrchardAccountDelivery::ReadCatalogForReplay(wallet,session,**view);};
    const auto initial=read();ASSERT_EQ(initial.accounts.size(),2u);ASSERT_EQ(initial.accounts[0].archive_revisions.size(),1u);
    const auto before=f.Snapshot();
    const auto id=util::hex(std::vector<unsigned char>(archived_identity.wallet_id.begin(),archived_identity.wallet_id.end()));
    const auto where=" WHERE wallet_id=X'"+id+"'";
    f.Sql(("CREATE TEMP TABLE saved_detached_archive AS SELECT * FROM orchard_wallet_retained"+where).c_str());
    f.Sql(("UPDATE orchard_wallet_retained SET sealed=zeroblob(length(sealed))"+where).c_str());
    EXPECT_THROW((void)read(),std::runtime_error);
    f.Sql(("UPDATE orchard_wallet_retained SET sealed=(SELECT sealed FROM saved_detached_archive AS saved WHERE saved.wallet_id=orchard_wallet_retained.wallet_id AND saved.account=orchard_wallet_retained.account AND saved.revision=orchard_wallet_retained.revision)"+where).c_str());
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(read().accounts[0].archive_revisions,initial.accounts[0].archive_revisions);
    bool changed=false;size_t origins=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        auto point=(*view)->Point(cursor);const auto original=point.lookups.origin;
        point.lookups.origin=[&,original](uint32_t height,const uint256& hash,const orchard::Hash& txid){
            ++origins;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
            const auto result=original(height,hash,txid);
            if(!changed){
                // Ordinary same-thread authenticated SQL replacement, with no
                // competing worker, lock removal or synchronization control.
                auto lease=wallet.AcquireDatabaseLease();auto seed=lease->CopyRecoverySeed(session);
                InventoryTestTransaction tx(db);orchard::WalletSnapshotStore store(db,archived_identity,seed->Bytes());
                auto saved=store.Read();OrchardAdmissionFixture::Require(bool(saved));
                (void)store.StageReplaceRetaining(saved->revision,saved->state);tx.Commit();changed=true;
            }
            return result;
        };
        return point;
    };
    EXPECT_THROW((void)wallet::OrchardDetachedCatalogTestAccess::Read(wallet,session,**view,provider),wallet::OrchardAccountDelivery::CatalogChanged);
    EXPECT_TRUE(changed);EXPECT_GT(origins,0u);EXPECT_TRUE(sqlite3_get_autocommit(db));
    const auto retry=read();ASSERT_EQ(retry.accounts[0].archive_revisions.size(),1u);
    EXPECT_EQ(retry.accounts[0].archive_revisions[0].first,initial.accounts[0].archive_revisions[0].first);
    EXPECT_EQ(retry.accounts[0].archive_revisions[0].second,initial.accounts[0].archive_revisions[0].second+1);
    EXPECT_EQ(OrchardCreationBytes(retry.accounts[0].state.account),OrchardCreationBytes(initial.accounts[0].state.account));
}
} // namespace dinero
#endif
