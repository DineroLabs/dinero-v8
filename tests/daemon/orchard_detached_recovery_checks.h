#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero::wallet {
struct OrchardDetachedRecoveryTestAccess {
    static auto Prepare(WalletManager& wallet,uint64_t session,const RuntimeAccountReplay& replay,
            uint64_t sequence,const std::function<OrchardAccountDelivery::RestorePoint(RuntimeOutboxCursor)>& points){
        return OrchardAccountDelivery::PrepareCatalogRecoveryWithPoints(wallet,session,replay,
            sequence?OrchardAccountDelivery::CatalogRecoveryAction::Apply:OrchardAccountDelivery::CatalogRecoveryAction::Observe,
            sequence,points);
    }
    static void Recheck(WalletManager& wallet,uint64_t session,const OrchardCatalogRecoveryPlan& plan){
        OrchardAccountDelivery::RecheckCatalogRecoveryInTransaction(wallet,session,plan);
    }
    static auto Commit(WalletManager& wallet,uint64_t session,OrchardCatalogRecoveryPlan& plan,size_t index){
        return OrchardAccountDelivery::CommitCatalogRecoveryAccount(wallet,session,plan,index);
    }
};
}
namespace dinero {
TEST(OrchardDetachedRecovery, PreparedTransitionRestoresWithoutOwnersAndCommitDoesNotRestore){
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(17).isMember("error"));f.Adopt();
    const auto [body,bundle]=f.Shield(f.AccountKeys(17));(void)f.Mine(body);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);f.MineEmpty();
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    const auto session=f.Session();auto* db=wallet.getCurrentDatabase();const auto before=f.Snapshot();
    size_t points=0,origins=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){
        ++points;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
        auto point=(*view)->Point(cursor);const auto lookup=point.lookups.origin;
        point.lookups.origin=[&,lookup](uint32_t height,const uint256& hash,const orchard::Hash& txid){
            ++origins;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
            EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
            return lookup(height,hash,txid);
        };
        return point;
    };
    auto plan=wallet::OrchardDetachedRecoveryTestAccess::Prepare(wallet,session,**view,(*view)->Head().sequence,provider);
    ASSERT_TRUE(plan);EXPECT_GT(points,0u);EXPECT_GT(origins,0u);EXPECT_EQ(f.Snapshot(),before);
    const auto restored_points=points,restored_origins=origins;
    {
        auto lease=wallet.AcquireDatabaseLease();ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_NO_THROW(wallet::OrchardDetachedRecoveryTestAccess::Recheck(wallet,session,*plan));
        ASSERT_EQ(sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr),SQLITE_OK);
    }
    const auto applied=wallet::OrchardDetachedRecoveryTestAccess::Commit(wallet,session,*plan,0);
    EXPECT_EQ(applied.account.Delivery().sequence,(*view)->Head().sequence);
    EXPECT_EQ(applied.account.Delivery().digest,(*view)->Head().digest);
    EXPECT_EQ(applied.account.Scan().BalanceUna(),500000u);
    EXPECT_EQ(points,restored_points);EXPECT_EQ(origins,restored_origins);
    EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
    const auto committed=f.Snapshot();
    EXPECT_THROW(wallet::OrchardDetachedRecoveryTestAccess::Commit(wallet,session,*plan,0),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),committed);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),500000u);
}
TEST(OrchardDetachedRecovery, CaptureAndWriteCommitRefusalsPreserveOwners){
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(17).isMember("error"));f.Adopt();f.MineEmpty();
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    const auto session=f.Session();auto* db=wallet.getCurrentDatabase();const auto before=f.Snapshot();
    size_t points=0;const auto provider=[&](RuntimeOutboxCursor cursor){++points;return (*view)->Point(cursor);};
    int commits=0;
    sqlite3_set_authorizer(db,[](void* p,int action,const char* operation,const char*,const char*,const char*){
        if(action==SQLITE_TRANSACTION&&operation&&std::string_view(operation)=="COMMIT"){
            ++*static_cast<int*>(p);return SQLITE_DENY;
        }
        return SQLITE_OK;
    },&commits);
    EXPECT_THROW(wallet::OrchardDetachedRecoveryTestAccess::Prepare(wallet,session,**view,1,provider),std::runtime_error);
    sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_EQ(commits,1);EXPECT_EQ(points,0u);EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Snapshot(),before);
    auto plan=wallet::OrchardDetachedRecoveryTestAccess::Prepare(wallet,session,**view,1,provider);ASSERT_TRUE(plan);
    const auto restored_points=points;commits=0;
    sqlite3_commit_hook(db,[](void* p){++*static_cast<int*>(p);return 1;},&commits);
    EXPECT_THROW(wallet::OrchardDetachedRecoveryTestAccess::Commit(wallet,session,*plan,0),std::runtime_error);
    sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_EQ(commits,1);EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(points,restored_points);EXPECT_EQ(f.Snapshot(),before);
    EXPECT_THROW(wallet::OrchardDetachedRecoveryTestAccess::Commit(wallet,session,*plan,0),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);
    {
        auto lease=wallet.AcquireDatabaseLease();ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
        EXPECT_THROW(wallet::OrchardDetachedRecoveryTestAccess::Prepare(wallet,session,**view,1,provider),std::runtime_error);
        EXPECT_FALSE(sqlite3_get_autocommit(db));ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);
    }
    EXPECT_EQ(points,restored_points);EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(17).account.Delivery().sequence,(*view)->Head().sequence);
}
TEST(OrchardDetachedRecovery, ChangedCatalogAccountAndDeniedRecheckRefuseBeforeEffects){
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));f.Adopt();f.MineEmpty();
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    const auto session=f.Session();auto* db=wallet.getCurrentDatabase();
    const auto provider=[&](RuntimeOutboxCursor cursor){return (*view)->Point(cursor);};
    auto plan=wallet::OrchardDetachedRecoveryTestAccess::Prepare(wallet,session,**view,1,provider);
    const auto before=f.Snapshot();
    EXPECT_THROW(wallet::OrchardDetachedRecoveryTestAccess::Commit(wallet,session+1,*plan,0),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);
    ASSERT_FALSE(f.Issue(3).isMember("error"));const auto changed=f.Snapshot();
    EXPECT_THROW(wallet::OrchardDetachedRecoveryTestAccess::Commit(wallet,session,*plan,0),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),changed);
    plan=wallet::OrchardDetachedRecoveryTestAccess::Prepare(wallet,session,**view,1,provider);
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){
        return action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_retained"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_THROW(wallet::OrchardDetachedRecoveryTestAccess::Commit(wallet,session,*plan,0),std::runtime_error);
    sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Snapshot(),changed);
    plan=wallet::OrchardDetachedRecoveryTestAccess::Prepare(wallet,session,**view,1,provider);
    ASSERT_FALSE(f.Call(29).isMember("error"));const auto expanded=f.Snapshot();
    EXPECT_THROW(wallet::OrchardDetachedRecoveryTestAccess::Commit(wallet,session,*plan,0),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),expanded);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(3).account.Delivery().sequence,(*view)->Head().sequence);
    EXPECT_EQ(f.Account(29).account.Delivery().sequence,(*view)->Head().sequence);
}
TEST(OrchardDetachedRecovery, AscendingPartialCommitsRetainPrefixForOwnedRetry){
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));f.Adopt();f.MineEmpty();
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();
    const auto provider=[&](RuntimeOutboxCursor cursor){return (*view)->Point(cursor);};
    auto plan=wallet::OrchardDetachedRecoveryTestAccess::Prepare(wallet,session,**view,1,provider);
    const auto first=wallet::OrchardDetachedRecoveryTestAccess::Commit(wallet,session,*plan,0);
    EXPECT_EQ(first.account.Delivery().sequence,1u);EXPECT_EQ(f.Account(17).account.Delivery().sequence,0u);
    f.Sql("CREATE TRIGGER refuse_detached_account BEFORE UPDATE ON orchard_wallet_snapshots WHEN NEW.account=17 BEGIN SELECT RAISE(ABORT,'required account refusal'); END");
    const auto prefix=f.Snapshot();
    EXPECT_THROW(wallet::OrchardDetachedRecoveryTestAccess::Commit(wallet,session,*plan,1),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),prefix);EXPECT_EQ(f.Account(3).revision,first.revision);
    EXPECT_EQ(OrchardCreationBytes(f.Account(3).account),OrchardCreationBytes(first.account));
    EXPECT_EQ(f.Account(17).account.Delivery().sequence,0u);
    f.Sql("DROP TRIGGER refuse_detached_account");
    EXPECT_THROW(wallet::OrchardDetachedRecoveryTestAccess::Commit(wallet,session,*plan,1),std::runtime_error);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(3).account.Delivery().sequence,(*view)->Head().sequence);
    EXPECT_EQ(f.Account(17).account.Delivery().sequence,(*view)->Head().sequence);
}
TEST(OrchardDetachedRecovery, UndoReactivationPreservesArchiveAndNonretainingRevision){
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
        ASSERT_EQ(inventory.accounts[0].entry.account,3u);ASSERT_EQ(inventory.accounts[0].current.archive.size(),1u);
        archived_identity=inventory.accounts[0].current.archive[0].identity;tx.Commit();
    }
    const auto archive_bytes=[&]{
        auto lease=wallet.AcquireDatabaseLease();struct Statement{sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} q;
        OrchardAdmissionFixture::Require(sqlite3_prepare_v2(db,"SELECT revision,sealed FROM orchard_wallet_snapshots WHERE wallet_id=? AND account=?",-1,&q.p,nullptr)==SQLITE_OK);
        OrchardAdmissionFixture::Require(sqlite3_bind_blob(q.p,1,archived_identity.wallet_id.data(),32,SQLITE_TRANSIENT)==SQLITE_OK);
        OrchardAdmissionFixture::Require(sqlite3_bind_int64(q.p,2,archived_identity.account)==SQLITE_OK);
        OrchardAdmissionFixture::Require(sqlite3_step(q.p)==SQLITE_ROW);
        const auto revision=sqlite3_column_int64(q.p,0);const auto* bytes=static_cast<const char*>(sqlite3_column_blob(q.p,1));
        OrchardAdmissionFixture::Require(bytes&&sqlite3_column_bytes(q.p,1)>0);
        const std::string sealed(bytes,sqlite3_column_bytes(q.p,1));OrchardAdmissionFixture::Require(sqlite3_step(q.p)==SQLITE_DONE);
        return std::pair{revision,sealed};
    };
    const auto before=f.Account(3);const auto archived=archive_bytes();
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    const auto provider=[&](RuntimeOutboxCursor cursor){
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        return (*view)->Point(cursor);
    };
    auto plan=wallet::OrchardDetachedRecoveryTestAccess::Prepare(wallet,session,**view,(*view)->Head().sequence,provider);
    const auto restored=wallet::OrchardDetachedRecoveryTestAccess::Commit(wallet,session,*plan,0);
    // Undo retains the previous account; archive reactivation then performs
    // the established non-retaining replacement at the immediate parent tip.
    EXPECT_EQ(restored.revision,before.revision+2);EXPECT_EQ(archive_bytes(),archived);
    const auto& pending=restored.account.Operations().Entries();ASSERT_EQ(pending.size(),1u);
    EXPECT_EQ(pending.begin()->second.transaction,authorization.Orchard().CanonicalBytes());
    EXPECT_TRUE(restored.account.Observations().empty());
    {
        auto lease=wallet.AcquireDatabaseLease();auto seed=lease->CopyRecoverySeed(session);InventoryTestTransaction tx(db);
        const auto inventory=wallet::OrchardOwnershipInventory::Read(db,seed->Bytes());
        const auto& retained=inventory.accounts[0].retained_accounts;
        EXPECT_EQ(std::count_if(retained.begin(),retained.end(),[&](const auto& saved){return saved.revision==before.revision;}),1);
        EXPECT_EQ(std::count_if(retained.begin(),retained.end(),[&](const auto& saved){return saved.revision==before.revision+1;}),0);
        tx.Commit();
    }
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);EXPECT_EQ(archive_bytes(),archived);
    EXPECT_EQ(f.Account(3).revision,restored.revision);
}
} // namespace dinero
#endif
