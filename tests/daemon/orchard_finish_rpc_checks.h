#pragma once
#include "daemon/services/mempool_service.h"
#include "consensus/orchard_authorization.h"
namespace dinero {
TEST(OrchardFinishRpc, MalformedRequestsRefuseBeforeServices){
    ExecutionContext context;
    for(const auto& params:std::vector<din::Json>{din::arr(),din::Json{},din::Json("request")}){
        const auto result=rpc_context_wallet_orchard_finishspend(context,params);SpendRpcRefused(result);
        EXPECT_NE(result["error"].asString(),"Daemon services unavailable");
        EXPECT_NE(result["error"].asString(),"Orchard wallet backend unavailable");
    }
    auto params=SpendRpcShape();params["fee_una"]=true;SpendRpcRefused(rpc_context_wallet_orchard_finishspend(context,params));
    params=SpendRpcShape();params["proof"]="not caller proof authority";SpendRpcRefused(rpc_context_wallet_orchard_finishspend(context,params));
}
TEST(OrchardFinishRpc, RegistryAndBackendOrServiceAbsenceRefuse){
    RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.finishspend");ASSERT_NE(method,nullptr);
    ExecutionContext context;const auto result=(*method)(context,OrchardBoundParamsForTest(context,SpendRpcShape()));SpendRpcRefused(result);
#ifndef DINERO_TEST_ORCHARD_ORIGIN
    EXPECT_EQ(result["error"].asString(),"Orchard wallet backend unavailable");
#else
    EXPECT_EQ(result["error"].asString(),"Daemon services unavailable");
#endif
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct OrchardFinishRpcFixture : OrchardSpendRpcFixture {
    unsigned broadcasts=0;
    bool ready_at_broadcast=false,autocommit_at_broadcast=false,retired_at_broadcast=false;
    std::vector<uint8_t> broadcast_body;
    OrchardFinishRpcFixture(){
        context.tx_ingress=f.ingress.get();
        f.ingress->mempool().setTxBroadcastCallback([this](const uint256& txid){
            ++broadcasts;
            autocommit_at_broadcast=sqlite3_get_autocommit(Database());
            const auto account=Account(3);const auto& entry=account.account.Operations().Entries().at(Operation(1));
            ready_at_broadcast=entry.phase==wallet::OrchardOperationQueue::Phase::Ready;
            const auto pooled=f.ingress->mempool().getMempoolEntry(txid);
            OrchardAdmissionFixture::Require(bool(pooled));broadcast_body=pooled->tx.Serialize();
            ready_at_broadcast=ready_at_broadcast&&entry.transaction==broadcast_body;
            auto use=WalletService::AcquireWalletUse(wallet);retired_at_broadcast=!use->OrchardProofs().Query(Operation(1));
        });
    }
    ~OrchardFinishRpcFixture(){f.ingress->mempool().setTxBroadcastCallback({});context.tx_ingress=nullptr;}
    auto Finish(const din::Json& params){return rpc_context_wallet_orchard_finishspend(RequestContext(),params);}
    void CompleteProof(){auto use=WalletService::AcquireWalletUse(wallet);
        OrchardAdmissionFixture::Require(ServiceProofTerminal(use->OrchardProofs(),Operation(1))==wallet::OrchardProofJobs::State::Succeeded);}
    void ExpectUnsubmitted(){EXPECT_EQ(broadcasts,0u);EXPECT_EQ(f.ingress->mempool().size(),0u);}
};
}
TEST(OrchardFinishRpc, ActualTransferAndUnshieldCommitBeforeAdmissionAndRelay){
    for(const bool withdraw:{false,true}){
        OrchardFinishRpcFixture f;auto params=f.RequestJson();
        if(withdraw){params["payments"]=din::arr();din::Json output;output["address"]=f.TransparentAddress();output["amount_una"]=Json::UInt64(400000);params["outputs"].append(output);}
        ASSERT_FALSE(f.CallSpend(params).isMember("error"));f.CompleteProof();f.ExpectUnsubmitted();
        RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.finishspend");ASSERT_NE(method,nullptr);
        const auto result=(*method)(f.RequestContext(),OrchardBoundParamsForTest(f.RequestContext(),params));ASSERT_FALSE(result.isMember("error"))<<result["error"].asString();
        EXPECT_TRUE(result["admitted"].asBool());EXPECT_FALSE(result["already_in_mempool"].asBool());EXPECT_EQ(result["durable_state"].asString(),"signed");
        EXPECT_EQ(f.broadcasts,1u);EXPECT_TRUE(f.ready_at_broadcast);EXPECT_TRUE(f.autocommit_at_broadcast);EXPECT_TRUE(f.retired_at_broadcast);
        const auto ready=f.Snapshot();const auto bytes=f.Account(3).account.Operations().Entries().at(f.Operation(1)).transaction;
        EXPECT_EQ(bytes,f.broadcast_body);const auto txid=uint256::FromHexUnsafe(result["txid"].asString());
        const auto again=f.Finish(params);ASSERT_FALSE(again.isMember("error"));EXPECT_FALSE(again["admitted"].asBool());EXPECT_TRUE(again["already_in_mempool"].asBool());EXPECT_EQ(again["txid"],result["txid"]);EXPECT_EQ(f.broadcasts,1u);EXPECT_EQ(f.Snapshot(),ready);
        f.MineEmpty();ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
        EXPECT_EQ(f.f.ingress->mempool().size(),0u);EXPECT_EQ(f.Account(3).account.Scan().BalanceUna(),withdraw?0u:200000u);EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),withdraw?0u:200000u);
        if(withdraw){const auto coin=f.f.db.getCoin(txid,0);ASSERT_TRUE(coin.ok());EXPECT_EQ(coin->amount,400000u);EXPECT_EQ(coin->script_pubkey,util::hex(f.f.script));}
        const auto confirmed=f.Snapshot();SpendRpcRefused(f.Finish(params));EXPECT_EQ(f.Snapshot(),confirmed);EXPECT_EQ(f.broadcasts,1u);
    }
}
TEST(OrchardFinishRpc, UnknownChangedAndIncompleteCatalogRequestsRefuseWithoutEffects){
    OrchardFinishRpcFixture f;const auto params=f.RequestJson();const auto initial=f.Snapshot();
    SpendRpcRefused(f.Finish(params));EXPECT_EQ(f.Snapshot(),initial);f.ExpectUnsubmitted();
    ASSERT_FALSE(f.CallSpend(params).isMember("error"));f.CompleteProof();const auto before=f.Snapshot();
    auto changed=params;changed["fee_una"]=Json::UInt64(99999);SpendRpcRefused(f.Finish(changed));
    changed=params;changed["payments"][0]["memo_hex"]="01";SpendRpcRefused(f.Finish(changed));
    changed=params;changed["account"]=Json::UInt64(17);SpendRpcRefused(f.Finish(changed));
    changed=params;changed["request_id"]="02"+std::string(62,'0');SpendRpcRefused(f.Finish(changed));
    EXPECT_EQ(f.Snapshot(),before);f.ExpectUnsubmitted();
    f.Sql("CREATE TEMP TABLE finish_saved_owner AS SELECT * FROM orchard_wallet_snapshots WHERE account=17; DELETE FROM orchard_wallet_snapshots WHERE account=17");
    const auto missing=f.Snapshot();SpendRpcRefused(f.Finish(params));EXPECT_EQ(f.Snapshot(),missing);f.ExpectUnsubmitted();
    f.Sql("INSERT INTO orchard_wallet_snapshots SELECT * FROM finish_saved_owner; DROP TABLE finish_saved_owner");EXPECT_EQ(f.Snapshot(),before);
    auto* db=f.Database();sqlite3_set_authorizer(db,[](void*,int action,const char* name,const char*,const char*,const char*){return action==SQLITE_READ&&name&&std::string_view(name)=="orchard_wallet_retained"?SQLITE_DENY:SQLITE_OK;},nullptr);
    SpendRpcRefused(f.Finish(params));sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_EQ(f.Snapshot(),before);f.ExpectUnsubmitted();
    bool seen=false;sqlite3_set_authorizer(db,[](void* p,int action,const char* name,const char*,const char*,const char*){if(action==SQLITE_TRANSACTION&&name&&std::string_view(name)=="COMMIT"){*static_cast<bool*>(p)=true;return SQLITE_DENY;}return SQLITE_OK;},&seen);
    SpendRpcRefused(f.Finish(params));sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(seen);EXPECT_EQ(f.Snapshot(),before);f.ExpectUnsubmitted();
    {auto use=WalletService::AcquireWalletUse(f.wallet);EXPECT_EQ(use->OrchardProofs().Query(f.Operation(1)),wallet::OrchardProofJobs::State::Succeeded);}
    const auto result=f.Finish(params);ASSERT_FALSE(result.isMember("error"))<<result["error"].asString();EXPECT_TRUE(result["admitted"].asBool());
}
TEST(OrchardFinishRpc, ReadyWriteAndCommitRefusalRetainProofWithoutSubmission){
    OrchardFinishRpcFixture f;const auto params=f.RequestJson();ASSERT_FALSE(f.CallSpend(params).isMember("error"));f.CompleteProof();const auto before=f.Snapshot();
    f.Sql("CREATE TRIGGER refuse_finish BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'finish refusal'); END");
    SpendRpcRefused(f.Finish(params));f.Sql("DROP TRIGGER refuse_finish");EXPECT_EQ(f.Snapshot(),before);f.ExpectUnsubmitted();
    struct CommitProbe{bool wrote=false,refused=false;} probe;auto* db=f.Database();
    // SQLite update hooks do not observe WITHOUT ROWID tables. Observe the
    // actual snapshot write with a test-only trigger; neither callback runs SQL.
    ASSERT_EQ(sqlite3_create_function_v2(db,"observe_finish_write",0,SQLITE_UTF8,&probe,
        [](sqlite3_context* context,int,sqlite3_value**){
            static_cast<CommitProbe*>(sqlite3_user_data(context))->wrote=true;
            sqlite3_result_null(context);
        },nullptr,nullptr,nullptr),SQLITE_OK);
    f.Sql("CREATE TEMP TRIGGER observe_finish_write AFTER UPDATE ON main.orchard_wallet_snapshots BEGIN SELECT observe_finish_write(); END");
    sqlite3_commit_hook(db,[](void* p){auto& s=*static_cast<CommitProbe*>(p);if(s.wrote){s.refused=true;return 1;}return 0;},&probe);
    SpendRpcRefused(f.Finish(params));sqlite3_commit_hook(db,nullptr,nullptr);
    f.Sql("DROP TRIGGER observe_finish_write");
    EXPECT_EQ(sqlite3_create_function_v2(db,"observe_finish_write",0,SQLITE_UTF8,nullptr,nullptr,nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_TRUE(probe.wrote);EXPECT_TRUE(probe.refused);EXPECT_EQ(f.Snapshot(),before);f.ExpectUnsubmitted();
    {auto use=WalletService::AcquireWalletUse(f.wallet);EXPECT_EQ(use->OrchardProofs().Query(f.Operation(1)),wallet::OrchardProofJobs::State::Succeeded);}
    const auto result=f.Finish(params);ASSERT_FALSE(result.isMember("error"))<<result["error"].asString();EXPECT_TRUE(result["admitted"].asBool());EXPECT_TRUE(f.ready_at_broadcast);
}
TEST(OrchardFinishRpc, RejectedAdmissionRetainsReadyForExactReopenRetry){
    OrchardFinishRpcFixture f;const auto params=f.RequestJson();ASSERT_FALSE(f.CallSpend(params).isMember("error"));f.CompleteProof();
    f.f.ingress->mempool().setMinFeeRate(1000000.0);const auto refused=f.Finish(params);
    ASSERT_FALSE(refused.isMember("error"))<<refused["error"].asString();EXPECT_FALSE(refused["admitted"].asBool());EXPECT_EQ(refused["durable_state"].asString(),"signed");
    EXPECT_EQ(refused["submission_code"].asString(),TxRejectCodeToString(TxRejectCode::INSUFFICIENT_FEE));f.ExpectUnsubmitted();
    const auto before=f.Snapshot();const auto bytes=f.Account(3).account.Operations().Entries().at(f.Operation(1)).transaction;
    {auto use=WalletService::AcquireWalletUse(f.wallet);EXPECT_FALSE(use->OrchardProofs().Query(f.Operation(1)));use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
    ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());f.f.ingress->mempool().setMinFeeRate(0.0);
    const auto accepted=f.Finish(params);ASSERT_FALSE(accepted.isMember("error"))<<accepted["error"].asString();EXPECT_TRUE(accepted["admitted"].asBool());EXPECT_EQ(accepted["txid"],refused["txid"]);EXPECT_EQ(f.broadcast_body,bytes);EXPECT_EQ(f.Snapshot(),before);
    EXPECT_TRUE(f.ready_at_broadcast);EXPECT_TRUE(f.autocommit_at_broadcast);EXPECT_TRUE(f.retired_at_broadcast);
}
TEST(OrchardFinishRpc, SelectedDomainAndMissingOwnedProofRefuseWithoutRegeneration){
    OrchardFinishRpcFixture f;const auto params=f.RequestJson();
    {wallet::OrchardProofJobs idle;const auto request=f.Request(idle);EXPECT_TRUE(request->enqueued);
     const auto before=f.Snapshot();const auto missing=f.Finish(params);
     EXPECT_TRUE(missing.isMember("error"));EXPECT_EQ(missing.size(),4u);
     EXPECT_EQ(missing["error_code"].asString(),"proof_not_ready");
     EXPECT_EQ(missing["proof_state"].asString(),"missing");EXPECT_TRUE(missing["reservation_retained"].asBool());
     EXPECT_EQ(f.Snapshot(),before);f.ExpectUnsubmitted();
     auto use=WalletService::AcquireWalletUse(f.wallet);EXPECT_FALSE(use->OrchardProofs().Query(f.Operation(1)));}
    OrchardFinishRpcFixture complete;const auto valid=complete.RequestJson();ASSERT_FALSE(complete.CallSpend(valid).isMember("error"));complete.CompleteProof();
    auto use=WalletService::AcquireWalletUse(complete.wallet);auto captured=complete.Read(use->OrchardProofs());ASSERT_TRUE(captured.proof);
    const auto envelope=orchard::TransactionEnvelope::Create(0,{}, {},100000,captured.proof->Bytes());const auto before=complete.Snapshot();
    auto domain=complete.Domain();++domain.branch_id;
    EXPECT_THROW(complete.f.service->AuthorizeOrchardWalletTransaction(envelope,domain,102),std::runtime_error);
    EXPECT_THROW(complete.f.service->AuthorizeOrchardWalletTransaction(envelope,complete.Domain(),103),std::runtime_error);
    const auto wrong_fee=orchard::TransactionEnvelope::Create(0,{}, {},99999,captured.proof->Bytes());
    EXPECT_ANY_THROW(complete.f.service->AuthorizeOrchardWalletTransaction(wrong_fee,complete.Domain(),102));
    EXPECT_EQ(complete.Snapshot(),before);complete.ExpectUnsubmitted();
    complete.context.tx_ingress=nullptr;SpendRpcRefused(complete.Finish(valid));EXPECT_EQ(complete.Snapshot(),before);complete.ExpectUnsubmitted();complete.context.tx_ingress=complete.f.ingress.get();
    const auto result=complete.Finish(valid);ASSERT_FALSE(result.isMember("error"))<<result["error"].asString();EXPECT_TRUE(result["admitted"].asBool());
}
#endif
TEST(OrchardFinishById, StrictSelectorsAndBackendPolicy) {
    ExecutionContext ctx;din::Json p;p["account"]=Json::UInt64(3);p["request_id"]=std::string(63,'0')+"1";
    for(const bool shield:{false,true}) {
        const auto call=[&](const din::Json& x){return shield?rpc_context_wallet_orchard_finishshield(ctx,x):rpc_context_wallet_orchard_finishspend(ctx,x);};
        const auto unavailable=call(p);SpendRpcRefused(unavailable);
#ifdef DINERO_TEST_ORCHARD_ORIGIN
        EXPECT_EQ(unavailable["error"].asString(),"Daemon services unavailable");
#else
        EXPECT_EQ(unavailable["error"].asString(),"Orchard wallet backend unavailable");
#endif
        for(int mode=0;mode<6;++mode){auto bad=p;
            if(mode==0)bad["account"]=true;
            if(mode==1)bad["account"]=Json::UInt64(0x80000000ULL);
            if(mode==2)bad["request_id"]=std::string(64,'0');
            if(mode==3)bad["request_id"]="01";
            if(mode==4)bad["fee_una"]=Json::UInt64(1);
            if(mode==5)bad["payments"]=din::arr();
            const auto error=call(bad);SpendRpcRefused(error);EXPECT_NE(error["error"],unavailable["error"]);
        }
        const auto queued=shield?rpc_context_wallet_orchard_queueshield(ctx,p):rpc_context_wallet_orchard_queuespend(ctx,p);
        SpendRpcRefused(queued);EXPECT_NE(queued["error"],unavailable["error"]);
    }
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardOperationStatus, StoredSpendCompletionMethodSurvivesReadyAndReopen) {
    for (const bool withdraw:{false,true}) {
        OrchardFinishRpcFixture f;auto original=f.RequestJson();
        if(withdraw){original["payments"]=din::arr();din::Json output;output["address"]=f.TransparentAddress();output["amount_una"]=Json::UInt64(400000);original["outputs"].append(output);}
        ASSERT_FALSE(f.CallSpend(original).isMember("error"));f.CompleteProof();
        const auto list=[&](){return rpc_context_wallet_orchard_listoperations(f.RequestContext(),OrchardOperationListRequest());};
        const auto before=f.Snapshot();const auto reserved=list();
        ASSERT_FALSE(reserved.isMember("error"))<<reserved.toStyledString();ASSERT_EQ(reserved["operations"].size(),1u);
        const auto& row=reserved["operations"][0];EXPECT_EQ(row["operation_id"],original["request_id"]);
        EXPECT_EQ(row["completion_method"].asString(),"wallet.orchard.finishspend");
        EXPECT_EQ(row["durable_state"].asString(),"reserved");EXPECT_EQ(row.size(),4u);
        EXPECT_EQ(f.Snapshot(),before);f.ExpectUnsubmitted();
        din::Json id;id["account"]=original["account"];id["request_id"]=row["operation_id"];
        const auto finished=f.Finish(id);ASSERT_FALSE(finished.isMember("error"))<<finished.toStyledString();
        const auto ready=f.Snapshot();const auto signed_list=list();ASSERT_FALSE(signed_list.isMember("error"));
        EXPECT_EQ(signed_list["operations"][0]["completion_method"],row["completion_method"]);
        EXPECT_EQ(signed_list["operations"][0]["txid"],finished["txid"]);EXPECT_EQ(signed_list["operations"][0].size(),5u);EXPECT_EQ(f.Snapshot(),ready);
        {auto use=WalletService::AcquireWalletUse(f.wallet);use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
        ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_EQ(list(),signed_list);EXPECT_EQ(f.Snapshot(),ready);
        const auto again=f.Finish(id);ASSERT_FALSE(again.isMember("error"));EXPECT_TRUE(again["already_in_mempool"].asBool());
        EXPECT_EQ(again["txid"],finished["txid"]);EXPECT_EQ(f.broadcasts,1u);EXPECT_EQ(f.Snapshot(),ready);
    }
}
TEST(OrchardFinishById, TransferAndUnshieldUseStoredIntentAndReadyReopen) {
    for(const bool withdraw:{false,true}) {
        OrchardFinishRpcFixture f;auto original=f.RequestJson();
        if(withdraw){original["payments"]=din::arr();din::Json output;output["address"]=f.TransparentAddress();output["amount_una"]=Json::UInt64(400000);original["outputs"].append(output);}
        ASSERT_FALSE(f.CallSpend(original).isMember("error"));f.CompleteProof();
        din::Json id;id["account"]=original["account"];id["request_id"]=original["request_id"];
        RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.finishspend");ASSERT_NE(method,nullptr);
        const auto bound=OrchardBoundParamsForTest(f.RequestContext(),id);
        const auto first=(*method)(f.RequestContext(),bound);ASSERT_FALSE(first.isMember("error"))<<first.toStyledString();
        EXPECT_TRUE(first["admitted"].asBool());EXPECT_EQ(f.broadcasts,1u);EXPECT_TRUE(f.ready_at_broadcast);EXPECT_TRUE(f.autocommit_at_broadcast);EXPECT_TRUE(f.retired_at_broadcast);
        const auto before=f.Snapshot();const auto bytes=f.Account(3).account.Operations().Entries().at(f.Operation(1)).transaction;EXPECT_EQ(bytes,f.broadcast_body);
        {auto use=WalletService::AcquireWalletUse(f.wallet);EXPECT_FALSE(use->OrchardProofs().Query(f.Operation(1)));use->Wallet().open("canonical-recovery");use->Wallet().unlockWallet("canonical-fixture-pass",0);}
        ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
        const auto stale=(*method)(f.RequestContext(),bound);EXPECT_EQ(stale["error_code"].asString(),"wallet_binding_mismatch");EXPECT_EQ(f.Snapshot(),before);
        const auto again=(*method)(f.RequestContext(),OrchardBoundParamsForTest(f.RequestContext(),id));ASSERT_FALSE(again.isMember("error"))<<again.toStyledString();
        EXPECT_TRUE(again["already_in_mempool"].asBool());EXPECT_EQ(again["txid"],first["txid"]);EXPECT_EQ(f.broadcasts,1u);EXPECT_EQ(f.Snapshot(),before);
        EXPECT_EQ(f.Account(3).account.Operations().Entries().at(f.Operation(1)).transaction,bytes);
    }
}
TEST(OrchardFinishById, MissingUnknownReadAndWriteFailuresPreserveOwnership) {
    OrchardFinishRpcFixture missing;wallet::OrchardProofJobs idle;ASSERT_TRUE(missing.Request(idle)->enqueued);
    const auto request=missing.RequestJson();din::Json id;id["account"]=request["account"];id["request_id"]=request["request_id"];
    const auto before=missing.Snapshot();const auto original_entry=missing.Account(3).account.Operations().Entries().at(missing.Operation(1));
    const auto unavailable=missing.Finish(id);
    EXPECT_EQ(unavailable["error_code"].asString(),"proof_not_ready");EXPECT_EQ(unavailable["proof_state"].asString(),"queued");EXPECT_TRUE(unavailable["reservation_retained"].asBool());EXPECT_EQ(missing.Snapshot(),before);missing.ExpectUnsubmitted();
    const auto resumed=missing.Account(3).account.Operations().Entries().at(missing.Operation(1));
    EXPECT_EQ(resumed.message,original_entry.message);EXPECT_EQ(resumed.nullifiers,original_entry.nullifiers);
    EXPECT_EQ(resumed.request_commitment,original_entry.request_commitment);ASSERT_TRUE(resumed.recovery);ASSERT_TRUE(original_entry.recovery);
    EXPECT_TRUE(std::equal(resumed.recovery->Bytes().begin(),resumed.recovery->Bytes().end(),original_entry.recovery->Bytes().begin(),original_entry.recovery->Bytes().end()));
    {auto use=WalletService::AcquireWalletUse(missing.wallet);EXPECT_TRUE(use->OrchardProofs().Query(missing.Operation(1)));}
    auto unknown=id;unknown["request_id"]=std::string(63,'0')+"2";SpendRpcRefused(missing.Finish(unknown));EXPECT_EQ(missing.Snapshot(),before);
    auto wrong=id;wrong["account"]=Json::UInt64(17);SpendRpcRefused(missing.Finish(wrong));EXPECT_EQ(missing.Snapshot(),before);
    SpendRpcRefused(rpc_context_wallet_orchard_finishshield(missing.RequestContext(),id));EXPECT_EQ(missing.Snapshot(),before);
    OrchardFinishRpcFixture ready;const auto original=ready.RequestJson();ASSERT_FALSE(ready.CallSpend(original).isMember("error"));ready.CompleteProof();
    const auto reserved=ready.Snapshot();auto* db=ready.Database();bool denied=false;
    sqlite3_set_authorizer(db,[](void* p,int action,const char* table,const char*,const char*,const char*){
        if(action==SQLITE_READ&&table&&std::string_view(table)=="orchard_wallet_snapshots"){*static_cast<bool*>(p)=true;return SQLITE_DENY;}return SQLITE_OK;},&denied);
    SpendRpcRefused(ready.Finish(id));sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(denied);EXPECT_EQ(ready.Snapshot(),reserved);ready.ExpectUnsubmitted();
    ready.Sql("CREATE TRIGGER refuse_id_finish BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'id finish refusal'); END");
    SpendRpcRefused(ready.Finish(id));ready.Sql("DROP TRIGGER refuse_id_finish");EXPECT_EQ(ready.Snapshot(),reserved);ready.ExpectUnsubmitted();
    {auto use=WalletService::AcquireWalletUse(ready.wallet);EXPECT_EQ(use->OrchardProofs().Query(ready.Operation(1)),wallet::OrchardProofJobs::State::Succeeded);}
    const auto finished=ready.Finish(id);ASSERT_FALSE(finished.isMember("error"))<<finished.toStyledString();EXPECT_TRUE(finished["admitted"].asBool());EXPECT_TRUE(ready.ready_at_broadcast);
}
#endif

} // namespace dinero
