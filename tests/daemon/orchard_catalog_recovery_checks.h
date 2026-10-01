#pragma once
#include "wallet/runtime_wallet_recovery.h"
#include <tuple>
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
struct RuntimeCatalogRecoveryTestAccess {
    static auto Resume(const RuntimeAccountReplay& view,const std::function<RuntimeOutboxPage(RuntimeOutboxCursor,size_t)>& source,
            WalletManager& wallet,UTXOIndex& index,uint64_t session){
        return RuntimeWalletRecovery::ResumeAccounts(view,source,wallet,index,session,std::nullopt,true);
    }
};
namespace {
struct OrchardCatalogRecoveryFixture : OrchardCreationFixture {
    void Adopt(){auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.service);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();const auto origin=f.service->getRuntimeWalletOrigin(use->Wallet(),session,&index->Index());
        OrchardAdmissionFixture::Require(origin.ok());OrchardAdmissionFixture::Require(f.service->adoptRuntimeWalletOrigin(use->Wallet(),index->Index(),**origin)==Status::Ok);
    }
    auto Progress(){auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.service);const auto session=Session();
        const auto a=RuntimeIndexDelivery::ReadForWallet(use->Wallet(),index->Index(),session);const auto b=RuntimeOrdinaryDelivery::ReadForWallet(use->Wallet(),session);
        OrchardAdmissionFixture::Require(bool(a)&&bool(b));return std::make_tuple(a->cursor,a->origin_hash,a->origin_height,a->tip_hash,a->tip_height,b->cursor,b->origin_hash,b->origin_height,b->tip_hash,b->tip_height);
    }
    auto Owned(){const auto view=f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());auto use=WalletService::AcquireWalletUse(wallet);
        return wallet::OrchardAccountDelivery::ReadCatalogForReplay(use->Wallet(),Session(),**view);
    }
    auto Recover(){auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.service);
        return RuntimeWalletRecovery::ResumeCatalogWalletStores(*f.service,use->Wallet(),index->Index(),Session());
    }
    int StorageTables(){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        struct Statement{sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} q;
        OrchardAdmissionFixture::Require(sqlite3_prepare_v2(lease->Database(),"SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('orchard_wallet_schema','orchard_wallet_snapshots','orchard_wallet_retained')",-1,&q.p,nullptr)==SQLITE_OK);
        OrchardAdmissionFixture::Require(sqlite3_step(q.p)==SQLITE_ROW);const auto count=sqlite3_column_int(q.p,0);OrchardAdmissionFixture::Require(sqlite3_step(q.p)==SQLITE_DONE);return count;
    }
};
}
TEST(OrchardCatalogRecovery, GeneratedEmptyReplaysWithoutSchemaOrAccountCreation){
    OrchardCatalogRecoveryFixture f;ASSERT_EQ(f.StorageTables(),0);const auto catalog=f.Catalog();ASSERT_TRUE(catalog&&catalog->generated&&catalog->accounts.empty());
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);EXPECT_EQ(f.StorageTables(),0);
    f.Adopt();f.MineEmpty();const auto owned=f.Owned();EXPECT_TRUE(owned.accounts.empty());EXPECT_EQ(owned.catalog,*catalog);EXPECT_EQ(f.StorageTables(),0);
    const auto recovered=f.Recover();EXPECT_EQ(recovered.catalog_revision,std::optional<uint64_t>(catalog->revision));EXPECT_TRUE(recovered.account_revisions.empty());EXPECT_EQ(recovered.applied.cursor.sequence,2u);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);EXPECT_EQ(f.Catalog(),catalog);EXPECT_EQ(f.StorageTables(),0);
    const auto progress=f.Progress();
    f.Sql("CREATE TABLE orchard_wallet_retained(wallet_id BLOB,account INTEGER,revision INTEGER,sealed BLOB)");auto before=f.Snapshot();
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);EXPECT_EQ(f.Progress(),progress);EXPECT_EQ(f.Snapshot(),before);
    f.Sql("DROP TABLE orchard_wallet_retained");EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    f.EnrollAccount();before=f.Snapshot();EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.Progress(),progress);EXPECT_EQ(f.Catalog(),catalog);
}
TEST(OrchardCatalogRecovery, NonconsecutiveCatalogOwnersRecoverShieldUndoAndReopen){
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));const auto catalog=f.Catalog();f.Adopt();
    const auto [body,bundle]=f.Shield(f.AccountKeys(17));(void)f.Mine(body);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(3).account.Scan().BalanceUna(),0u);EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),500000u);
    auto recovered=f.Recover();ASSERT_EQ(recovered.account_revisions.size(),2u);EXPECT_EQ(recovered.account_revisions[0].first,3u);EXPECT_EQ(recovered.account_revisions[1].first,17u);EXPECT_EQ(recovered.catalog_revision,std::optional<uint64_t>(catalog->revision));EXPECT_EQ(recovered.applied.cursor.sequence,2u);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),500000u);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),0u);EXPECT_EQ(f.Account(3).account.Delivery().sequence,3u);EXPECT_EQ(f.Account(17).account.Delivery().sequence,3u);EXPECT_EQ(f.Catalog(),catalog);
}
TEST(OrchardCatalogRecovery, MissingOtherOwnerAndUnknownCatalogRefuseBeforeEffects){
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));f.Adopt();f.MineEmpty();
    const auto progress=f.Progress();const auto catalog=f.Setting("orchard_account_catalog_v1");
    const auto refuse=[&](){const auto bytes=f.Snapshot();EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);EXPECT_EQ(f.Progress(),progress);EXPECT_EQ(f.Snapshot(),bytes);};
    f.Sql("CREATE TEMP TABLE saved_recovery_owner AS SELECT * FROM orchard_wallet_snapshots WHERE account=17");
    f.Sql("DELETE FROM orchard_wallet_snapshots WHERE account=17");refuse();f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_recovery_owner");
    f.Sql("UPDATE orchard_wallet_snapshots SET sealed=zeroblob(length(sealed)) WHERE account=17");refuse();
    f.Sql("DELETE FROM orchard_wallet_snapshots WHERE account=17; INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_recovery_owner; DROP TABLE saved_recovery_owner");
    f.Sql("DELETE FROM settings WHERE key='orchard_account_catalog_v1'");refuse();
    auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().setSetting("orchard_account_catalog_v1","00");refuse();use->Wallet().setSetting("orchard_account_catalog_v1",catalog);
    sqlite3* db;{auto lease=use->Wallet().AcquireDatabaseLease();db=lease->Database();}
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){return action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_retained"?SQLITE_DENY:SQLITE_OK;},nullptr);
    const auto denied=f.wallet->RecoverActiveWalletFromCanonicalSource();sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_EQ(denied,Recovery::Deferred);EXPECT_EQ(f.Progress(),progress);
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto session=f.Session();auto borrowed_use=WalletService::AcquireWalletUse(f.wallet);auto borrowed_lease=borrowed_use->Wallet().AcquireDatabaseLease();f.Sql("BEGIN IMMEDIATE");
    EXPECT_THROW(wallet::OrchardAccountDelivery::ReadCatalogForReplay(use->Wallet(),session,**view),std::runtime_error);EXPECT_FALSE(sqlite3_get_autocommit(db));f.Sql("ROLLBACK");borrowed_lease.reset();borrowed_use.reset();EXPECT_EQ(f.Progress(),progress);
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);EXPECT_EQ(f.Account(17).account.Delivery().sequence,2u);
    // Authenticate recovery-mode inventory directly; no owner/schema generation.
    use->Wallet().createFromBip39("unknown-recovery","abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about","");
    const auto before=f.Snapshot();
    EXPECT_THROW(wallet::OrchardAccountDelivery::ReadCatalogForReplay(use->Wallet(),f.Session(),**view),std::runtime_error);EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardCatalogRecovery, PartialAccountCommitAndCatalogGrowthRequireOwnedRetry){
    OrchardCatalogRecoveryFixture f;ASSERT_FALSE(f.Call(3).isMember("error"));ASSERT_FALSE(f.Call(17).isMember("error"));f.Adopt();f.MineEmpty();const auto catalog=f.Catalog();
    f.Sql("CREATE TRIGGER refuse_catalog_recovery BEFORE UPDATE ON orchard_wallet_snapshots WHEN NEW.account=17 BEGIN SELECT RAISE(ABORT,'required account refusal'); END");
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::Deferred);EXPECT_EQ(f.Account(3).account.Delivery().sequence,1u);EXPECT_EQ(f.Account(17).account.Delivery().sequence,0u);EXPECT_EQ(std::get<0>(f.Progress()).sequence,1u);EXPECT_EQ(std::get<5>(f.Progress()).sequence,1u);EXPECT_EQ(f.Catalog(),catalog);
    f.Sql("DROP TRIGGER refuse_catalog_recovery");EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);EXPECT_EQ(f.Account(3).account.Delivery().sequence,2u);EXPECT_EQ(f.Account(17).account.Delivery().sequence,2u);
    f.MineEmpty();const auto progress=f.Progress();const auto first=f.Account(3);const auto other=f.Account(17);const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);const auto session=f.Session();bool changed=false;
    const auto source=[&](RuntimeOutboxCursor cursor,size_t count){
        if(!changed){changed=true;const auto created=f.Call(29);OrchardAdmissionFixture::Require(!created.isMember("error"));}
        const auto page=f.f.service->getRuntimeDeliveryPage(cursor,count);OrchardAdmissionFixture::Require(page.ok());return **page;
    };
    EXPECT_THROW(RuntimeCatalogRecoveryTestAccess::Resume(**view,source,use->Wallet(),index->Index(),session),std::runtime_error);EXPECT_TRUE(changed);EXPECT_EQ(f.Progress(),progress);
    EXPECT_EQ(OrchardCreationBytes(f.Account(3).account),OrchardCreationBytes(first.account));EXPECT_EQ(f.Account(3).revision,first.revision);EXPECT_EQ(OrchardCreationBytes(f.Account(17).account),OrchardCreationBytes(other.account));EXPECT_EQ(f.Account(17).revision,other.revision);
    const auto after=f.Catalog();ASSERT_TRUE(after);EXPECT_EQ(after->revision,catalog->revision+1);ASSERT_EQ(after->accounts.size(),3u);
    const auto retry=f.Recover();EXPECT_EQ(retry.catalog_revision,std::optional<uint64_t>(after->revision));EXPECT_EQ(retry.account_revisions.size(),3u);EXPECT_EQ(retry.applied.cursor.sequence,3u);EXPECT_EQ(f.Account(29).account.Delivery().sequence,3u);
}
} // namespace dinero
#endif
