#pragma once
namespace dinero {
namespace {
din::Json OrchardOperationListRequest(uint32_t account=3) {
    din::Json params;params["account"]=Json::UInt64(account);return params;
}
}
TEST(OrchardOperationStatus, ReceivedHistoryStrictParametersAndBackendPolicy) {
    ExecutionContext ctx;din::Json valid;valid["account"]=Json::UInt64(3);
    const auto unavailable=rpc_context_wallet_orchard_listreceived(ctx,valid);
    EXPECT_TRUE(unavailable.isMember("error"));EXPECT_FALSE(unavailable.isMember("received"));
#ifndef DINERO_TEST_ORCHARD_ORIGIN
    EXPECT_EQ(unavailable["error"].asString(),"Orchard wallet backend unavailable");
#else
    EXPECT_EQ(unavailable["error"].asString(),"Daemon services unavailable");
#endif
    std::vector<din::Json> invalid{din::Json{},din::arr(),din::Json(3)};
    for(const auto& field:{"offset","limit","expected_revision"}) {
        for(const auto& value:std::vector<din::Json>{din::Json(-1),din::Json(0.5),din::Json(true),din::Json("1")}) {
            auto p=valid;p[field]=value;invalid.push_back(p);
        }
    }
    auto p=valid;p["offset"]=1;invalid.push_back(p);p=valid;p["limit"]=0;invalid.push_back(p);
    p=valid;p["limit"]=1001;invalid.push_back(p);p=valid;p["expected_revision"]=0;invalid.push_back(p);
    p=valid;p["unknown"]=1;invalid.push_back(p);
    for(const auto& bad:invalid) {const auto r=rpc_context_wallet_orchard_listreceived(ctx,bad);EXPECT_TRUE(r.isMember("error"));EXPECT_FALSE(r.isMember("received"));EXPECT_NE(r["error"],unavailable["error"]);}
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.listreceived");ASSERT_NE(method,nullptr);
    EXPECT_EQ((*method)(ctx,valid)["error_code"].asString(),"wallet_binding_required");
}
TEST(OrchardOperationStatus, AccountListStrictParametersAndBackendPolicy) {
    ExecutionContext ctx;din::Json empty(Json::objectValue);
    const auto unavailable=rpc_context_wallet_orchard_listaccounts(ctx,empty);
    EXPECT_TRUE(unavailable.isMember("error"));EXPECT_FALSE(unavailable.isMember("accounts"));
#ifndef DINERO_TEST_ORCHARD_ORIGIN
    EXPECT_EQ(unavailable["error"].asString(),"Orchard wallet backend unavailable");
#else
    EXPECT_EQ(unavailable["error"].asString(),"Daemon services unavailable");
#endif
    for(const auto& invalid:std::vector<din::Json>{din::Json{},din::arr(),din::Json(3),OrchardOperationListRequest()}) {
        const auto rejected=rpc_context_wallet_orchard_listaccounts(ctx,invalid);
        EXPECT_TRUE(rejected.isMember("error"));EXPECT_FALSE(rejected.isMember("accounts"));EXPECT_NE(rejected["error"],unavailable["error"]);
    }
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.listaccounts");ASSERT_NE(method,nullptr);
    EXPECT_TRUE((*method)(ctx,OrchardBoundParamsForTest(ctx,empty)).isMember("error"));
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
    ExecutionContext context;const auto result=(*method)(context,OrchardBoundParamsForTest(context,OrchardOperationListRequest()));
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
TEST(OrchardOperationStatus, ReceivedHistoryRetainsSpentReceiptAndPagesAcrossReopenReorg) {
    OrchardOperationStatusFixture f;
    const auto list=[&](uint32_t account,uint64_t offset=0,uint64_t revision=0) {
        auto p=OrchardOperationListRequest(account);p["offset"]=Json::UInt64(offset);p["limit"]=1;
        if(revision)p["expected_revision"]=Json::UInt64(revision);
        return rpc_context_wallet_orchard_listreceived(f.RequestContext(),p);
    };
    const auto before=f.Snapshot();const auto initial=list(3);ASSERT_FALSE(initial.isMember("error"))<<initial.toStyledString();
    ASSERT_EQ(initial["received"].size(),1u);EXPECT_EQ(initial["received"][0]["amount_una"].asUInt64(),500000u);EXPECT_EQ(initial["received"][0]["scope"].asString(),"external");
    EXPECT_TRUE(initial["history_complete"].asBool());EXPECT_TRUE(initial["next_offset"].isNull());EXPECT_EQ(f.Snapshot(),before);
    auto request=f.Reserve();const auto authorization=f.Prove(request);(void)f.Ready(1,request.revision,authorization);
    const auto body=MempoolTransaction::FromOrchard(authorization.Transaction());const auto included=f.Mine(body);ASSERT_TRUE(included);
    const auto lag=list(3);ASSERT_FALSE(lag.isMember("error"));EXPECT_FALSE(lag["account_caught_up_to_captured_source"].asBool());EXPECT_EQ(lag["total_count"].asUInt64(),1u);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto stored=f.Snapshot();const auto first=list(3);ASSERT_FALSE(first.isMember("error"))<<first.toStyledString();
    EXPECT_EQ(first["total_count"].asUInt64(),2u);EXPECT_EQ(first["next_offset"].asUInt64(),1u);EXPECT_EQ(first["received"][0],initial["received"][0]);
    const auto revision=first["account_revision"].asUInt64();const auto second=list(3,1,revision);ASSERT_FALSE(second.isMember("error"));ASSERT_EQ(second["received"].size(),1u);
    EXPECT_EQ(second["received"][0]["amount_una"].asUInt64(),200000u);EXPECT_EQ(second["received"][0]["scope"].asString(),"internal");EXPECT_EQ(second["received"][0]["txid"].asString(),body.GetTxid().AsUint256().GetHex());
    EXPECT_EQ(second["received"][0]["recipient_hex"].asString().size(),86u);EXPECT_EQ(second["received"][0]["memo_hex"].asString().size(),1024u);EXPECT_TRUE(second["next_offset"].isNull());
    const auto eof=list(3,2,revision);ASSERT_FALSE(eof.isMember("error"));EXPECT_TRUE(eof["received"].empty());EXPECT_TRUE(eof["next_offset"].isNull());
    const auto stale=list(3,1,revision-1);EXPECT_EQ(stale["error_code"].asString(),"stale_account_revision");EXPECT_FALSE(stale.isMember("received"));
    const auto target=list(17);ASSERT_FALSE(target.isMember("error"));ASSERT_EQ(target["received"].size(),1u);EXPECT_EQ(target["received"][0]["amount_una"].asUInt64(),200000u);EXPECT_EQ(target["received"][0]["scope"].asString(),"external");EXPECT_EQ(f.Snapshot(),stored);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_EQ(list(3),first);EXPECT_EQ(list(3,1,revision),second);EXPECT_EQ(f.Snapshot(),stored);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto undone=list(3);ASSERT_FALSE(undone.isMember("error"));EXPECT_EQ(undone["total_count"].asUInt64(),1u);EXPECT_EQ(undone["received"][0],initial["received"][0]);const auto undone_target=list(17);ASSERT_FALSE(undone_target.isMember("error"));ASSERT_TRUE(undone_target["received"].isArray());EXPECT_TRUE(undone_target["received"].empty());
    EXPECT_EQ(list(3,1,revision)["error_code"].asString(),"stale_account_revision");
    const auto restored=f.Submit(included->Orchard().WireBytes());ASSERT_TRUE(restored.accepted()&&restored.connected)<<restored.reason;ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto again=list(3);ASSERT_FALSE(again.isMember("error"));EXPECT_EQ(again["total_count"].asUInt64(),2u);EXPECT_EQ(again["received"],first["received"]);
}
TEST(OrchardOperationStatus, ReceivedHistoryUsesWholeCatalogAndWalletBinding) {
    OrchardOperationStatusFixture f;const auto p=OrchardOperationListRequest();const auto initial=f.Snapshot();auto wrong=f.RequestContext();wrong.walletName="another-wallet";
    const auto mismatch=rpc_context_wallet_orchard_listreceived(wrong,p);EXPECT_TRUE(mismatch.isMember("error"));EXPECT_FALSE(mismatch.isMember("received"));EXPECT_EQ(f.Snapshot(),initial);
    f.Sql("CREATE TEMP TABLE saved_receipt_account AS SELECT * FROM orchard_wallet_snapshots WHERE account=17; DELETE FROM orchard_wallet_snapshots WHERE account=17");
    const auto missing=f.Snapshot();const auto result=rpc_context_wallet_orchard_listreceived(f.RequestContext(),p);EXPECT_TRUE(result.isMember("error"));EXPECT_FALSE(result.isMember("received"));EXPECT_EQ(f.Snapshot(),missing);
    f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_receipt_account; DROP TABLE saved_receipt_account");const auto retry=rpc_context_wallet_orchard_listreceived(f.RequestContext(),p);ASSERT_FALSE(retry.isMember("error"));EXPECT_EQ(retry["total_count"].asUInt64(),1u);EXPECT_EQ(f.Snapshot(),initial);
}
TEST(OrchardOperationStatus, AccountListUsesWholeCatalogAndReopensWithoutMutation) {
    OrchardOperationStatusFixture f;din::Json params(Json::objectValue);
    const auto list=[&](){return rpc_context_wallet_orchard_listaccounts(f.RequestContext(),params);};
    const auto before=f.Snapshot();const auto result=list();ASSERT_FALSE(result.isMember("error"))<<result.toStyledString();
    ASSERT_TRUE(result["accounts"].isArray());ASSERT_EQ(result["accounts"].size(),2u);std::set<uint64_t> numbers;
    for(const auto& account:result["accounts"]) {
        ASSERT_TRUE(account["account"].isUInt64());const auto n=account["account"].asUInt64();numbers.insert(n);
        const auto expected=f.List(static_cast<uint32_t>(n));ASSERT_FALSE(expected.isMember("error"));
        for(const auto* field:{"account","account_revision","account_sequence","account_digest"})EXPECT_EQ(account[field],expected[field]);
        EXPECT_EQ(result["captured_source_sequence"],expected["captured_source_sequence"]);EXPECT_EQ(result["captured_source_digest"],expected["captured_source_digest"]);
        EXPECT_EQ(account["checkpoint_height"].asUInt(),f.Account(static_cast<uint32_t>(n)).account.Scan().Checkpoint().height);
    }
    EXPECT_EQ(numbers,(std::set<uint64_t>{3,17}));EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_EQ(list(),result);EXPECT_EQ(f.Snapshot(),before);
    f.Sql("CREATE TEMP TABLE saved_account_list AS SELECT * FROM orchard_wallet_snapshots WHERE account=17; DELETE FROM orchard_wallet_snapshots WHERE account=17");
    const auto missing=f.Snapshot();const auto refused=list();EXPECT_TRUE(refused.isMember("error"));EXPECT_FALSE(refused.isMember("accounts"));EXPECT_EQ(f.Snapshot(),missing);
    f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_account_list; DROP TABLE saved_account_list");EXPECT_EQ(list(),result);EXPECT_EQ(f.Snapshot(),before);
    auto wrong=f.RequestContext();wrong.walletName="another-wallet";const auto mismatch=rpc_context_wallet_orchard_listaccounts(wrong,params);
    EXPECT_TRUE(mismatch.isMember("error"));EXPECT_FALSE(mismatch.isMember("accounts"));EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardOperationStatus, ReservedSignedAndReopenPreserveDurableBytes) {
    OrchardOperationStatusFixture f;const auto empty_bytes=f.Snapshot();const auto empty=f.List();
    ASSERT_FALSE(empty.isMember("error"))<<empty["error"].asString();ASSERT_TRUE(empty["operations"].isArray());EXPECT_TRUE(empty["operations"].empty());EXPECT_EQ(f.Snapshot(),empty_bytes);
    auto request=f.Reserve();const auto reserved_bytes=f.Snapshot();const auto reserved=f.List();
    ASSERT_FALSE(reserved.isMember("error"))<<reserved["error"].asString();ASSERT_EQ(reserved["operations"].size(),1u);
    EXPECT_EQ(reserved["operations"][0]["operation_id"].asString(),std::string("01")+std::string(62,'0'));
    EXPECT_EQ(reserved["operations"][0]["durable_state"].asString(),"reserved");EXPECT_EQ(reserved["account_revision"].asUInt64(),request.revision);
    EXPECT_FALSE(reserved["operations"][0].isMember("completion_method"));
    EXPECT_EQ(reserved["operations"][0].size(),3u);EXPECT_TRUE(reserved["operations"][0]["chain_observation"].isNull());EXPECT_FALSE(reserved["operations"][0].isMember("txid"));EXPECT_EQ(f.Snapshot(),reserved_bytes);
    const auto authorization=f.Prove(request);const auto ready=f.Ready(1,request.revision,authorization);const auto signed_bytes=f.Snapshot();
    const auto signed_state=f.List();ASSERT_FALSE(signed_state.isMember("error"));EXPECT_EQ(signed_state["operations"][0]["durable_state"].asString(),"signed");
    EXPECT_FALSE(signed_state["operations"][0].isMember("completion_method"));
    EXPECT_EQ(signed_state["account_revision"].asUInt64(),ready.revision);EXPECT_EQ(signed_state["operations"][0].size(),4u);EXPECT_TRUE(signed_state["operations"][0]["chain_observation"].isNull());EXPECT_EQ(signed_state["operations"][0]["txid"].asString(),MempoolTransaction::FromOrchard(authorization.Transaction()).GetTxid().AsUint256().GetHex());EXPECT_EQ(f.Snapshot(),signed_bytes);
    const auto other=f.List(17);ASSERT_FALSE(other.isMember("error"));EXPECT_TRUE(other["operations"].empty());EXPECT_EQ(f.Snapshot(),signed_bytes);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");}
    EXPECT_TRUE(f.List().isMember("error"));EXPECT_EQ(f.Snapshot(),signed_bytes);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_EQ(f.List(),signed_state);EXPECT_EQ(f.Snapshot(),signed_bytes);
}
TEST(OrchardOperationStatus, ActualConfirmationLagUndoAndReinclusionRemainReadOnly) {
    OrchardOperationStatusFixture f;auto request=f.Reserve();const auto authorization=f.Prove(request);
    (void)f.Ready(1,request.revision,authorization);
    const auto body=MempoolTransaction::FromOrchard(authorization.Transaction());const auto txid=body.GetTxid().AsUint256().GetHex();
    const auto included=f.Mine(body);ASSERT_TRUE(included);
    const auto lag_bytes=f.Snapshot();const auto lag=f.List();ASSERT_FALSE(lag.isMember("error"))<<lag.toStyledString();
    ASSERT_EQ(lag["operations"].size(),1u);EXPECT_TRUE(lag["operations"][0]["chain_observation"].isNull());
    EXPECT_EQ(lag["operations"][0]["txid"].asString(),txid);
    EXPECT_LT(lag["account_sequence"].asUInt64(),lag["captured_source_sequence"].asUInt64());EXPECT_EQ(f.Snapshot(),lag_bytes);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto confirmed_bytes=f.Snapshot();const auto confirmed=f.List();ASSERT_FALSE(confirmed.isMember("error"))<<confirmed.toStyledString();
    ASSERT_EQ(confirmed["operations"].size(),1u);const auto& observation=confirmed["operations"][0]["chain_observation"];
    EXPECT_EQ(confirmed["operations"][0]["txid"].asString(),txid);ASSERT_EQ(observation.size(),4u);
    EXPECT_EQ(observation["outcome"].asString(),"confirmed");EXPECT_EQ(observation["transaction_id"].asString(),txid);
    EXPECT_EQ(observation["block_hash"].asString(),included->Orchard().Header().GetHash().GetHex());
    EXPECT_EQ(observation["height"].asUInt(),included->Context()->height);
    EXPECT_EQ(confirmed["account_sequence"],confirmed["captured_source_sequence"]);EXPECT_EQ(f.Snapshot(),confirmed_bytes);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_EQ(f.List(),confirmed);EXPECT_EQ(f.Snapshot(),confirmed_bytes);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto undone_bytes=f.Snapshot();const auto undone=f.List();ASSERT_FALSE(undone.isMember("error"))<<undone.toStyledString();
    ASSERT_EQ(undone["operations"].size(),1u);EXPECT_TRUE(undone["operations"][0]["chain_observation"].isNull());
    EXPECT_EQ(undone["operations"][0]["txid"].asString(),txid);EXPECT_EQ(f.Snapshot(),undone_bytes);
    const auto restored=f.Submit(included->Orchard().WireBytes());ASSERT_TRUE(restored.accepted()&&restored.connected)<<restored.reason;
    ASSERT_EQ(f.f.service->GetActiveTip()->hash,included->Orchard().Header().GetHash());
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto final_bytes=f.Snapshot();const auto again=f.List();ASSERT_FALSE(again.isMember("error"))<<again.toStyledString();
    ASSERT_EQ(again["operations"].size(),1u);EXPECT_EQ(again["operations"][0]["txid"].asString(),txid);
    EXPECT_EQ(again["operations"][0]["chain_observation"]["outcome"].asString(),"confirmed");
    EXPECT_EQ(again["operations"][0]["chain_observation"]["transaction_id"].asString(),txid);
    EXPECT_EQ(again["operations"][0]["chain_observation"]["block_hash"].asString(),included->Orchard().Header().GetHash().GetHex());
    EXPECT_EQ(f.Snapshot(),final_bytes);
}
TEST(OrchardOperationStatus, ActualConflictingInclusionRetainsReservationAndRewindsObservation) {
    OrchardOperationStatusFixture f;auto request=f.Reserve();const auto authorization=f.Prove(request);
    // A real valid transaction consumes the reserved note before this wallet
    // has committed any Ready body. Replay must report the winning transaction
    // as a conflict, not infer an owned signed transaction from the proof plan.
    const auto body=MempoolTransaction::FromOrchard(authorization.Transaction());const auto winner=body.GetTxid().AsUint256().GetHex();
    const auto included=f.Mine(body);ASSERT_TRUE(included);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto before=f.Snapshot();const auto listed=f.List();ASSERT_FALSE(listed.isMember("error"))<<listed.toStyledString();
    ASSERT_EQ(listed["operations"].size(),1u);const auto& entry=listed["operations"][0];
    EXPECT_EQ(entry["durable_state"].asString(),"reserved");EXPECT_FALSE(entry.isMember("txid"));
    const auto& observed=entry["chain_observation"];ASSERT_EQ(observed.size(),4u);
    EXPECT_EQ(observed["outcome"].asString(),"conflicted");EXPECT_EQ(observed["transaction_id"].asString(),winner);
    EXPECT_EQ(observed["block_hash"].asString(),included->Orchard().Header().GetHash().GetHex());
    EXPECT_EQ(observed["height"].asUInt(),included->Context()->height);EXPECT_EQ(f.Snapshot(),before);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto undone_bytes=f.Snapshot();const auto undone=f.List();ASSERT_FALSE(undone.isMember("error"))<<undone.toStyledString();
    ASSERT_EQ(undone["operations"].size(),1u);EXPECT_EQ(undone["operations"][0]["durable_state"].asString(),"reserved");
    EXPECT_FALSE(undone["operations"][0].isMember("txid"));EXPECT_TRUE(undone["operations"][0]["chain_observation"].isNull());
    EXPECT_EQ(f.Snapshot(),undone_bytes);EXPECT_THROW(f.Reserve(2,1),std::runtime_error);EXPECT_EQ(f.Snapshot(),undone_bytes);
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
        if (!state.seen&&sql&&std::string_view(sql)=="SELECT wallet_id,account,revision FROM orchard_wallet_snapshots ORDER BY wallet_id,account") {state.seen=true;sqlite3_interrupt(state.db);}return 0;
    },&interrupt);
    const auto incomplete=f.List();sqlite3_trace_v2(db,0,nullptr,nullptr);EXPECT_TRUE(interrupt.seen);EXPECT_TRUE(incomplete.isMember("error"));EXPECT_FALSE(incomplete.isMember("operations"));EXPECT_EQ(f.Snapshot(),before);
    auto borrowed_use=WalletService::AcquireWalletUse(f.wallet);auto borrowed_lease=borrowed_use->Wallet().AcquireDatabaseLease();f.Sql("BEGIN IMMEDIATE");const auto borrowed=f.List();EXPECT_TRUE(borrowed.isMember("error"));EXPECT_FALSE(borrowed.isMember("operations"));EXPECT_FALSE(sqlite3_get_autocommit(db));f.Sql("ROLLBACK");borrowed_lease.reset();borrowed_use.reset();EXPECT_EQ(f.Snapshot(),before);
    const auto retry=f.List();ASSERT_FALSE(retry.isMember("error"))<<retry["error"].asString();EXPECT_EQ(retry["operations"].size(),1u);EXPECT_EQ(f.Snapshot(),before);
}
TEST(OrchardOperationStatus, CatalogSqlFailureIsNotRetried) {
    OrchardOperationStatusFixture f;auto request=f.Reserve();(void)request;
    auto* db=f.Database();const auto before=f.Snapshot();size_t denied=0;
    ASSERT_EQ(sqlite3_set_authorizer(db,[](void* value,int action,const char* table,const char*,const char*,const char*) {
        if(action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_snapshots"){
            ++*static_cast<size_t*>(value);return SQLITE_DENY;
        }
        return SQLITE_OK;
    },&denied),SQLITE_OK);
    const auto result=f.List();sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_EQ(denied,1u);EXPECT_TRUE(result.isMember("error"));
    EXPECT_FALSE(result.isMember("operations"));EXPECT_FALSE(result.isMember("account_revision"));
    EXPECT_EQ(f.Snapshot(),before);
    const auto stable=f.List();ASSERT_FALSE(stable.isMember("error"))<<stable["error"].asString();
    EXPECT_EQ(stable["operations"].size(),1u);EXPECT_EQ(f.Snapshot(),before);
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

namespace dinero {
TEST(OrchardBalance, MalformedRequestsNeverReturnAmounts) {
    ExecutionContext context;
    std::vector<din::Json> invalid{din::arr(),din::Json{},din::Json("3")};
    for (const auto& value:std::vector<din::Json>{din::Json(-1),din::Json(0.5),din::Json(true),din::Json("3"),din::Json(Json::UInt64(0x80000000ULL))}) {
        din::Json p;p["account"]=value;invalid.push_back(p);
    }
    auto extra=OrchardOperationListRequest();extra["include_secret"]=true;invalid.push_back(extra);
    for (const auto& p:invalid) {
        const auto result=rpc_context_wallet_orchard_getbalance(context,p);
        ASSERT_TRUE(result.isMember("error"));EXPECT_EQ(result.size(),1u);
    }
}
TEST(OrchardBalance, RegistryAndUnavailableBackendOrServiceReturnError) {
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.getbalance");ASSERT_NE(method,nullptr);
    ExecutionContext context;const auto result=(*method)(context,OrchardBoundParamsForTest(context,OrchardOperationListRequest()));
    ASSERT_TRUE(result.isMember("error"));EXPECT_EQ(result.size(),1u);
#ifndef DINERO_TEST_ORCHARD_ORIGIN
    EXPECT_EQ(result["error"].asString(),"Orchard wallet backend unavailable");
#endif
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct OrchardBalanceFixture : OrchardOperationStatusFixture {
    din::Json Balance(uint32_t account=3) {return rpc_context_wallet_orchard_getbalance(RequestContext(),OrchardOperationListRequest(account));}
};
void ExpectOrchardAmounts(const din::Json& result,uint64_t confirmed,uint64_t reserved) {
    ASSERT_FALSE(result.isMember("error"))<<result.toStyledString();
    EXPECT_EQ(result["confirmed_una"].type(),Json::uintValue);
    EXPECT_EQ(result["confirmed_una"].asUInt64(),confirmed);
    EXPECT_EQ(result["reserved_confirmed_una"].asUInt64(),reserved);
    EXPECT_EQ(result["unreserved_confirmed_una"].asUInt64(),confirmed-reserved);
    EXPECT_FALSE(result.isMember("spendable"));EXPECT_FALSE(result.isMember("pending_incoming_una"));
}
}
TEST(OrchardBalance, ReservationSignedAndReopenPreserveAmountsAndBytes) {
    OrchardBalanceFixture f;const auto initial_bytes=f.Snapshot();const auto initial=f.Balance();
    ExpectOrchardAmounts(initial,500000,0);EXPECT_TRUE(initial["account_caught_up_to_captured_source"].asBool());
    ExpectOrchardAmounts(f.Balance(17),0,0);EXPECT_EQ(f.Snapshot(),initial_bytes);
    auto request=f.Reserve();const auto reserved_bytes=f.Snapshot();const auto reserved=f.Balance();
    ExpectOrchardAmounts(reserved,500000,500000);EXPECT_EQ(reserved["account_revision"].asUInt64(),request.revision);EXPECT_EQ(f.Snapshot(),reserved_bytes);
    const auto authorization=f.Prove(request);(void)f.Ready(1,request.revision,authorization);
    const auto ready_bytes=f.Snapshot();const auto ready=f.Balance();ExpectOrchardAmounts(ready,500000,500000);EXPECT_EQ(f.Snapshot(),ready_bytes);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");}
    const auto locked=f.Balance();ASSERT_TRUE(locked.isMember("error"));EXPECT_EQ(locked.size(),1u);EXPECT_EQ(f.Snapshot(),ready_bytes);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_EQ(f.Balance(),ready);EXPECT_EQ(f.Snapshot(),ready_bytes);
}
TEST(OrchardBalance, TransferLagIncomingAndUndoUseActualCheckpoint) {
    OrchardBalanceFixture f;auto request=f.Reserve();const auto authorization=f.Prove(request);(void)f.Ready(1,request.revision,authorization);
    const auto before=f.Balance();const auto included=f.Mine(MempoolTransaction::FromOrchard(authorization.Transaction()));ASSERT_TRUE(included);
    const auto lag_bytes=f.Snapshot();const auto lag=f.Balance();ExpectOrchardAmounts(lag,500000,500000);
    EXPECT_FALSE(lag["account_caught_up_to_captured_source"].asBool());EXPECT_EQ(lag["checkpoint_hash"],before["checkpoint_hash"]);
    EXPECT_LT(lag["account_sequence"].asUInt64(),lag["captured_source_sequence"].asUInt64());ExpectOrchardAmounts(f.Balance(17),0,0);EXPECT_EQ(f.Snapshot(),lag_bytes);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto synced_bytes=f.Snapshot();const auto synced=f.Balance();ExpectOrchardAmounts(synced,200000,0);ExpectOrchardAmounts(f.Balance(17),200000,0);
    EXPECT_TRUE(synced["account_caught_up_to_captured_source"].asBool());EXPECT_EQ(synced["checkpoint_hash"].asString(),included->Orchard().Header().GetHash().GetHex());EXPECT_EQ(f.Snapshot(),synced_bytes);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    const auto undo_bytes=f.Snapshot();ExpectOrchardAmounts(f.Balance(),500000,500000);ExpectOrchardAmounts(f.Balance(17),0,0);EXPECT_EQ(f.Snapshot(),undo_bytes);
    EXPECT_THROW(f.Reserve(2,1),std::runtime_error);
}
TEST(OrchardBalance, UnknownLockedIncompleteAndFailedReadNeverReturnZero) {
    OrchardBalanceFixture f;auto context=f.RequestContext();context.walletName="different-wallet";
    const auto refuse=[](const din::Json& result){ASSERT_TRUE(result.isMember("error"))<<result.toStyledString();EXPECT_EQ(result.size(),1u);};
    const auto before=f.Snapshot();refuse(rpc_context_wallet_orchard_getbalance(context,OrchardOperationListRequest()));refuse(f.Balance(29));EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().lockWallet();}
    refuse(f.Balance());EXPECT_EQ(f.Snapshot(),before);
    {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    f.Sql("CREATE TEMP TABLE saved_balance_owner AS SELECT * FROM orchard_wallet_snapshots WHERE account=17; DELETE FROM orchard_wallet_snapshots WHERE account=17");
    const auto missing=f.Snapshot();refuse(f.Balance());EXPECT_EQ(f.Snapshot(),missing);
    f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM saved_balance_owner; DROP TABLE saved_balance_owner");
    auto* db=f.Database();size_t denied=0;
    ASSERT_EQ(sqlite3_set_authorizer(db,[](void* value,int action,const char* table,const char*,const char*,const char*) {
        if(action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_snapshots") {++*static_cast<size_t*>(value);return SQLITE_DENY;}return SQLITE_OK;
    },&denied),SQLITE_OK);
    const auto failed=f.Balance();sqlite3_set_authorizer(db,nullptr,nullptr);refuse(failed);EXPECT_EQ(denied,1u);EXPECT_EQ(f.Snapshot(),before);
    auto borrowed_use=WalletService::AcquireWalletUse(f.wallet);auto lease=borrowed_use->Wallet().AcquireDatabaseLease();f.Sql("BEGIN IMMEDIATE");
    const auto borrowed=f.Balance();refuse(borrowed);EXPECT_FALSE(sqlite3_get_autocommit(db));f.Sql("ROLLBACK");lease.reset();borrowed_use.reset();
    ExpectOrchardAmounts(f.Balance(),500000,0);EXPECT_EQ(f.Snapshot(),before);
}
#endif
} // namespace dinero
