#pragma once
#include "rpc/orchard_account_rpc.h"
namespace dinero {
namespace {
din::Json OrchardIssuanceRequest(uint32_t account=0){din::Json p;p["account"]=Json::UInt64(account);return p;}
}
TEST(OrchardAccountIssuance, RejectMalformedRequestsBeforeServices){
    ExecutionContext context;
    std::vector<din::Json> invalid{din::arr(),din::Json{},din::Json("0")};
    for(const auto& value:std::vector<din::Json>{din::Json(-1),din::Json(0.5),din::Json(true),din::Json("0"),din::Json(Json::UInt64(0x80000000ULL))}){
        din::Json p;p["account"]=value;invalid.push_back(p);
    }
    auto extra=OrchardIssuanceRequest();extra["label"]="extra";invalid.push_back(extra);
    for(const auto& p:invalid){const auto out=rpc_context_wallet_orchard_getnewaddress(context,p);
        EXPECT_TRUE(out.isMember("error"));EXPECT_FALSE(out.isMember("address"));EXPECT_FALSE(out.isMember("revision"));}
}
TEST(OrchardAccountIssuance, RegistryAndUnavailableBackendOrServicesRefuse){
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.getnewaddress");ASSERT_NE(method,nullptr);
    ExecutionContext context;const auto out=(*method)(context,OrchardIssuanceRequest());
    EXPECT_TRUE(out.isMember("error"));EXPECT_FALSE(out.isMember("address"));
#ifndef DINERO_TEST_ORCHARD_ORIGIN
    EXPECT_EQ(out["error"].asString(),"Orchard wallet backend unavailable");
#endif
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct OrchardIssuanceFixture : CanonicalRecoveryFixture {
    OrchardIssuanceFixture():CanonicalRecoveryFixture(true){
        const auto keys=Keys();const auto [body,bundle]=Shield(keys);(void)Mine(body);
        ExecutionContext c;c.daemon=&context;c.walletName="canonical-recovery";
        const auto created=rpc_context_wallet_orchard_createaccount(c,OrchardIssuanceRequest());
        OrchardAdmissionFixture::Require(!created.isMember("error"));
        {auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.service);
         const auto session=use->Wallet().AcquireDatabaseLease()->Session();
         const auto origin=f.service->getRuntimeWalletOrigin(use->Wallet(),session,&index->Index());
         OrchardAdmissionFixture::Require(origin.ok());
         OrchardAdmissionFixture::Require(f.service->adoptRuntimeWalletOrigin(use->Wallet(),index->Index(),**origin)==Status::Ok);}
        OrchardAdmissionFixture::Require(wallet->RecoverActiveWalletFromCanonicalSource()==Recovery::AppliedPrefix);}
    din::Json Call(uint32_t account=0){ExecutionContext c;c.daemon=&context;c.walletName="canonical-recovery";
        return rpc_context_wallet_orchard_getnewaddress(c,OrchardIssuanceRequest(account));}
    void Sql(const char* sql){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        OrchardAdmissionFixture::Require(sqlite3_exec(lease->Database(),sql,nullptr,nullptr,nullptr)==SQLITE_OK);}
    std::string Rows(){auto use=WalletService::AcquireWalletUse(wallet);auto lease=use->Wallet().AcquireDatabaseLease();
        const char* sql="SELECT 'current',hex(wallet_id),account,revision,hex(sealed) FROM orchard_wallet_snapshots UNION ALL SELECT 'retained',hex(wallet_id),account,revision,hex(sealed) FROM orchard_wallet_retained ORDER BY 1,2,3,4";
        struct Statement{sqlite3_stmt* p=nullptr;~Statement(){sqlite3_finalize(p);}} q;
        OrchardAdmissionFixture::Require(sqlite3_prepare_v2(lease->Database(),sql,-1,&q.p,nullptr)==SQLITE_OK);
        std::string out;int rc;while((rc=sqlite3_step(q.p))==SQLITE_ROW){for(int i=0;i<5;++i){
            const auto* p=sqlite3_column_text(q.p,i);OrchardAdmissionFixture::Require(p);
            out.append(reinterpret_cast<const char*>(p),sqlite3_column_bytes(q.p,i));out+='|';}out+='\n';}
        OrchardAdmissionFixture::Require(rc==SQLITE_DONE);return out;
    }
};
std::vector<uint8_t> OrchardAccountBytes(const wallet::OrchardAccountState& state){auto bytes=state.Encode();return {bytes.Bytes().begin(),bytes.Bytes().end()};}
}
TEST(OrchardAccountIssuance, ActualRpcPersistsCounterAndPreservesAuthenticatedStateAcrossReopen){
    OrchardIssuanceFixture f;const auto before=f.ReadAccount();ASSERT_EQ(before.account.Scan().BalanceUna(),500000u);
    const auto expected=before.account.IssueReceiver(orchard::WalletScope::External);
    RegisterOrchardAccountRpc();const auto* handler=g_rpcRegistry.lookup("wallet.orchard.getnewaddress");ASSERT_NE(handler,nullptr);
    ExecutionContext c;c.daemon=&f.context;c.walletName="canonical-recovery";
    const auto result=(*handler)(c,OrchardIssuanceRequest());ASSERT_FALSE(result.isMember("error"))<<result["error"].asString();
    EXPECT_EQ(result["address"].asString(),expected.second.EncodeAddress(orchard::WalletNetwork::Regtest));
    EXPECT_EQ(result["revision"].asUInt64(),before.revision+1);EXPECT_EQ(result["account"].asUInt64(),0u);
    const auto after=f.ReadAccount();EXPECT_EQ(OrchardAccountBytes(after.account),OrchardAccountBytes(expected.first));
    EXPECT_EQ(after.account.Delivery(),before.account.Delivery());EXPECT_EQ(after.account.Archive(),before.account.Archive());
    EXPECT_EQ(after.account.ParentSnapshotRevision(),before.account.ParentSnapshotRevision());
    const auto next=after.account.IssueReceiver(orchard::WalletScope::External).second.EncodeAddress(orchard::WalletNetwork::Regtest);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");}
    EXPECT_TRUE(f.Call().isMember("error"));
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());const auto reopened=f.Call();ASSERT_FALSE(reopened.isMember("error"));
    EXPECT_EQ(reopened["address"].asString(),next);EXPECT_NE(next,result["address"].asString());
    EXPECT_EQ(f.ReadAccount().account.Scan().BalanceUna(),500000u);
}
TEST(OrchardAccountIssuance, RequiredWriteCommitAndBorrowedTransactionPreserveCounter){
    OrchardIssuanceFixture f;const auto original=f.Rows();
    f.Sql("CREATE TRIGGER refuse_issuance BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'fixture issuance write refusal'); END");
    const auto refused=f.Call();EXPECT_TRUE(refused.isMember("error"));EXPECT_FALSE(refused.isMember("address"));EXPECT_EQ(f.Rows(),original);
    f.Sql("DROP TRIGGER refuse_issuance");
    auto use=WalletService::AcquireWalletUse(f.wallet);sqlite3* db;
    {auto lease=use->Wallet().AcquireDatabaseLease();db=lease->Database();}
    bool commit_seen=false;sqlite3_commit_hook(db,[](void* p){*static_cast<bool*>(p)=true;return 1;},&commit_seen);
    const auto denied=f.Call();sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_TRUE(commit_seen);EXPECT_TRUE(denied.isMember("error"));EXPECT_FALSE(denied.isMember("address"));EXPECT_EQ(f.Rows(),original);
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto session=f.Session();
    {auto lease=use->Wallet().AcquireDatabaseLease();ASSERT_EQ(sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr),SQLITE_OK);
     EXPECT_THROW(wallet::OrchardAccountDelivery::IssueReceiverForReplay(use->Wallet(),session,{f.Domain(),102,0},**view,orchard::WalletScope::External),std::runtime_error);
     EXPECT_FALSE(sqlite3_get_autocommit(db));ASSERT_EQ(sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr),SQLITE_OK);}
    {auto source_owner=ChainstateService::AcquireWalletIndexUse(f.f.service);
     {auto lease=use->Wallet().AcquireDatabaseLease();
      EXPECT_FALSE(f.f.service->getRuntimeAccountReplayForWallet(use->Wallet(),session).ok());}
     {auto selected=f.f.service->AcquireBlockIngressActivationLock();
      EXPECT_FALSE(f.f.service->getRuntimeAccountReplayForWallet(use->Wallet(),session).ok());}
     EXPECT_TRUE(f.f.service->getRuntimeAccountReplayForWallet(use->Wallet(),session).ok());}
    EXPECT_EQ(f.Rows(),original);EXPECT_FALSE(f.Call().isMember("error"));
}
TEST(OrchardAccountIssuance, MissingIdentityAccountReadFailureAndWrongSessionNeverInitialize){
    OrchardIssuanceFixture f;const auto original=f.Rows();const auto before=f.ReadAccount();
    EXPECT_TRUE(f.Call(7).isMember("error"));EXPECT_EQ(f.Rows(),original);
    auto use=WalletService::AcquireWalletUse(f.wallet);sqlite3* db;
    {auto lease=use->Wallet().AcquireDatabaseLease();db=lease->Database();}
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){
        return action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_snapshots"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_TRUE(f.Call().isMember("error"));sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_EQ(f.Rows(),original);
    f.Sql("CREATE TEMP TABLE saved_issuance_identity AS SELECT runtime_delivery_id FROM wallet_meta WHERE id=1");
    f.Sql("UPDATE wallet_meta SET runtime_delivery_id=NULL WHERE id=1");
    EXPECT_TRUE(f.Call().isMember("error"));EXPECT_EQ(f.Rows(),original);
    {auto lease=use->Wallet().AcquireDatabaseLease();sqlite3_stmt* q=nullptr;
     ASSERT_EQ(sqlite3_prepare_v2(db,"SELECT runtime_delivery_id IS NULL FROM wallet_meta WHERE id=1",-1,&q,nullptr),SQLITE_OK);
     ASSERT_EQ(sqlite3_step(q),SQLITE_ROW);EXPECT_EQ(sqlite3_column_int(q,0),1);sqlite3_finalize(q);}
    f.Sql("UPDATE wallet_meta SET runtime_delivery_id=(SELECT runtime_delivery_id FROM saved_issuance_identity) WHERE id=1");
    f.Sql("DROP TABLE saved_issuance_identity");
    const auto view=f.f.service->getRuntimeAccountReplay();ASSERT_TRUE(view.ok());const auto session=f.Session();
    EXPECT_THROW(wallet::OrchardAccountDelivery::IssueReceiverForReplay(use->Wallet(),session+1,{f.Domain(),102,0},**view,orchard::WalletScope::External),std::runtime_error);
    auto wrong=f.Domain();wrong.branch_id^=1;
    EXPECT_THROW(wallet::OrchardAccountDelivery::IssueReceiverForReplay(use->Wallet(),session,{wrong,102,0},**view,orchard::WalletScope::External),std::runtime_error);
    EXPECT_EQ(f.Rows(),original);EXPECT_EQ(OrchardAccountBytes(f.ReadAccount().account),OrchardAccountBytes(before.account));
    ExecutionContext named;named.daemon=&f.context;named.walletName="another-wallet";
    EXPECT_TRUE(rpc_context_wallet_orchard_getnewaddress(named,OrchardIssuanceRequest()).isMember("error"));EXPECT_EQ(f.Rows(),original);
    EXPECT_FALSE(f.Call().isMember("error"));
}
#endif
} // namespace dinero
