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
TEST(OrchardPreactivationEnrollment, CallerCannotSupplyBirthdayCheckpointOrProfile){
    ExecutionContext c;
    for(const char* key:{"birthday_height","height","checkpoint_hash","account","network"}){
        din::Json p;p[key]=0;const auto denied=rpc_context_wallet_orchard_enrollpreactivation(c,p);
        EXPECT_TRUE(denied.isMember("error"));EXPECT_FALSE(denied.isMember("enrolled"));
    }
}
TEST(OrchardAccountCreation, RegistryAndBackendOrServiceAbsenceRefuse){
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.createaccount");ASSERT_NE(method,nullptr);
    ExecutionContext context;const auto result=(*method)(context,OrchardBoundParamsForTest(context,OrchardCreationRequest()));EXPECT_TRUE(result.isMember("error"));EXPECT_FALSE(result.isMember("address"));
#ifndef DINERO_TEST_ORCHARD_ORIGIN
    EXPECT_EQ(result["error"].asString(),"Orchard wallet backend unavailable");
#endif
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct OrchardCreationFixture : CanonicalRecoveryFixture {
    explicit OrchardCreationFixture(bool mine=true):CanonicalRecoveryFixture(true){if(mine)MineEmpty();}
    ExecutionContext RequestContext(){ExecutionContext c;c.daemon=&context;auto use=WalletService::AcquireWalletUse(wallet);c.walletName=use->Wallet().getCurrentWalletName();return c;}
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
    const auto first=(*method)(f.RequestContext(),OrchardBoundParamsForTest(f.RequestContext(),OrchardCreationRequest()));ASSERT_FALSE(first.isMember("error"))<<first["error"].asString();EXPECT_EQ(first["revision"].asUInt64(),1u);EXPECT_TRUE(first["requires_sync"].asBool());
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
namespace {
struct PreactivationEnrollmentFixture : OrchardCreationFixture {
    PreactivationEnrollmentFixture():OrchardCreationFixture(false){
        ShieldedStateStartupTestAccess::PublishEnrollmentParent(*f.service,*parent);
        auto use=WalletService::AcquireWalletUse(wallet);
        use->Wallet().createFromBip39("preactivation-recovered",
            "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about","");
    }
    din::Json Enroll(){return rpc_context_wallet_orchard_enrollpreactivation(RequestContext(),din::obj());}
};
template<class Rows> void PreserveEmptyParentCase(const std::string& label,const Rows& rows,
        const std::string& wallet_before,const std::string& wallet_after){
    const auto root=std::filesystem::canonical(std::filesystem::temp_directory_path())/
        ("empty-parent-account-evidence-"+label+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    OrchardAdmissionFixture::Require(std::filesystem::create_directory(root));
    std::cout<<"Empty parent evidence: "<<root.string()<<std::endl;
    Json::Value result;result["label"]=label;result["wallet_before"]=wallet_before;result["wallet_after"]=wallet_after;
    result["source_rows"]=Json::Value(Json::arrayValue);
    const auto hex=[](const std::string& bytes){return util::hex(std::vector<uint8_t>(bytes.begin(),bytes.end()));};
    for(const auto& [key,value]:rows){Json::Value row;row["family"]=key.first;row["key_hex"]=hex(key.second);row["value_hex"]=hex(value);result["source_rows"].append(row);}
    std::ofstream file(root/"case.json",std::ios::binary);OrchardAdmissionFixture::Require(bool(file));
    file<<Json::writeString(Json::StreamWriterBuilder(),result);file.close();OrchardAdmissionFixture::Require(bool(file));
}
}
TEST(OrchardPreactivationEnrollment, RecoveredSeedReopensAndFindsRealShieldAcrossActivation){
    PreactivationEnrollmentFixture f;const auto initial=f.Catalog();ASSERT_TRUE(initial);ASSERT_FALSE(initial->generated);
    const auto owner=f.Setting("wallet_initial_owner_v1");const auto keys=f.AccountKeys(0);
    const auto enrolled=f.Enroll();ASSERT_FALSE(enrolled.isMember("error"))<<enrolled.toStyledString();
    EXPECT_EQ(enrolled["checkpoint_height"].asUInt(),101u);EXPECT_FALSE(enrolled["scan_complete_to_current_tip"].asBool());
    auto catalog=f.Catalog();ASSERT_TRUE(catalog&&catalog->preactivation);EXPECT_FALSE(catalog->generated);
    EXPECT_EQ(catalog->revision,2u);EXPECT_EQ(catalog->preactivation->activation,102u);EXPECT_EQ(catalog->preactivation->height,101u);
    EXPECT_EQ(f.Setting("wallet_initial_owner_v1"),owner);const auto before=f.Snapshot();
    const auto again=f.Enroll();ASSERT_FALSE(again.isMember("error"));EXPECT_TRUE(again["already_enrolled"].asBool());EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().encryptWallet("preactivation-pass");
     use->Wallet().open("preactivation-recovered");use->Wallet().unlockWallet("preactivation-pass",0);}
    EXPECT_EQ(f.Catalog(),catalog);EXPECT_EQ(f.AccountKeys(0).ExportFullViewingKey(),keys.ExportFullViewingKey());
    f.MineEmpty();const auto created=f.Call(0);ASSERT_FALSE(created.isMember("error"))<<created.toStyledString();
    const auto [body,bundle]=f.Shield(keys);(void)f.Mine(body);f.ReplayAccount(0);
    const auto account=f.Account(0);EXPECT_EQ(account.account.Scan().BalanceUna(),500000u);
    EXPECT_EQ(account.account.Scan().Notes().size(),1u);EXPECT_EQ(account.account.Scan().Checkpoint().height,103u);
    catalog=f.Catalog();ASSERT_TRUE(catalog&&catalog->preactivation);EXPECT_FALSE(catalog->generated);
    EXPECT_EQ(catalog->accounts.size(),1u);EXPECT_EQ(f.Setting("wallet_initial_owner_v1"),owner);EXPECT_TRUE(f.Enroll().isMember("error"));
}
TEST(OrchardEmptyParentAccount, PublicNamedAccountIssuanceNeedsNoFabricatedFirstEvent){
    PreactivationEnrollmentFixture f;
    ASSERT_FALSE(f.Enroll().isMember("error"));const auto enrolled=f.Catalog();
    ASSERT_TRUE(enrolled&&enrolled->preactivation&&!enrolled->generated);
    const auto before=f.f.service->getRuntimeDeliveryPage({},1);ASSERT_TRUE(before.ok());
    const auto source_before=ChainDBTransactionReadTestPeer::HandoffRows(f.f.db);const auto wallet_before=f.Snapshot();
    ASSERT_EQ((*before)->head.sequence,0u);ASSERT_TRUE((*before)->events.empty());ASSERT_FALSE((*before)->after_tip);
    const auto captured=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(captured.ok());const auto& view=**captured;
    EXPECT_EQ(view.Head().sequence,0u);EXPECT_TRUE(view.Head().digest.IsNull());
    EXPECT_EQ(view.Profile().activation_height,102u);EXPECT_EQ(view.Profile().domain.genesis_wire,f.Domain().genesis_wire);
    EXPECT_EQ(view.Point({}).checkpoint.height,101u);EXPECT_EQ(view.Point({}).checkpoint.block_hash,f.parent->hash);
    EXPECT_EQ(view.Point({}).checkpoint.tree_size,0u);
    EXPECT_ANY_THROW(view.Event(1));
    EXPECT_ANY_THROW(view.Block(1));
    EXPECT_ANY_THROW(view.State(1));
    EXPECT_ANY_THROW(view.Authorizations(1));
    RegisterOrchardAccountRpc();const auto* create=g_rpcRegistry.lookup("wallet.orchard.createaccount");ASSERT_NE(create,nullptr);
    const auto context=f.RequestContext();
    const auto created=(*create)(context,OrchardBoundParamsForTest(context,OrchardCreationRequest(0)));
    ASSERT_FALSE(created.isMember("error"))<<created.toStyledString();
    const auto issued=f.Issue(0);ASSERT_FALSE(issued.isMember("error"))<<issued.toStyledString();
    EXPECT_NE(issued["address"].asString(),created["address"].asString());
    const auto account=f.Account(0);EXPECT_EQ(account.account.Delivery().sequence,0u);
    EXPECT_EQ(account.account.Scan().Checkpoint().block_hash,f.parent->hash);EXPECT_EQ(account.account.Scan().BalanceUna(),0u);
    const auto catalog=f.Catalog();ASSERT_TRUE(catalog&&!catalog->generated&&catalog->preactivation);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().encryptWallet("empty-parent-password");
     use->Wallet().open("preactivation-recovered");use->Wallet().unlockWallet("empty-parent-password",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_EQ(f.Catalog(),catalog);
    EXPECT_EQ(f.Account(0).account.Scan().Checkpoint().block_hash,f.parent->hash);
    const auto unchanged=f.f.service->getRuntimeDeliveryPage({},1);ASSERT_TRUE(unchanged.ok());
    EXPECT_EQ((*unchanged)->head,(*before)->head);EXPECT_TRUE((*unchanged)->events.empty());EXPECT_FALSE((*unchanged)->after_tip);
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(f.f.db),source_before);
    PreserveEmptyParentCase("issued-at-parent",source_before,wallet_before,f.Snapshot());
    f.MineEmpty();f.ReplayAccount(0);EXPECT_EQ(f.Account(0).account.Delivery().sequence,1u);EXPECT_EQ(f.Catalog(),catalog);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    const auto undone=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(undone.ok());
    EXPECT_EQ((*undone)->Head().sequence,2u);EXPECT_EQ((*undone)->Event(1)->context.height,102u);
}
TEST(OrchardEmptyParentAccount, BadSourceAndMissingCatalogNeverCreateOwnersOrResetHistory){
    PreactivationEnrollmentFixture f;ASSERT_FALSE(f.Enroll().isMember("error"));
    const auto before=f.Snapshot();
    f.f.service->setHeaderChainSelector(std::make_shared<consensus::HeaderChainSelector>());
    EXPECT_FALSE(f.f.service->getRuntimeAccountReplay().ok());EXPECT_TRUE(f.Call(0).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    auto headers=std::make_shared<consensus::HeaderChainSelector>();for(const auto& b:f.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));
    f.f.service->setHeaderChainSelector(headers);
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token,f.f.blocks[100].GetHash(),100),Status::Ok);
    EXPECT_FALSE(f.f.service->getRuntimeAccountReplay().ok());EXPECT_TRUE(f.Call(0).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token,f.parent->hash,101),Status::Ok);
    ASSERT_TRUE(f.f.service->getRuntimeAccountReplay().ok());
    ChainDBTransactionReadTestPeer::HandoffPutOrchardRow(f.f.db,f.f.token,"O1-untracked-parent","refusal evidence");
    const auto corrupt=ChainDBTransactionReadTestPeer::HandoffRows(f.f.db);
    EXPECT_FALSE(f.f.service->getRuntimeAccountReplay().ok());EXPECT_TRUE(f.Call(0).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(f.f.db),corrupt);
    PreserveEmptyParentCase("untracked-namespace-refusal",corrupt,before,f.Snapshot());
    ChainDBTransactionReadTestPeer::HandoffDeleteOrchardRow(f.f.db,f.f.token,"O1-untracked-parent");
    f.Sql("DELETE FROM settings WHERE key='orchard_account_catalog_v1'");const auto missing=f.Snapshot();
    EXPECT_TRUE(f.Call(0).isMember("error"));EXPECT_EQ(f.Snapshot(),missing);
    const auto page=f.f.service->getRuntimeDeliveryPage({},1);ASSERT_TRUE(page.ok());EXPECT_EQ((*page)->head.sequence,0u);
    EXPECT_TRUE((*page)->events.empty());EXPECT_FALSE((*page)->after_tip);
}
TEST(OrchardEmptyParentQueue, CurrentCatalogReadmissionObservesWithoutInventingProgress){
    PreactivationEnrollmentFixture f;ASSERT_FALSE(f.Enroll().isMember("error"));ASSERT_FALSE(f.Call(0).isMember("error"));
    const auto audit=[&](uint64_t session){auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        return f.f.service->checkRuntimeEmptyParentWallet(use->Wallet(),session);};
    const auto before=f.Snapshot();const auto rows=ChainDBTransactionReadTestPeer::HandoffRows(f.f.db);
    EXPECT_EQ(audit(f.Session()),Status::Ok);EXPECT_NE(audit(f.Session()+1),Status::Ok);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(f.f.db),rows);
    // Production canonical recovery returns NotRequired, NOT AppliedPrefix.
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::NotRequired);
    EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().encryptWallet("parent-queue-pass");
     use->Wallet().open("preactivation-recovered");}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_NE(audit(f.Session()),Status::Ok);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unlockWallet("parent-queue-pass",0);}
    const auto reopened=f.Snapshot();EXPECT_EQ(audit(f.Session()),Status::Ok);EXPECT_EQ(f.Snapshot(),reopened);
    f.Sql("CREATE TEMP TABLE saved_empty_queue_owner AS SELECT * FROM orchard_wallet_snapshots; DELETE FROM orchard_wallet_snapshots");
    const auto missing=f.Snapshot();EXPECT_NE(audit(f.Session()),Status::Ok);EXPECT_EQ(f.Snapshot(),missing);
    f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_empty_queue_owner; DROP TABLE saved_empty_queue_owner");
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token,f.f.blocks[100].GetHash(),100),Status::Ok);
    EXPECT_NE(audit(f.Session()),Status::Ok);EXPECT_EQ(f.Snapshot(),reopened);
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token,f.parent->hash,101),Status::Ok);
    ChainDBTransactionReadTestPeer::HandoffPutOrchardRow(f.f.db,f.f.token,"O1-untracked-queue","refusal evidence");
    const auto corrupt=ChainDBTransactionReadTestPeer::HandoffRows(f.f.db);
    EXPECT_NE(audit(f.Session()),Status::Ok);EXPECT_EQ(f.Snapshot(),reopened);
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(f.f.db),corrupt);
    PreserveEmptyParentCase("queue-readmission-refusal",corrupt,reopened,f.Snapshot());
}
TEST(OrchardEmptyParentQueue, EmptySourceReachesRealCoinFenceAndNeverBypassesIt){
    PreactivationEnrollmentFixture f;ASSERT_FALSE(f.Enroll().isMember("error"));ASSERT_FALSE(f.Call(0).isMember("error"));
    const auto keys=f.AccountKeys(0);const auto current=f.Account(0);const auto session=f.Session();
    // Deliberately NOT a wallet funding fixture. The real source coin has a
    // wrong supplied value and must fail at the selected coin fence, before
    // reservation, proof scheduling or any Ready bytes can be produced.
    orchard::ResolvedInput input{};const auto id=f.f.blocks[1].vtx.front().GetTxid().AsUint256();
    std::copy(id.begin(),id.end(),input.txid_wire.begin());input.sequence=UINT32_MAX;
    input.amount_una=3000;input.script_pub_key=f.f.script;
    const std::vector<orchard::ResolvedInput> inputs{input};
    const std::vector<orchard::WalletPayment> payments{{2000,keys.Receiver(orchard::WalletScope::External,{})}};
    const auto before=f.Snapshot();const auto rows=ChainDBTransactionReadTestPeer::HandoffRows(f.f.db);
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    wallet::OrchardProofJobs jobs;
    try{(void)f.f.service->queueRuntimeWalletShield(use->Wallet(),session,0,current.revision,orchard::Hash{91},inputs,payments,{},1000,jobs);
        FAIL()<<"Wrong selected coin was queued";}
    catch(const std::runtime_error& error){EXPECT_STREQ(error.what(),"Shield live/durable input mismatch or unavailable");}
    try{(void)f.f.service->reserveRuntimeWalletShield(use->Wallet(),session,0,current.revision,orchard::Hash{92},inputs,payments,{},1000);
        FAIL()<<"Wrong selected coin was reserved";}
    catch(const std::runtime_error& error){EXPECT_STREQ(error.what(),"Shield live/durable input mismatch or unavailable");}
    EXPECT_FALSE(jobs.Query(orchard::Hash{91}));EXPECT_EQ(f.Snapshot(),before);
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(f.f.db),rows);
    const auto page=f.f.service->getRuntimeDeliveryPage({},1);ASSERT_TRUE(page.ok());
    EXPECT_EQ((*page)->head.sequence,0u);EXPECT_TRUE((*page)->events.empty());EXPECT_FALSE((*page)->after_tip);
    PreserveEmptyParentCase("queue-coin-refusal",rows,before,f.Snapshot());
}
TEST(OrchardEmptyParentQueue, NamespaceChangedAfterCaptureRefusesAtFinalReadmissionFence){
    PreactivationEnrollmentFixture f;ASSERT_FALSE(f.Enroll().isMember("error"));ASSERT_FALSE(f.Call(0).isMember("error"));
    const auto before=f.Snapshot();const auto session=f.Session();
    struct Injection {PreactivationEnrollmentFixture* fixture;bool changed=false;bool failed=false;} injection{&f};
    auto use=WalletService::AcquireWalletUse(f.wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
    sqlite3* db=nullptr;{auto lease=use->Wallet().AcquireDatabaseLease();db=lease->Database();}
    // The catalog preparation transaction commits after immutable source capture
    // and before selected ownership. Inject only untrusted source bytes there;
    // no private owner, marker, cursor or wallet effect is forged.
    sqlite3_commit_hook(db,[](void* p){auto& x=*static_cast<Injection*>(p);if(x.changed)return 0;
        x.changed=true;try{ChainDBTransactionReadTestPeer::HandoffPutOrchardRow(x.fixture->f.db,
            x.fixture->f.token,"O1-after-empty-source-capture","final fence refusal evidence");}
        catch(...){x.failed=true;return 1;}return 0;},&injection);
    const auto result=f.f.service->checkRuntimeEmptyParentWallet(use->Wallet(),session);
    sqlite3_commit_hook(db,nullptr,nullptr);
    ASSERT_TRUE(injection.changed);ASSERT_FALSE(injection.failed);EXPECT_NE(result,Status::Ok);
    EXPECT_EQ(f.Snapshot(),before);
    const auto corrupted=ChainDBTransactionReadTestPeer::HandoffRows(f.f.db);
    PreserveEmptyParentCase("queue-final-namespace-fence",corrupted,before,f.Snapshot());
    ChainDBTransactionReadTestPeer::HandoffDeleteOrchardRow(f.f.db,f.f.token,"O1-after-empty-source-capture");
    EXPECT_EQ(f.f.service->checkRuntimeEmptyParentWallet(use->Wallet(),session),Status::Ok);
}
TEST(OrchardPreactivationEnrollment, MissingCorruptOrUntrackedStateNeverRepairs){
    PreactivationEnrollmentFixture f;const auto good=f.Setting("orchard_account_catalog_v1");
    const auto owner=f.Setting("wallet_initial_owner_v1");
    for(const char* sql:{"DELETE FROM settings WHERE key='orchard_account_catalog_v1'",
                        "UPDATE settings SET value='00' WHERE key='orchard_account_catalog_v1'",
                        "DELETE FROM settings WHERE key='wallet_initial_owner_v1'"}){
        f.Sql(sql);const auto before=f.Snapshot();EXPECT_TRUE(f.Enroll().isMember("error"));EXPECT_EQ(f.Snapshot(),before);
        auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().setSetting("orchard_account_catalog_v1",good);
        use->Wallet().setSetting("wallet_initial_owner_v1",owner);
    }
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
     f.Sql("PRAGMA synchronous=FULL; BEGIN IMMEDIATE");orchard::WalletSnapshotStore::InitializeSchemaUnderTransaction(lease->Database());
     f.Sql("CREATE TABLE orchard_wallet_retained(wallet_id BLOB,account INTEGER,revision INTEGER,sealed BLOB); COMMIT");}
    f.Sql("INSERT INTO orchard_wallet_snapshots(wallet_id,account,revision,sealed) VALUES(randomblob(32),0,1,zeroblob(128))");
    const auto before=f.Snapshot();EXPECT_TRUE(f.Enroll().isMember("error"));EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardPreactivationEnrollment, UpdateCommitAndBorrowedOwnerRefusalsAreAtomic){
    PreactivationEnrollmentFixture f;const auto before=f.Snapshot();
    f.Sql("CREATE TRIGGER refuse_enrollment BEFORE UPDATE ON settings WHEN NEW.key='orchard_account_catalog_v1' BEGIN SELECT RAISE(ABORT,'enrollment refused'); END");
    EXPECT_TRUE(f.Enroll().isMember("error"));EXPECT_EQ(f.Snapshot(),before);f.Sql("DROP TRIGGER refuse_enrollment");
    {auto use=WalletService::AcquireWalletUse(f.wallet);sqlite3* db;
     {auto lease=use->Wallet().AcquireDatabaseLease();db=lease->Database();}
     bool seen=false;sqlite3_commit_hook(db,[](void* p){*static_cast<bool*>(p)=true;return 1;},&seen);
     EXPECT_TRUE(f.Enroll().isMember("error"));sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_TRUE(seen);}
    EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();EXPECT_TRUE(f.Enroll().isMember("error"));}
    {auto selected=f.f.service->AcquireBlockIngressActivationLock();EXPECT_TRUE(f.Enroll().isMember("error"));}
    EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(f.Enroll().isMember("error"));
}
TEST(OrchardPreactivationEnrollment, UnknownHeadersValidatedMarkerAndRetainedActivationHistoryRefuse){
    PreactivationEnrollmentFixture f;const auto before=f.Snapshot();
    f.f.service->setHeaderChainSelector(std::make_shared<consensus::HeaderChainSelector>());
    EXPECT_TRUE(f.Enroll().isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    auto headers=std::make_shared<consensus::HeaderChainSelector>();
    for(const auto& b:f.f.blocks)ASSERT_TRUE(headers->AddHeader(b.header));f.f.service->setHeaderChainSelector(headers);
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token,f.f.blocks[100].GetHash(),100),Status::Ok);
    EXPECT_TRUE(f.Enroll().isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token,f.parent->hash,101),Status::Ok);
    f.MineEmpty();EXPECT_TRUE(f.Enroll().isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    EXPECT_TRUE(f.Enroll().isMember("error"));EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardPreactivationEnrollment, ScopedCertificateTamperingAndStalePublicBindingRefuse){
    PreactivationEnrollmentFixture f;RegisterOrchardAccountRpc();
    const auto* method=g_rpcRegistry.lookup("wallet.orchard.enrollpreactivation");ASSERT_NE(method,nullptr);
    const auto request=OrchardBoundParamsForTest(f.RequestContext(),din::obj());
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("preactivation-recovered");}
    const auto before=f.Snapshot();EXPECT_TRUE((*method)(f.RequestContext(),request).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    const auto fresh=OrchardBoundParamsForTest(f.RequestContext(),din::obj());
    const auto result=(*method)(f.RequestContext(),fresh);ASSERT_FALSE(result.isMember("error"))<<result.toStyledString();
    const auto catalog=f.Catalog();ASSERT_TRUE(catalog&&catalog->preactivation);const auto& p=*catalog->preactivation;
    wallet::OrchardAccountCatalog::Entry e{0,p.network,p.genesis,p.branch,p.activation};EXPECT_TRUE(catalog->Covers(e));
    e.branch^=1;EXPECT_FALSE(catalog->Covers(e));e.branch=p.branch;e.genesis[0]^=1;EXPECT_FALSE(catalog->Covers(e));
    e.genesis=p.genesis;++e.activation;EXPECT_FALSE(catalog->Covers(e));e.activation=p.activation;e.network^=1;EXPECT_FALSE(catalog->Covers(e));
    const auto good=f.Setting("orchard_account_catalog_v1");std::vector<uint8_t> bytes;ASSERT_TRUE(util::unhex(good,bytes));
    bytes.back()^=1;
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().setSetting("orchard_account_catalog_v1",util::hex(bytes));}
    const auto damaged=f.Snapshot();EXPECT_TRUE(f.Enroll().isMember("error"));EXPECT_THROW((void)f.Catalog(),std::runtime_error);EXPECT_EQ(f.Snapshot(),damaged);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().setSetting("orchard_account_catalog_v1",good);}
    EXPECT_EQ(f.Catalog(),catalog);
}
TEST(OrchardPreactivationEnrollment, GeneratedCatalogNeverReclassified){
    OrchardCreationFixture f(false);ShieldedStateStartupTestAccess::PublishEnrollmentParent(*f.f.service,*f.parent);
    const auto before=f.Snapshot();const auto catalog=f.Catalog();ASSERT_TRUE(catalog&&catalog->generated);
    EXPECT_TRUE(rpc_context_wallet_orchard_enrollpreactivation(f.RequestContext(),din::obj()).isMember("error"));
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.Catalog(),catalog);
}
namespace {
constexpr const char* RecoveryTestPhrase="abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
struct Account0RecoveryFixture:OrchardCreationFixture {
    Account0RecoveryFixture():OrchardCreationFixture(false){
        ShieldedStateStartupTestAccess::PublishEnrollmentParent(*f.service,*parent);MineEmpty();
        auto use=WalletService::AcquireWalletUse(wallet);use->Wallet().createFromBip39("before-recovery",RecoveryTestPhrase,"");
    }
    void RestoreLate(){auto use=WalletService::AcquireWalletUse(wallet);use->Wallet().createFromBip39("after-activation-recovery",RecoveryTestPhrase,"");}
    din::Json Recover(bool restart=false){auto p=din::obj();if(restart)p["restart"]=true;return rpc_context_wallet_orchard_recoveraccount0(RequestContext(),p);}
};
}
TEST(OrchardAccount0Recovery, LaterRestoreFindsEarlierFundsAndMemoAndSurvivesReopen){
    Account0RecoveryFixture f;const auto keys=f.AccountKeys(0);std::array<uint8_t,512> memo{};memo[0]='R';memo[1]='6';
    const auto [body,bundle]=f.Shield(keys,memo);(void)f.Mine(body);f.MineEmpty();f.RestoreLate();
    ASSERT_FALSE(f.Catalog()->KnownInventory());EXPECT_TRUE(f.Call(0).isMember("error"));
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.recoveraccount0");ASSERT_NE(method,nullptr);
    const auto result=(*method)(f.RequestContext(),OrchardBoundParamsForTest(f.RequestContext(),din::obj()));
    ASSERT_FALSE(result.isMember("error"))<<result.toStyledString();ASSERT_TRUE(result["complete"].asBool())<<result.toStyledString();
    const auto catalog=f.Catalog();ASSERT_TRUE(catalog&&catalog->recovered);EXPECT_FALSE(catalog->generated);
    EXPECT_EQ(catalog->accounts.size(),1u);EXPECT_EQ(catalog->accounts.front().account,0u);EXPECT_TRUE(f.Call(1).isMember("error"));
    for(int reopen=0;reopen<2;++reopen){
        const auto account=f.Account(0);EXPECT_EQ(account.account.Scan().BalanceUna(),500000u);
        ASSERT_EQ(account.account.Scan().CompleteReceipts().size(),1u);
        const auto note=account.account.Scan().DecryptReceipt(0);EXPECT_EQ(note.Facts().memo[0],'R');EXPECT_EQ(note.Facts().memo[1],'6');
        {auto use=WalletService::AcquireWalletUse(f.wallet);if(!reopen)use->Wallet().encryptWallet("late-recovery-pass");
         use->Wallet().open("after-activation-recovery");use->Wallet().unlockWallet("late-recovery-pass",0);}
        EXPECT_EQ(f.Catalog(),catalog);
    }
    const auto again=f.Recover();ASSERT_FALSE(again.isMember("error"));EXPECT_TRUE(again["already_recovered"].asBool());EXPECT_TRUE(again["complete"].asBool());
    const auto before=f.Snapshot();EXPECT_TRUE(f.Recover(true).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    const auto owned=f.Account(0);ASSERT_EQ(owned.account.Scan().Notes().size(),1u);
    const auto& unspent=owned.account.Scan().Notes().front();const std::vector<orchard::WalletSpendInput> inputs{{*unspent.note,*unspent.witness}};
    const auto recipient=orchard::WalletKeys::FromSeed(std::array<uint8_t,64>{91},0);
    const std::vector<orchard::WalletPayment> payment{{400000,recipient.Receiver(orchard::WalletScope::External,{})}};
    orchard::Hash anchor{};std::copy(owned.account.Scan().Checkpoint().anchor.begin(),owned.account.Scan().Checkpoint().anchor.end(),anchor.begin());
    const auto proved=orchard::WalletBundlePlan::PrepareSpend(keys,inputs,anchor,payment)
        .Prove(orchard::SigningContext::Create(f.Domain(),0,{}, {},100000));
    const auto transfer=MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::Create(0,{}, {},100000,proved.Bytes()));
    (void)f.Mine(transfer);const auto advanced=f.Recover();ASSERT_FALSE(advanced.isMember("error"))<<advanced.toStyledString();
    EXPECT_TRUE(advanced["complete"].asBool());const auto spent=f.Account(0);
    EXPECT_EQ(spent.account.Scan().BalanceUna(),0u);EXPECT_TRUE(spent.account.Scan().Notes().empty());
    EXPECT_EQ(spent.account.Scan().CompleteReceipts().size(),1u);EXPECT_EQ(spent.account.Scan().DecryptReceipt(0).Facts().memo[0],'R');
}
TEST(OrchardAccount0Recovery, EncryptedPartialPrefixResumesWithoutEarlyCatalogPublication){
    Account0RecoveryFixture f;const auto keys=f.AccountKeys(0);const auto [body,bundle]=f.Shield(keys);(void)f.Mine(body);
    for(size_t i=0;i<65;++i)f.MineEmpty();f.RestoreLate();const auto before=f.Catalog();
    const auto first=f.Recover();ASSERT_FALSE(first.isMember("error"))<<first.toStyledString();EXPECT_FALSE(first["complete"].asBool());
    EXPECT_EQ(first["applied_sequence"].asUInt64(),64u);EXPECT_EQ(f.Catalog(),before);EXPECT_TRUE(f.Call(0).isMember("error"));
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().encryptWallet("prefix-pass");use->Wallet().open("after-activation-recovery");use->Wallet().unlockWallet("prefix-pass",0);}
    const auto done=f.Recover();ASSERT_FALSE(done.isMember("error"))<<done.toStyledString();EXPECT_TRUE(done["complete"].asBool())<<done.toStyledString();
    EXPECT_EQ(f.Account(0).account.Scan().BalanceUna(),500000u);
}
TEST(OrchardAccount0Recovery, MissingCorruptUnknownRowsAndGeneratedOwnersNeverReclassify){
    Account0RecoveryFixture f;f.RestoreLate();const auto good=f.Setting("orchard_account_catalog_v1");
    for(const char* sql:{"DELETE FROM settings WHERE key='orchard_account_catalog_v1'","UPDATE settings SET value='00' WHERE key='orchard_account_catalog_v1'"}){
        f.Sql(sql);const auto before=f.Snapshot();EXPECT_TRUE(f.Recover().isMember("error"));EXPECT_TRUE(f.Recover(true).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
        auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().setSetting("orchard_account_catalog_v1",good);
    }
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();f.Sql("PRAGMA synchronous=FULL; BEGIN IMMEDIATE");
     orchard::WalletSnapshotStore::InitializeSchemaUnderTransaction(lease->Database());f.Sql("CREATE TABLE orchard_wallet_retained(wallet_id BLOB,account INTEGER,revision INTEGER,sealed BLOB); COMMIT");}
    f.Sql("INSERT INTO orchard_wallet_snapshots(wallet_id,account,revision,sealed) VALUES(randomblob(32),0,1,zeroblob(128))");
    const auto before=f.Snapshot();EXPECT_TRUE(f.Recover().isMember("error"));EXPECT_TRUE(f.Recover(true).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardAccount0Recovery, PublicationFailureAndBorrowedOwnersPreserveActiveInventory){
    Account0RecoveryFixture f;f.RestoreLate();const auto before=f.Snapshot();
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();EXPECT_TRUE(f.Recover().isMember("error"));}
    {auto lock=f.f.service->AcquireBlockIngressActivationLock();EXPECT_TRUE(f.Recover().isMember("error"));}
    f.Sql("CREATE TRIGGER refuse_recovery BEFORE UPDATE ON settings WHEN NEW.key='orchard_account_catalog_v1' BEGIN SELECT RAISE(ABORT,'refuse recovery publication'); END");
    EXPECT_TRUE(f.Recover().isMember("error"));EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(f.Catalog()->KnownInventory());
    f.Sql("DROP TRIGGER refuse_recovery");const auto retry=f.Recover();ASSERT_FALSE(retry.isMember("error"))<<retry.toStyledString();EXPECT_TRUE(retry["complete"].asBool());
}
TEST(OrchardAccount0Recovery, GeneratedProvenanceAndCallerScopeOrBirthdayNeverOverrideGuards){
    OrchardCreationFixture f;const auto before=f.Snapshot();
    EXPECT_TRUE(rpc_context_wallet_orchard_recoveraccount0(f.RequestContext(),din::obj()).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    for(const char* key:{"account","height","birthday_height","checkpoint_hash","network"}){
        auto p=din::obj();p[key]=0;EXPECT_TRUE(rpc_context_wallet_orchard_recoveraccount0(f.RequestContext(),p).isMember("error"));
    }
    EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardAccount0Recovery, CorruptIncompleteCacheRequiresExplicitRestartAndNeverRepairsActiveOwners){
    Account0RecoveryFixture f;const auto keys=f.AccountKeys(0);const auto [body,bundle]=f.Shield(keys);(void)f.Mine(body);
    for(size_t i=0;i<65;++i)f.MineEmpty();f.RestoreLate();const auto catalog=f.Catalog();
    const auto first=f.Recover();ASSERT_FALSE(first.isMember("error"));ASSERT_FALSE(first["complete"].asBool());
    f.Sql("UPDATE orchard_recovery_snapshots SET sealed=zeroblob(length(sealed))");
    const auto active=f.Snapshot();EXPECT_TRUE(f.Recover().isMember("error"));EXPECT_EQ(f.Snapshot(),active);EXPECT_EQ(f.Catalog(),catalog);
    const auto restarted=f.Recover(true);ASSERT_FALSE(restarted.isMember("error"))<<restarted.toStyledString();
    EXPECT_FALSE(restarted["complete"].asBool());EXPECT_EQ(f.Catalog(),catalog);
    const auto done=f.Recover();ASSERT_FALSE(done.isMember("error"));ASSERT_TRUE(done["complete"].asBool());
    EXPECT_EQ(f.Account(0).account.Scan().BalanceUna(),500000u);
    const auto admitted=f.Snapshot();EXPECT_TRUE(f.Recover(true).isMember("error"));EXPECT_EQ(f.Snapshot(),admitted);
}
TEST(OrchardAccount0Recovery, DdlAdoptionAndCommitRefusalRollBackCatalogAndActiveRows){
    Account0RecoveryFixture f;f.RestoreLate();const auto before=f.Snapshot();sqlite3* db;
    {auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();db=lease->Database();}
    sqlite3_set_authorizer(db,[](void*,int action,const char*,const char* table,const char*,const char*){
        return action==SQLITE_ALTER_TABLE&&table&&std::string_view(table)=="orchard_recovery_retained"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    const auto denied=f.Recover();sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_TRUE(denied.isMember("error"));EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(f.Catalog()->KnownInventory());
    // The verified staging prefix survives the failed atomic adoption. Refuse
    // its publication COMMIT too, without consuming or relabelling that prefix.
    bool saw_catalog=false;
    sqlite3_update_hook(db,[](void* p,int op,const char*,const char* table,sqlite3_int64){
        if(op==SQLITE_UPDATE&&table&&std::string_view(table)=="settings")*static_cast<bool*>(p)=true;
    },&saw_catalog);
    sqlite3_commit_hook(db,[](void* p){return *static_cast<bool*>(p)?1:0;},&saw_catalog);
    const auto uncommitted=f.Recover();sqlite3_commit_hook(db,nullptr,nullptr);sqlite3_update_hook(db,nullptr,nullptr);
    EXPECT_TRUE(saw_catalog);EXPECT_TRUE(uncommitted.isMember("error"));EXPECT_EQ(f.Snapshot(),before);EXPECT_FALSE(f.Catalog()->KnownInventory());
    const auto done=f.Recover();ASSERT_FALSE(done.isMember("error"))<<done.toStyledString();EXPECT_TRUE(done["complete"].asBool());
}
TEST(OrchardAccount0Recovery, StagingRetentionIsBoundedBeforeFinalPublication){
    Account0RecoveryFixture f;for(size_t i=0;i<321;++i)f.MineEmpty();f.RestoreLate();
    const auto catalog=f.Catalog();bool complete=false;size_t calls=0;
    while(!complete&&calls<7){
        const auto progress=f.Recover();ASSERT_FALSE(progress.isMember("error"))<<progress.toStyledString();
        complete=progress["complete"].asBool();++calls;
        if(!complete){
            EXPECT_EQ(f.Catalog(),catalog);
            auto use=WalletService::AcquireWalletUse(f.wallet);auto lease=use->Wallet().AcquireDatabaseLease();
            sqlite3_stmt* rows=nullptr;
            ASSERT_EQ(sqlite3_prepare_v2(lease->Database(),"SELECT count(*) FROM orchard_recovery_retained",-1,&rows,nullptr),SQLITE_OK);
            ASSERT_EQ(sqlite3_step(rows),SQLITE_ROW);const auto count=sqlite3_column_int64(rows,0);sqlite3_finalize(rows);
            EXPECT_LE(count,256); // Fails on the original complete-only pruning.
        }
    }
    ASSERT_TRUE(complete);EXPECT_EQ(calls,6u);EXPECT_TRUE(f.Catalog()->recovered.has_value());
    EXPECT_EQ(f.Account(0).account.Scan().BalanceUna(),0u);
}
#endif
} // namespace dinero
