#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero::wallet {
struct OrchardDetachedIssuanceTestAccess {
    static auto Issue(WalletManager& wallet,uint64_t session,const OrchardAccountDelivery::Profile& profile,
            const RuntimeAccountReplay& replay,bool creating,
            const std::function<OrchardAccountDelivery::RestorePoint(RuntimeOutboxCursor)>& points){
        return OrchardAccountDelivery::IssueCatalogWithRestorePoints(wallet,session,profile,replay,
            orchard::WalletScope::External,creating,points);
    }
};
}
namespace dinero {
TEST(OrchardDetachedIssuance, EmptyCreationAndFundedIssuanceRestoreWithoutOwners){
    OrchardCatalogRecoveryFixture f;
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();
    const auto session=f.Session();auto* db=wallet.getCurrentDatabase();
    auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    size_t points=0,origins=0;bool empty_creation=true;
    const auto provider=[&](RuntimeOutboxCursor cursor){
        ++points;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
        EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
        if(empty_creation)EXPECT_EQ(f.StorageTables(),0);
        auto point=(*view)->Point(cursor);const auto original=point.lookups.origin;
        point.lookups.origin=[&,original](uint32_t height,const uint256& hash,const orchard::Hash& txid){
            ++origins;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
            EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));return original(height,hash,txid);
        };
        return point;
    };
    const auto created=wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,session,{f.Domain(),102,3},**view,true,provider);
    EXPECT_GE(points,2u);EXPECT_EQ(created.revision,1u);ASSERT_EQ(f.Catalog()->accounts.size(),1u);
    const auto initial=f.Account(3);EXPECT_EQ(initial.account.Delivery().sequence,0u);
    EXPECT_EQ(initial.account.Scan().Checkpoint().block_hash,f.parent->hash);
    const auto expected_initial=wallet::OrchardAccountState::Begin(f.Domain(),f.AccountKeys(3).ExportFullViewingKey(),102,f.parent->hash)
        .IssueReceiver(orchard::WalletScope::External);
    EXPECT_EQ(created.address,expected_initial.second.EncodeAddress(orchard::WalletNetwork::Regtest));
    EXPECT_EQ(OrchardCreationBytes(initial.account),OrchardCreationBytes(expected_initial.first));
    empty_creation=false;ASSERT_FALSE(f.Call(17).isMember("error"));f.Adopt();
    const auto [body,bundle]=f.Shield(f.AccountKeys(3));(void)f.Mine(body);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    const auto before=f.Account(3),other=f.Account(17);const auto catalog=f.Catalog();
    const auto expected=before.account.IssueReceiver(orchard::WalletScope::External);
    const auto issued=wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,session,{f.Domain(),102,3},**view,false,provider);
    EXPECT_GT(origins,0u);EXPECT_EQ(issued.revision,before.revision+1);
    EXPECT_EQ(issued.address,expected.second.EncodeAddress(orchard::WalletNetwork::Regtest));
    EXPECT_EQ(OrchardCreationBytes(f.Account(3).account),OrchardCreationBytes(expected.first));
    EXPECT_EQ(OrchardCreationBytes(f.Account(17).account),OrchardCreationBytes(other.account));
    EXPECT_EQ(f.Account(17).revision,other.revision);EXPECT_EQ(f.Catalog(),catalog);
    EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
}
TEST(OrchardDetachedIssuance, CaptureAndWriterFailuresReturnNoAddressOrPartialSchema){
    OrchardCatalogRecoveryFixture f;const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();auto* db=wallet.getCurrentDatabase();
    size_t points=0;const auto provider=[&](RuntimeOutboxCursor cursor){++points;return (*view)->Point(cursor);};
    const auto create=[&]{return wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,session,{f.Domain(),102,3},**view,true,provider);};
    const auto initial=f.Snapshot();
    sqlite3_set_authorizer(db,[](void*,int action,const char* operation,const char*,const char*,const char*){
        return action==SQLITE_TRANSACTION&&operation&&std::string_view(operation)=="COMMIT"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_THROW((void)create(),std::runtime_error);sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_EQ(points,0u);EXPECT_EQ(f.StorageTables(),0);EXPECT_EQ(f.Snapshot(),initial);
    {DetachedCatalogSqlHooks hooks(db);hooks.refuse_final_commit=true;
     EXPECT_THROW((void)create(),std::runtime_error);EXPECT_EQ(hooks.commits,2u);}
    EXPECT_GT(points,0u);EXPECT_EQ(f.StorageTables(),0);EXPECT_EQ(f.Snapshot(),initial);
    ASSERT_NO_THROW((void)create());ASSERT_FALSE(f.Call(17).isMember("error"));const auto established=f.Snapshot();
    const auto issue=[&]{return wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,session,{f.Domain(),102,3},**view,false,provider);};
    {DetachedCatalogSqlHooks hooks(db);hooks.deny_final_read=true;EXPECT_THROW((void)issue(),std::runtime_error);EXPECT_EQ(hooks.commits,1u);}
    EXPECT_EQ(f.Snapshot(),established);
    {DetachedCatalogSqlHooks hooks(db);hooks.refuse_final_commit=true;EXPECT_THROW((void)issue(),std::runtime_error);EXPECT_EQ(hooks.commits,2u);}
    EXPECT_EQ(f.Snapshot(),established);
    f.Sql("CREATE TRIGGER corrupt_issuance_other AFTER UPDATE ON orchard_wallet_snapshots WHEN NEW.account=3 BEGIN UPDATE orchard_wallet_snapshots SET sealed=zeroblob(length(sealed)) WHERE account=17; END");
    EXPECT_THROW((void)issue(),std::runtime_error);EXPECT_EQ(f.Snapshot(),established);f.Sql("DROP TRIGGER corrupt_issuance_other");
    f.Sql("CREATE TRIGGER corrupt_creation_other AFTER INSERT ON orchard_wallet_snapshots WHEN NEW.account=29 BEGIN UPDATE orchard_wallet_snapshots SET sealed=zeroblob(length(sealed)) WHERE account=17; END");
    EXPECT_TRUE(f.Call(29).isMember("error"));EXPECT_EQ(f.Snapshot(),established);f.Sql("DROP TRIGGER corrupt_creation_other");
    EXPECT_NO_THROW((void)issue());EXPECT_FALSE(f.Call(29).isMember("error"));
    EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
}
TEST(OrchardDetachedIssuance, ChangedCatalogOrOtherAccountRefusesPreparedResult){
    OrchardCatalogRecoveryFixture f;const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();
    bool changed=false;std::string changed_bytes;
    const auto grow=[&](RuntimeOutboxCursor cursor){
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
        if(!changed){changed=true;const auto created=f.Call(17);EXPECT_FALSE(created.isMember("error"));changed_bytes=f.Snapshot();}
        return (*view)->Point(cursor);
    };
    EXPECT_THROW((void)wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,session,{f.Domain(),102,3},**view,true,grow),wallet::OrchardAccountDelivery::CatalogChanged);
    EXPECT_TRUE(changed);EXPECT_EQ(f.Snapshot(),changed_bytes);ASSERT_EQ(f.Catalog()->accounts.size(),1u);EXPECT_EQ(f.Catalog()->accounts[0].account,17u);
    ASSERT_FALSE(f.Call(3).isMember("error"));const auto requested=f.Account(3);changed=false;
    const auto mutate=[&](RuntimeOutboxCursor cursor){
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));
        if(!changed){changed=true;const auto issued=f.Issue(17);EXPECT_FALSE(issued.isMember("error"));changed_bytes=f.Snapshot();}
        return (*view)->Point(cursor);
    };
    EXPECT_THROW((void)wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,session,{f.Domain(),102,3},**view,false,mutate),wallet::OrchardAccountDelivery::CatalogChanged);
    EXPECT_TRUE(changed);EXPECT_EQ(f.Snapshot(),changed_bytes);EXPECT_EQ(f.Account(3).revision,requested.revision);
    EXPECT_EQ(OrchardCreationBytes(f.Account(3).account),OrchardCreationBytes(requested.account));
    EXPECT_FALSE(f.Issue(3).isMember("error"));
}
TEST(OrchardDetachedIssuance, CallerLeaseTransactionLockAndSourceRefusalPreserveOwners){
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();auto* db=wallet.getCurrentDatabase();
    const auto before=f.Snapshot();size_t points=0;
    const auto provider=[&](RuntimeOutboxCursor cursor){++points;return (*view)->Point(cursor);};
    const auto call=[&](bool creating){return wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,session,{f.Domain(),102,creating?17u:3u},**view,creating,provider);};
    {auto lease=wallet.AcquireDatabaseLease();EXPECT_THROW((void)call(false),std::runtime_error);
    EXPECT_THROW((void)call(true),std::runtime_error);}
    EXPECT_EQ(points,0u);ASSERT_EQ(sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW((void)call(false),std::runtime_error);EXPECT_FALSE(sqlite3_get_autocommit(db));
    ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);EXPECT_EQ(points,0u);EXPECT_EQ(f.Snapshot(),before);
    wallet.lockWallet();EXPECT_THROW((void)call(false),std::runtime_error);
    EXPECT_THROW((void)call(true),std::runtime_error);EXPECT_EQ(points,0u);
    wallet.unlockWallet("canonical-fixture-pass",0);EXPECT_EQ(f.Snapshot(),before);
    const auto refused=[&](RuntimeOutboxCursor)->wallet::OrchardAccountDelivery::RestorePoint{
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));throw std::runtime_error("fixture source unavailable");
    };
    EXPECT_THROW((void)wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,session,{f.Domain(),102,3},**view,false,refused),std::runtime_error);
    EXPECT_THROW((void)wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,session,{f.Domain(),102,17},**view,true,refused),std::runtime_error);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));EXPECT_TRUE(sqlite3_get_autocommit(db));
    EXPECT_NO_THROW((void)call(false));
    EXPECT_NO_THROW((void)call(true));
}
TEST(OrchardDetachedIssuance, ArchivedSignedOperationsAndOtherAccountsRemainExact){
    OrchardSpendRequestFixture f;auto request=f.Reserve();const auto authorization=f.Prove(request);
    (void)f.Ready(1,request.revision,authorization);(void)f.Mine(MempoolTransaction::FromOrchard(authorization.Transaction()));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);f.ArchiveCompleted();
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto& wallet=use->Wallet();const auto session=f.Session();
    const auto before=f.Account(3),other=f.Account(17);const auto catalog=f.Catalog();
    orchard::WalletStorageIdentity archived_identity;
    {auto lease=wallet.AcquireDatabaseLease();auto seed=lease->CopyRecoverySeed(session);InventoryTestTransaction tx(lease->Database());
     const auto inventory=wallet::OrchardOwnershipInventory::Read(lease->Database(),seed->Bytes());
     ASSERT_EQ(inventory.accounts[0].current.archive.size(),1u);
     archived_identity=inventory.accounts[0].current.archive[0].identity;tx.Commit();}
    const auto archive_bytes=[&]{
        auto lease=wallet.AcquireDatabaseLease();struct Statement{sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} q;
        OrchardAdmissionFixture::Require(sqlite3_prepare_v2(lease->Database(),"SELECT revision,sealed FROM orchard_wallet_snapshots WHERE wallet_id=? AND account=?",-1,&q.p,nullptr)==SQLITE_OK);
        OrchardAdmissionFixture::Require(sqlite3_bind_blob(q.p,1,archived_identity.wallet_id.data(),32,SQLITE_TRANSIENT)==SQLITE_OK);
        OrchardAdmissionFixture::Require(sqlite3_bind_int64(q.p,2,archived_identity.account)==SQLITE_OK);
        OrchardAdmissionFixture::Require(sqlite3_step(q.p)==SQLITE_ROW);
        const auto revision=sqlite3_column_int64(q.p,0);const auto* bytes=static_cast<const char*>(sqlite3_column_blob(q.p,1));
        OrchardAdmissionFixture::Require(bytes&&sqlite3_column_bytes(q.p,1)>0);const std::string sealed(bytes,sqlite3_column_bytes(q.p,1));
        OrchardAdmissionFixture::Require(sqlite3_step(q.p)==SQLITE_DONE);return std::pair{revision,sealed};
    };
    const auto archived=archive_bytes();
    size_t origins=0;const auto provider=[&](RuntimeOutboxCursor cursor){
        EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));auto point=(*view)->Point(cursor);const auto original=point.lookups.origin;
        point.lookups.origin=[&,original](uint32_t height,const uint256& hash,const orchard::Hash& txid){
            ++origins;EXPECT_TRUE(WalletDetachedReadTestAccess::Released(wallet));return original(height,hash,txid);
        };return point;
    };
    const auto expected=before.account.IssueReceiver(orchard::WalletScope::External);
    const auto issued=wallet::OrchardDetachedIssuanceTestAccess::Issue(wallet,session,{f.Domain(),102,3},**view,false,provider);
    EXPECT_GT(origins,0u);EXPECT_EQ(issued.revision,before.revision+1);EXPECT_EQ(issued.address,expected.second.EncodeAddress(orchard::WalletNetwork::Regtest));
    EXPECT_EQ(OrchardCreationBytes(f.Account(3).account),OrchardCreationBytes(expected.first));
    EXPECT_EQ(OrchardCreationBytes(f.Account(17).account),OrchardCreationBytes(other.account));EXPECT_EQ(f.Account(17).revision,other.revision);
    EXPECT_EQ(f.Catalog(),catalog);EXPECT_EQ(archive_bytes(),archived);
}
} // namespace dinero
#endif
