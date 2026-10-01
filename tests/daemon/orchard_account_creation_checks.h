#pragma once
#include "wallet/orchard_account_catalog.h"
#include "rpc/orchard_account_rpc.h"
namespace dinero {
namespace {
din::Json OrchardCreationRequest(uint32_t account=3){din::Json p;p["account"]=Json::UInt64(account);return p;}
}
TEST(OrchardAccountCreation, MalformedRequestsRefuseBeforeServices){
    ExecutionContext context;std::vector<din::Json> invalid{din::arr(),din::Json{},din::Json("3")};
    for(const auto& value:std::vector<din::Json>{din::Json(-1),din::Json(0.5),din::Json(true),din::Json("3"),din::Json(Json::UInt64(0x80000000ULL))}){din::Json p;p["account"]=value;invalid.push_back(p);}
    auto extra=OrchardCreationRequest();extra["replace_existing"]=true;invalid.push_back(extra);
    for(const auto& p:invalid){const auto result=rpc_context_wallet_orchard_createaccount(context,p);EXPECT_TRUE(result.isMember("error"));EXPECT_FALSE(result.isMember("address"));EXPECT_FALSE(result.isMember("revision"));}
}
TEST(OrchardAccountCreation, RegistryAndBackendOrServiceAbsenceRefuse){
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.createaccount");ASSERT_NE(method,nullptr);
    ExecutionContext context;const auto result=(*method)(context,OrchardCreationRequest());EXPECT_TRUE(result.isMember("error"));EXPECT_FALSE(result.isMember("address"));
#ifndef DINERO_TEST_ORCHARD_ORIGIN
    EXPECT_EQ(result["error"].asString(),"Orchard wallet backend unavailable");
#endif
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct OrchardCreationFixture : CanonicalRecoveryFixture {
    OrchardCreationFixture():CanonicalRecoveryFixture(true){MineEmpty();}
    ExecutionContext RequestContext(){ExecutionContext c;c.daemon=&context;c.walletName="canonical-recovery";return c;}
    din::Json Call(uint32_t account=3){return rpc_context_wallet_orchard_createaccount(RequestContext(),OrchardCreationRequest(account));}
    din::Json Issue(uint32_t account=3){return rpc_context_wallet_orchard_getnewaddress(RequestContext(),OrchardCreationRequest(account));}
    void Sql(const std::string& text){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();OrchardAdmissionFixture::Require(sqlite3_exec(lease->Database(),text.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK);}
    std::string Setting(const char* key){auto use=WalletService::AcquireWalletUse(wallet);return use->Wallet().getSetting(key);}
    auto Catalog(){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();auto seed=lease->CopyRecoverySeed(lease->Session());auto* db=lease->Database();
        OrchardAdmissionFixture::Require(sqlite3_exec(db,"BEGIN",nullptr,nullptr,nullptr)==SQLITE_OK);
        try{auto result=wallet::OrchardAccountCatalog::Read(db,seed->Bytes());OrchardAdmissionFixture::Require(sqlite3_exec(db,"COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK);return result;}
        catch(...){OrchardAdmissionFixture::Require(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK);throw;}}
    auto Account(uint32_t number=3){const auto view=f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());auto use=WalletService::AcquireWalletUse(wallet);
        const auto session=use->Wallet().AcquireDatabaseLease()->Session();return wallet::OrchardAccountDelivery::ReadForReplay(use->Wallet(),session,{Domain(),102,number},**view);}
    auto AccountKeys(uint32_t number=3){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();auto seed=lease->CopyRecoverySeed(lease->Session());return orchard::WalletKeys::FromSeed(seed->Bytes(),number);}
    std::string Snapshot(){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();auto* db=lease->Database();std::string result;
        const auto query=[&](const char* text){struct Statement{sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} q;OrchardAdmissionFixture::Require(sqlite3_prepare_v2(db,text,-1,&q.p,nullptr)==SQLITE_OK);int rc;
            while((rc=sqlite3_step(q.p))==SQLITE_ROW){for(int i=0;i<sqlite3_column_count(q.p);++i){const auto* bytes=sqlite3_column_text(q.p,i);if(bytes)result.append(reinterpret_cast<const char*>(bytes),sqlite3_column_bytes(q.p,i));result+='|';}result+='\n';}
            OrchardAdmissionFixture::Require(rc==SQLITE_DONE);};
        query("SELECT hex(key),hex(value),updated_at FROM settings ORDER BY key");query("SELECT hex(encrypted_seed),encryption_version FROM hd_seeds ORDER BY id");query("SELECT hex(runtime_delivery_id) FROM wallet_meta WHERE id=1");
        query("SELECT name,sql FROM sqlite_master WHERE type='table' AND name IN ('orchard_wallet_schema','orchard_wallet_snapshots','orchard_wallet_retained') ORDER BY name");
        for(const auto& table:std::vector<std::string>{"orchard_wallet_snapshots","orchard_wallet_retained"}){
            struct Statement{sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} q;const auto text="SELECT 1 FROM sqlite_master WHERE type='table' AND name='"+table+"'";
            OrchardAdmissionFixture::Require(sqlite3_prepare_v2(db,text.c_str(),-1,&q.p,nullptr)==SQLITE_OK);const auto rc=sqlite3_step(q.p);
            if(rc==SQLITE_ROW){OrchardAdmissionFixture::Require(sqlite3_step(q.p)==SQLITE_DONE);query(("SELECT hex(wallet_id),account,revision,hex(sealed) FROM "+table+" ORDER BY wallet_id,account,revision").c_str());}else OrchardAdmissionFixture::Require(rc==SQLITE_DONE);
        }return result;
    }
    void ReplayAccount(uint32_t number=3){const auto view=f.service->getRuntimeAccountReplay();OrchardAdmissionFixture::Require(view.ok());auto current=Account(number);auto use=WalletService::AcquireWalletUse(wallet);const auto session=use->Wallet().AcquireDatabaseLease()->Session();
        for(uint64_t sequence=current.account.Delivery().sequence+1;sequence<=(*view)->Head().sequence;++sequence)
            current=wallet::OrchardAccountDelivery::ApplyForReplay(use->Wallet(),session,{Domain(),102,number},current.revision,**view,sequence);
    }
};
std::vector<uint8_t> OrchardCreationBytes(const wallet::OrchardAccountState& state){auto encoded=state.Encode();return {encoded.Bytes().begin(),encoded.Bytes().end()};}
}
TEST(OrchardAccountCreation, ActualRpcAllocatesNonconsecutiveOwnersAtOriginAndReopens){
    OrchardCreationFixture f;const auto initial=f.Catalog();ASSERT_TRUE(initial);ASSERT_TRUE(initial->generated);ASSERT_TRUE(initial->accounts.empty());
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.createaccount");ASSERT_NE(method,nullptr);
    const auto first=(*method)(f.RequestContext(),OrchardCreationRequest());ASSERT_FALSE(first.isMember("error"))<<first["error"].asString();EXPECT_EQ(first["revision"].asUInt64(),1u);EXPECT_TRUE(first["requires_sync"].asBool());
    const auto account=f.Account();EXPECT_EQ(account.account.Delivery().sequence,0u);EXPECT_EQ(account.account.Scan().Checkpoint().height,101u);EXPECT_EQ(account.account.Scan().Checkpoint().block_hash,f.parent->hash);EXPECT_EQ(account.account.Scan().BalanceUna(),0u);
    const auto keys=f.AccountKeys();const auto expected=wallet::OrchardAccountState::Begin(f.Domain(),keys.ExportFullViewingKey(),102,f.parent->hash).IssueReceiver(orchard::WalletScope::External);
    EXPECT_EQ(first["address"].asString(),expected.second.EncodeAddress(orchard::WalletNetwork::Regtest));EXPECT_EQ(OrchardCreationBytes(account.account),OrchardCreationBytes(expected.first));
    auto catalog=f.Catalog();ASSERT_TRUE(catalog);ASSERT_EQ(catalog->accounts.size(),1u);EXPECT_EQ(catalog->accounts[0].account,3u);EXPECT_EQ(catalog->revision,2u);
    const auto untouched=OrchardCreationBytes(account.account);const auto second=f.Call(17);ASSERT_FALSE(second.isMember("error"));EXPECT_NE(second["address"].asString(),first["address"].asString());EXPECT_EQ(OrchardCreationBytes(f.Account().account),untouched);
    catalog=f.Catalog();ASSERT_EQ(catalog->accounts.size(),2u);EXPECT_EQ(catalog->accounts[0].account,3u);EXPECT_EQ(catalog->accounts[1].account,17u);EXPECT_EQ(catalog->revision,3u);const auto bytes=f.Snapshot();EXPECT_TRUE(f.Call(3).isMember("error"));EXPECT_EQ(f.Snapshot(),bytes);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");}
    EXPECT_TRUE(f.Call(29).isMember("error"));
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());const auto next=f.Issue();ASSERT_FALSE(next.isMember("error"));EXPECT_NE(next["address"].asString(),first["address"].asString());EXPECT_EQ(f.Catalog(),catalog);
}
TEST(OrchardAccountCreation, RequiredWritesCommitAndBorrowedOwnerPreserveCatalogAndRows){
    OrchardCreationFixture f;const auto initial=f.Snapshot();f.Sql("CREATE TRIGGER refuse_creation BEFORE UPDATE ON settings WHEN NEW.key='orchard_account_catalog_v1' BEGIN SELECT RAISE(ABORT,'catalog refusal'); END");
    auto denied=f.Call();EXPECT_TRUE(denied.isMember("error"));EXPECT_FALSE(denied.isMember("address"));EXPECT_EQ(f.Snapshot(),initial);f.Sql("DROP TRIGGER refuse_creation");
    auto use=WalletService::AcquireWalletUse(f.wallet);sqlite3* db;{auto lease=use->Wallet().AcquireDatabaseLease();db=lease->Database();}
    bool commit_seen=false;sqlite3_commit_hook(db,[](void* p){*static_cast<bool*>(p)=true;return 1;},&commit_seen);denied=f.Call();sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_TRUE(commit_seen);EXPECT_TRUE(denied.isMember("error"));EXPECT_EQ(f.Snapshot(),initial);
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto session=f.Session();
    auto borrowed_use=WalletService::AcquireWalletUse(f.wallet);auto borrowed_lease=borrowed_use->Wallet().AcquireDatabaseLease();f.Sql("BEGIN IMMEDIATE");
    EXPECT_THROW(wallet::OrchardAccountDelivery::CreateAccountForReplay(use->Wallet(),session,{f.Domain(),102,3},**view),std::runtime_error);EXPECT_FALSE(sqlite3_get_autocommit(db));f.Sql("ROLLBACK");borrowed_lease.reset();borrowed_use.reset();EXPECT_EQ(f.Snapshot(),initial);
    EXPECT_THROW(wallet::OrchardAccountDelivery::CreateAccountForReplay(use->Wallet(),session+1,{f.Domain(),102,3},**view),std::runtime_error);auto wrong=f.Domain();wrong.branch_id^=1;
    EXPECT_THROW(wallet::OrchardAccountDelivery::CreateAccountForReplay(use->Wallet(),session,{wrong,102,3},**view),std::runtime_error);EXPECT_EQ(f.Snapshot(),initial);
    ASSERT_FALSE(f.Call().isMember("error"));const auto established=f.Snapshot();
    f.Sql("CREATE TRIGGER refuse_creation BEFORE INSERT ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'snapshot refusal'); END");denied=f.Call(17);EXPECT_TRUE(denied.isMember("error"));EXPECT_EQ(f.Snapshot(),established);f.Sql("DROP TRIGGER refuse_creation");
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){return action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_snapshots"?SQLITE_DENY:SQLITE_OK;},nullptr);
    denied=f.Call(17);sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(denied.isMember("error"));EXPECT_EQ(f.Snapshot(),established);ASSERT_FALSE(f.Call(17).isMember("error"));
}
TEST(OrchardAccountCreation, DeletedDeclaredForeignAndCorruptRetainedOwnersNeverRecreate){
    OrchardCreationFixture f;ASSERT_FALSE(f.Call().isMember("error"));ASSERT_FALSE(f.Issue().isMember("error"));const auto catalog=f.Setting("orchard_account_catalog_v1");
    f.Sql("CREATE TEMP TABLE saved_creation_rows AS SELECT * FROM orchard_wallet_snapshots");f.Sql("DELETE FROM orchard_wallet_snapshots");auto before=f.Snapshot();EXPECT_TRUE(f.Call(3).isMember("error"));EXPECT_TRUE(f.Call(17).isMember("error"));EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.Setting("orchard_account_catalog_v1"),catalog);
    f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_creation_rows");f.Sql("DROP TABLE saved_creation_rows");
    f.Sql("INSERT INTO orchard_wallet_snapshots(wallet_id,account,revision,sealed) SELECT zeroblob(32),account,revision,sealed FROM orchard_wallet_snapshots LIMIT 1");before=f.Snapshot();EXPECT_TRUE(f.Call(17).isMember("error"));EXPECT_EQ(f.Snapshot(),before);f.Sql("DELETE FROM orchard_wallet_snapshots WHERE wallet_id=zeroblob(32)");
    f.Sql("CREATE TEMP TABLE saved_creation_retained AS SELECT * FROM orchard_wallet_retained");f.Sql("UPDATE orchard_wallet_retained SET sealed=zeroblob(length(sealed))");before=f.Snapshot();EXPECT_TRUE(f.Call(17).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    f.Sql("DELETE FROM orchard_wallet_retained; INSERT INTO orchard_wallet_retained SELECT * FROM saved_creation_retained; DROP TABLE saved_creation_retained");ASSERT_FALSE(f.Call(17).isMember("error"));
}
TEST(OrchardAccountCreation, UnknownRecoveryMissingCatalogAndUntrackedRowsNeverInitialize){
    OrchardCreationFixture f;auto use=WalletService::AcquireWalletUse(f.wallet);const auto good=use->Wallet().getSetting("orchard_account_catalog_v1");
    f.Sql("DELETE FROM settings WHERE key='orchard_account_catalog_v1'");auto before=f.Snapshot();EXPECT_TRUE(f.Call().isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    use->Wallet().setSetting("orchard_account_catalog_v1","00");before=f.Snapshot();EXPECT_TRUE(f.Call().isMember("error"));EXPECT_EQ(f.Snapshot(),before);use->Wallet().setSetting("orchard_account_catalog_v1",good);
    // Explicit pre-catalog fixture enrollment cannot be certified as empty.
    f.EnrollAccount();before=f.Snapshot();EXPECT_TRUE(f.Call().isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    use->Wallet().createFromBip39("recovered-catalog","abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about","");
    auto recovered=f.Catalog();ASSERT_TRUE(recovered);EXPECT_FALSE(recovered->generated);auto c=f.RequestContext();c.walletName="recovered-catalog";before=f.Snapshot();
    const auto denied=rpc_context_wallet_orchard_createaccount(c,OrchardCreationRequest());EXPECT_TRUE(denied.isMember("error"));EXPECT_FALSE(denied.isMember("address"));EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardAccountCreation, CreatedOwnerReplaysRealShieldAndUndoWithoutCatalogReset){
    OrchardCreationFixture f;ASSERT_FALSE(f.Call().isMember("error"));const auto catalog=f.Setting("orchard_account_catalog_v1");const auto keys=f.AccountKeys();const auto [body,bundle]=f.Shield(keys);(void)f.Mine(body);
    f.ReplayAccount();auto account=f.Account();EXPECT_EQ(account.account.Scan().BalanceUna(),500000u);EXPECT_EQ(account.account.Delivery().sequence,2u);EXPECT_EQ(f.Setting("orchard_account_catalog_v1"),catalog);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));f.ReplayAccount();account=f.Account();EXPECT_EQ(account.account.Scan().BalanceUna(),0u);EXPECT_EQ(account.account.Delivery().sequence,3u);EXPECT_EQ(f.Setting("orchard_account_catalog_v1"),catalog);
    // A missing recorded predecessor refuses another allocation rather than
    // creating a new account while losing the existing undo owner.
    const auto before=account.account.ParentSnapshotRevision();ASSERT_GT(before,0u);
    f.Sql("CREATE TEMP TABLE saved_creation_parent AS SELECT * FROM orchard_wallet_retained WHERE revision="+std::to_string(before));
    f.Sql("DELETE FROM orchard_wallet_retained WHERE revision="+std::to_string(before));const auto missing=f.Snapshot();EXPECT_TRUE(f.Call(17).isMember("error"));EXPECT_EQ(f.Snapshot(),missing);
    f.Sql("INSERT INTO orchard_wallet_retained SELECT * FROM saved_creation_parent; DROP TABLE saved_creation_parent");ASSERT_FALSE(f.Call(17).isMember("error"));EXPECT_EQ(f.Account().account.Scan().BalanceUna(),0u);
}
#endif
} // namespace dinero
