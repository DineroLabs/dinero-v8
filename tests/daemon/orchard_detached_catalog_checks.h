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
