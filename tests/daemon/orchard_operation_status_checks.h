#pragma once
namespace dinero {
namespace {
din::Json OrchardOperationListRequest(uint32_t account=3) {
    din::Json params;params["account"]=Json::UInt64(account);return params;
}
}
TEST(OrchardOperationStatus, MalformedRequestsRefuseBeforeServices) {
    ExecutionContext context;
    std::vector<din::Json> invalid{din::arr(),din::Json{},din::Json("3")};
    for (const auto& value:std::vector<din::Json>{din::Json(-1),din::Json(0.5),din::Json(true),din::Json("3"),din::Json(Json::UInt64(0x80000000ULL))}) {
        din::Json p;p["account"]=value;invalid.push_back(p);
    }
    auto extra=OrchardOperationListRequest();extra["include_secret"]=true;invalid.push_back(extra);
    for (const auto& p:invalid) {
        const auto result=rpc_context_wallet_orchard_listoperations(context,p);
        EXPECT_TRUE(result.isMember("error"));EXPECT_FALSE(result.isMember("operations"));
        EXPECT_FALSE(result.isMember("account_revision"));
    }
}
TEST(OrchardOperationStatus, RegistryAndBackendOrServiceAbsenceRefuse) {
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.listoperations");ASSERT_NE(method,nullptr);
    ExecutionContext context;const auto result=(*method)(context,OrchardOperationListRequest());
    EXPECT_TRUE(result.isMember("error"));EXPECT_FALSE(result.isMember("operations"));
#ifndef DINERO_TEST_ORCHARD_ORIGIN
    EXPECT_EQ(result["error"].asString(),"Orchard wallet backend unavailable");
#endif
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct OrchardOperationStatusFixture : OrchardSpendOwnerFixture {
    din::Json List(uint32_t account=3) {return rpc_context_wallet_orchard_listoperations(RequestContext(),OrchardOperationListRequest(account));}
};
}
TEST(OrchardOperationStatus, ReservedSignedAndReopenPreserveDurableBytes) {
    OrchardOperationStatusFixture f;const auto empty_bytes=f.Snapshot();const auto empty=f.List();
    ASSERT_FALSE(empty.isMember("error"))<<empty["error"].asString();ASSERT_TRUE(empty["operations"].isArray());EXPECT_TRUE(empty["operations"].empty());EXPECT_EQ(f.Snapshot(),empty_bytes);
    auto request=f.Reserve();const auto reserved_bytes=f.Snapshot();const auto reserved=f.List();
    ASSERT_FALSE(reserved.isMember("error"))<<reserved["error"].asString();ASSERT_EQ(reserved["operations"].size(),1u);
    EXPECT_EQ(reserved["operations"][0]["operation_id"].asString(),std::string("01")+std::string(62,'0'));
    EXPECT_EQ(reserved["operations"][0]["durable_state"].asString(),"reserved");EXPECT_EQ(reserved["account_revision"].asUInt64(),request.revision);
    EXPECT_EQ(reserved["operations"][0].size(),2u);EXPECT_EQ(f.Snapshot(),reserved_bytes);
    const auto authorization=f.Prove(request);const auto ready=f.Ready(1,request.revision,authorization);const auto signed_bytes=f.Snapshot();
    const auto signed_state=f.List();ASSERT_FALSE(signed_state.isMember("error"));EXPECT_EQ(signed_state["operations"][0]["durable_state"].asString(),"signed");
    EXPECT_EQ(signed_state["account_revision"].asUInt64(),ready.revision);EXPECT_EQ(signed_state["operations"][0].size(),2u);EXPECT_EQ(f.Snapshot(),signed_bytes);
    const auto other=f.List(17);ASSERT_FALSE(other.isMember("error"));EXPECT_TRUE(other["operations"].empty());EXPECT_EQ(f.Snapshot(),signed_bytes);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");}
    EXPECT_TRUE(f.List().isMember("error"));EXPECT_EQ(f.Snapshot(),signed_bytes);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_EQ(f.List(),signed_state);EXPECT_EQ(f.Snapshot(),signed_bytes);
}
TEST(OrchardOperationStatus, IncompleteCatalogAndSqlReadRefuseWithoutPrefix) {
    OrchardOperationStatusFixture f;auto request=f.Reserve();(void)request;
    const auto refuse=[&] {const auto before=f.Snapshot();const auto result=f.List();EXPECT_TRUE(result.isMember("error"));EXPECT_FALSE(result.isMember("operations"));EXPECT_FALSE(result.isMember("account_revision"));EXPECT_EQ(f.Snapshot(),before);};
    f.Sql("CREATE TEMP TABLE saved_status_other AS SELECT * FROM orchard_wallet_snapshots WHERE account=17; DELETE FROM orchard_wallet_snapshots WHERE account=17");refuse();
    f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_status_other; DROP TABLE saved_status_other");
    auto* db=f.Database();const auto before=f.Snapshot();
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*) {
        return action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_retained"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    const auto denied=f.List();sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(denied.isMember("error"));EXPECT_FALSE(denied.isMember("operations"));EXPECT_EQ(f.Snapshot(),before);
    struct Interrupt {sqlite3* db;bool seen=false;} interrupt{db};
    sqlite3_trace_v2(db,SQLITE_TRACE_ROW,[](unsigned,void* arg,void* stmt,void*) {
        auto& state=*static_cast<Interrupt*>(arg);const char* sql=sqlite3_sql(static_cast<sqlite3_stmt*>(stmt));
        if (!state.seen&&sql&&std::string_view(sql)=="SELECT wallet_id,account,revision FROM orchard_wallet_snapshots ORDER BY account") {state.seen=true;sqlite3_interrupt(state.db);}return 0;
    },&interrupt);
    const auto incomplete=f.List();sqlite3_trace_v2(db,0,nullptr,nullptr);EXPECT_TRUE(interrupt.seen);EXPECT_TRUE(incomplete.isMember("error"));EXPECT_FALSE(incomplete.isMember("operations"));EXPECT_EQ(f.Snapshot(),before);
    auto borrowed_use=WalletService::AcquireWalletUse(f.wallet);auto borrowed_lease=borrowed_use->Wallet().AcquireDatabaseLease();f.Sql("BEGIN IMMEDIATE");const auto borrowed=f.List();EXPECT_TRUE(borrowed.isMember("error"));EXPECT_FALSE(borrowed.isMember("operations"));EXPECT_FALSE(sqlite3_get_autocommit(db));f.Sql("ROLLBACK");borrowed_lease.reset();borrowed_use.reset();EXPECT_EQ(f.Snapshot(),before);
    const auto retry=f.List();ASSERT_FALSE(retry.isMember("error"))<<retry["error"].asString();EXPECT_EQ(retry["operations"].size(),1u);EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardOperationStatus, WalletBindingLockUnknownCatalogAndCapturedLag) {
    OrchardOperationStatusFixture f;auto request=f.Reserve();(void)request;auto context=f.RequestContext();context.walletName="different-wallet";
    const auto before=f.Snapshot();const auto wrong=rpc_context_wallet_orchard_listoperations(context,OrchardOperationListRequest());EXPECT_TRUE(wrong.isMember("error"));EXPECT_FALSE(wrong.isMember("operations"));
    EXPECT_TRUE(f.List(29).isMember("error"));EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().lockWallet();}
    const auto locked=f.List();EXPECT_TRUE(locked.isMember("error"));EXPECT_FALSE(locked.isMember("operations"));EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    const auto catalog=f.Setting("orchard_account_catalog_v1");f.Sql("DELETE FROM settings WHERE key='orchard_account_catalog_v1'");
    const auto missing_bytes=f.Snapshot();const auto missing=f.List();EXPECT_TRUE(missing.isMember("error"));EXPECT_FALSE(missing.isMember("operations"));EXPECT_EQ(f.Snapshot(),missing_bytes);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().setSetting("orchard_account_catalog_v1",catalog);}
    const auto prior=f.List();ASSERT_FALSE(prior.isMember("error"));f.MineEmpty();const auto lag_bytes=f.Snapshot();const auto lag=f.List();
    ASSERT_FALSE(lag.isMember("error"))<<lag["error"].asString();EXPECT_EQ(lag["operations"],prior["operations"]);
    EXPECT_LT(lag["account_sequence"].asUInt64(),lag["captured_source_sequence"].asUInt64());EXPECT_FALSE(lag.isMember("spendable"));EXPECT_EQ(f.Snapshot(),lag_bytes);
}
#endif
} // namespace dinero
