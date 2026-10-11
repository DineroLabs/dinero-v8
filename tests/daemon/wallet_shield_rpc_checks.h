#pragma once
#include "rpc/orchard_account_rpc.h"
namespace dinero {
namespace {
din::Json ShieldRpcShape(){auto request=SpendRpcShape();request.removeMember("outputs");return request;}
}
TEST(WalletShieldRpc, MalformedIntentRefusesBeforeServices){
    ExecutionContext context;
    for(bool finish:{false,true}){
        const auto call=[&](const din::Json& p){return finish?rpc_context_wallet_orchard_finishshield(context,p):rpc_context_wallet_orchard_queueshield(context,p);};
        for(const auto& p:std::vector<din::Json>{din::arr(),din::Json{},din::Json("shield")}){
            const auto result=call(p);SpendRpcRefused(result);EXPECT_NE(result["error"].asString(),"Daemon services unavailable");
            EXPECT_NE(result["error"].asString(),"Orchard wallet backend unavailable");
        }
        auto p=ShieldRpcShape();p["fee_una"]=true;SpendRpcRefused(call(p));
        p=ShieldRpcShape();p["outputs"]=din::arr();SpendRpcRefused(call(p));
        p=ShieldRpcShape();p["inputs"]=din::arr();SpendRpcRefused(call(p));
        p=ShieldRpcShape();p["payments"]=din::arr();SpendRpcRefused(call(p));
        p=ShieldRpcShape();p["payments"][0]["memo_hex"]="0";SpendRpcRefused(call(p));
    }
}
TEST(WalletShieldRpc, RegistryAndBackendPolicyRemainExplicit){
    RegisterOrchardAccountRpc();ExecutionContext context;
    for(const auto name:{"wallet.orchard.queueshield","wallet.orchard.finishshield"}){
        const auto* handler=g_rpcRegistry.lookup(name);ASSERT_NE(handler,nullptr);const auto result=(*handler)(context,OrchardBoundParamsForTest(context,ShieldRpcShape()));
        SpendRpcRefused(result);
#ifdef DINERO_TEST_ORCHARD_ORIGIN
        EXPECT_EQ(result["error"].asString(),"Daemon services unavailable");
#else
        EXPECT_EQ(result["error"].asString(),"Orchard wallet backend unavailable");
#endif
    }
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct ShieldRpcFixture:ShieldReservationFixture {
    unsigned broadcasts=0;bool ready_at_broadcast=false,autocommit_at_broadcast=false;
    std::vector<uint8_t> broadcast_body;
    ShieldRpcFixture(){
        context.tx_ingress=f.ingress.get();
        f.ingress->mempool().setTxBroadcastCallback([this](const uint256& txid){
            ++broadcasts;autocommit_at_broadcast=sqlite3_get_autocommit(wallet->get().getCurrentDatabase());
            const auto current=Account(3);const auto& entry=current.account.Operations().Entries().at(orchard::Hash{81});
            const auto pooled=f.ingress->mempool().getMempoolEntry(txid);Need(bool(pooled));broadcast_body=pooled->tx.Serialize();
            ready_at_broadcast=entry.phase==wallet::OrchardOperationQueue::Phase::Ready&&entry.transaction==broadcast_body;
        });
    }
    ~ShieldRpcFixture(){f.ingress->mempool().setTxBroadcastCallback({});context.tx_ingress=nullptr;}
    auto Request(uint64_t fee=10000){din::Json request,payment;request["account"]=Json::UInt64(3);
        request["expected_revision"]=Json::UInt64(Account(3).revision);std::vector<uint8_t> id(32);id[0]=81;
        request["request_id"]=util::hex(id);request["fee_una"]=Json::UInt64(fee);
        payment["address"]=Payments()[0].recipient.EncodeAddress(orchard::WalletNetwork::Regtest);
        payment["amount_una"]=Json::UInt64(20000);payment["memo_hex"]="0100";
        request["payments"]=din::arr();request["payments"].append(payment);return request;}
    auto QueueRpc(const din::Json& request){return rpc_context_wallet_orchard_queueshield(execution,request);}
    auto FinishRpc(const din::Json& request){return rpc_context_wallet_orchard_finishshield(execution,request);}
    void Complete(){auto use=WalletService::AcquireWalletUse(wallet);
        Need(ServiceProofTerminal(use->OrchardProofs(),orchard::Hash{81})==wallet::OrchardProofJobs::State::Succeeded);}
};
}
TEST(OrchardOperationStatus, StoredShieldCompletionMethodSurvivesReadyAndReopen) {
    ShieldRpcFixture f;const auto original=f.Request();
    ASSERT_FALSE(f.QueueRpc(original).isMember("error"));f.Complete();
    const auto list=[&](){return rpc_context_wallet_orchard_listoperations(f.execution,OrchardOperationListRequest());};
    const auto before=f.Snapshot();const auto reserved=list();
    ASSERT_FALSE(reserved.isMember("error"))<<reserved.toStyledString();ASSERT_EQ(reserved["operations"].size(),1u);
    const auto& row=reserved["operations"][0];EXPECT_EQ(row["operation_id"],original["request_id"]);
    EXPECT_EQ(row["completion_method"].asString(),"wallet.orchard.finishshield");
    EXPECT_EQ(row["durable_state"].asString(),"reserved");EXPECT_EQ(row.size(),4u);EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.broadcasts,0u);
    din::Json id;id["account"]=original["account"];id["request_id"]=row["operation_id"];
    const auto finished=f.FinishRpc(id);ASSERT_FALSE(finished.isMember("error"))<<finished.toStyledString();
    const auto ready=f.Snapshot();const auto signed_list=list();ASSERT_FALSE(signed_list.isMember("error"));
    EXPECT_EQ(signed_list["operations"][0]["completion_method"],row["completion_method"]);
    EXPECT_EQ(signed_list["operations"][0]["txid"],finished["txid"]);EXPECT_EQ(signed_list["operations"][0].size(),5u);EXPECT_EQ(f.Snapshot(),ready);
    f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());EXPECT_EQ(list(),signed_list);EXPECT_EQ(f.Snapshot(),ready);
    const auto again=f.FinishRpc(id);ASSERT_FALSE(again.isMember("error"));EXPECT_TRUE(again["already_in_mempool"].asBool());
    EXPECT_EQ(again["txid"],finished["txid"]);EXPECT_EQ(f.broadcasts,1u);EXPECT_EQ(f.Snapshot(),ready);
}
TEST(WalletShieldRpc, AutomaticSelectionRetryReadyAdmissionMiningAndRecovery){
    ShieldRpcFixture f;const auto request=f.Request();const auto result=f.QueueRpc(request);ASSERT_FALSE(result.isMember("error"))<<result.toStyledString();
    EXPECT_TRUE(result["proof_queued"].asBool());EXPECT_EQ(result["input_count"].asUInt64(),1u);EXPECT_EQ(f.broadcasts,0u);
    const auto account=f.Account(3);const auto& entry=account.account.Operations().Entries().at(orchard::Hash{81});ASSERT_TRUE(entry.shield_request);
    ASSERT_EQ(entry.inputs.size(),1u);ASSERT_EQ(entry.shield_request->outputs.size(),1u);
    EXPECT_EQ(entry.inputs[0].txid_wire,f.Inputs()[0].txid_wire);EXPECT_EQ(entry.shield_request->outputs[0].amount_una,70000u);
    EXPECT_NE(entry.shield_request->outputs[0].script_pub_key,f.script);EXPECT_EQ(entry.shield_request->payments[0].memo[0],1);
    const auto before=f.Snapshot();const auto retry=f.QueueRpc(request);ASSERT_FALSE(retry.isMember("error"))<<retry.toStyledString();
    EXPECT_TRUE(retry["existing_request"].asBool());EXPECT_FALSE(retry["proof_queued"].asBool());EXPECT_EQ(f.Snapshot(),before);
    f.Complete();const auto finished=f.FinishRpc(request);ASSERT_FALSE(finished.isMember("error"))<<finished.toStyledString();
    EXPECT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();EXPECT_EQ(f.broadcasts,1u);EXPECT_TRUE(f.ready_at_broadcast);EXPECT_TRUE(f.autocommit_at_broadcast);
    const auto body=f.broadcast_body;ASSERT_FALSE(body.empty());const auto ready=f.Snapshot();f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    const auto reopened=f.QueueRpc(request);ASSERT_FALSE(reopened.isMember("error"))<<reopened.toStyledString();EXPECT_TRUE(reopened["existing_request"].asBool());
    const auto repeated=f.FinishRpc(request);ASSERT_FALSE(repeated.isMember("error"))<<repeated.toStyledString();
    EXPECT_EQ(repeated["txid"],finished["txid"]);EXPECT_TRUE(repeated["already_in_mempool"].asBool());EXPECT_EQ(f.Snapshot(),ready);
    const auto envelope=orchard::TransactionEnvelope::DecodeExact(body);ASSERT_TRUE(f.Mine(MempoolTransaction::FromOrchard(envelope)));
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);
    EXPECT_EQ(f.Account(17).account.Scan().BalanceUna(),20000u);
    const auto observed_bytes=f.Snapshot();const auto observed=f.QueueRpc(request);
    ASSERT_FALSE(observed.isMember("error"))<<observed.toStyledString();EXPECT_FALSE(observed["archived"].asBool());
    EXPECT_TRUE(observed["existing_request"].asBool());EXPECT_FALSE(observed["proof_queued"].asBool());EXPECT_EQ(f.Snapshot(),observed_bytes);
    f.ArchiveConfirmedShield();
    const auto archived=f.QueueRpc(request);ASSERT_FALSE(archived.isMember("error"))<<archived.toStyledString();EXPECT_TRUE(archived["archived"].asBool());
    const auto after=f.Snapshot();SpendRpcRefused(f.FinishRpc(request));EXPECT_EQ(f.Snapshot(),after);
}
TEST(WalletShieldRpc, LowFeeRefusalRetainsExactReadyAcrossReopenAndRetry){
    ShieldRpcFixture f;const auto request=f.Request(1000);
    const auto queued=f.QueueRpc(request);ASSERT_FALSE(queued.isMember("error"))<<queued.toStyledString();
    f.Complete();const auto finished=f.FinishRpc(request);
    ASSERT_FALSE(finished.isMember("error"))<<finished.toStyledString();
    EXPECT_FALSE(finished["admitted"].asBool());EXPECT_FALSE(finished["already_in_mempool"].asBool());
    EXPECT_EQ(finished["submission_code"].asString(),TxRejectCodeToString(TxRejectCode::INSUFFICIENT_FEE));
    EXPECT_EQ(finished["submission_message"].asString(),"Orchard fee rate below minimum");
    const auto account=f.Account(3);const auto& entry=account.account.Operations().Entries().at(orchard::Hash{81});
    ASSERT_EQ(entry.phase,wallet::OrchardOperationQueue::Phase::Ready);ASSERT_FALSE(entry.transaction.empty());
    ASSERT_TRUE(entry.shield_request);EXPECT_EQ(entry.shield_request->fee_una,1000u);
    const auto body=MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::DecodeExact(entry.transaction));
    ASSERT_GT(body.GetVirtualSize(),0u);
    EXPECT_LT(1000.0/body.GetVirtualSize(),f.f.ingress->mempool().getMinFeeRate());
    EXPECT_EQ(finished["txid"].asString(),body.GetTxid().AsUint256().GetHex());
    EXPECT_EQ(f.broadcasts,0u);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    const auto ready=f.Snapshot();f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    const auto retry=f.FinishRpc(request);ASSERT_FALSE(retry.isMember("error"))<<retry.toStyledString();
    EXPECT_FALSE(retry["admitted"].asBool());EXPECT_EQ(retry["submission_code"],finished["submission_code"]);
    EXPECT_EQ(retry["submission_message"],finished["submission_message"]);EXPECT_EQ(retry["txid"],finished["txid"]);
    EXPECT_EQ(f.Account(3).account.Operations().Entries().at(orchard::Hash{81}).transaction,entry.transaction);
    EXPECT_EQ(f.Snapshot(),ready);EXPECT_EQ(f.broadcasts,0u);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
}
TEST(WalletShieldRpc, SharedReservationsManualLocksAndInsufficientFundsDoNotIssueChange){
    for(int mode:{0,1,2,3}){
        ShieldRpcFixture f;auto request=f.Request();
        if(mode==0){const auto payment=f.Pay();ASSERT_TRUE(payment.success)<<payment.error;}
        if(mode==1)f.ReserveFunding(17);
        if(mode==2)ASSERT_TRUE(f.wallet->get().lockUTXO(f.funding.txid.GetHex(),f.funding.vout));
        if(mode==3)request["payments"][0]["amount_una"]=Json::UInt64(100000);
        const auto before=f.Snapshot();SpendRpcRefused(f.QueueRpc(request));EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.broadcasts,0u);
        EXPECT_TRUE(f.Account(3).account.Operations().Entries().empty());
    }
}
TEST(WalletShieldRpc, CandidateReadAndBindingErrorsLeaveNoPartialSelection){
    ShieldRpcFixture f;const auto request=f.Request();auto* db=f.wallet->get().getCurrentDatabase();const auto before=f.Snapshot();
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*){
        return action==SQLITE_READ&&table&&std::string_view(table)=="utxos"?SQLITE_DENY:SQLITE_OK;},nullptr);
    const auto denied=f.QueueRpc(request);sqlite3_set_authorizer(db,nullptr,nullptr);SpendRpcRefused(denied);EXPECT_EQ(f.Snapshot(),before);
    {auto named=f.execution;named.walletName="different-wallet";SpendRpcRefused(rpc_context_wallet_orchard_queueshield(named,request));}
    EXPECT_EQ(f.Snapshot(),before);const auto queued=f.QueueRpc(request);ASSERT_FALSE(queued.isMember("error"))<<queued.toStyledString();
    const auto committed=f.Snapshot();auto changed=request;changed["payments"][0]["memo_hex"]="0200";
    SpendRpcRefused(f.QueueRpc(changed));SpendRpcRefused(f.FinishRpc(changed));EXPECT_EQ(f.Snapshot(),committed);EXPECT_EQ(f.broadcasts,0u);
}
TEST(WalletShieldRpc, ReadyFailureRetainsOwnedProofAndPreventsSubmission){
    ShieldRpcFixture f;const auto request=f.Request();const auto queued=f.QueueRpc(request);ASSERT_FALSE(queued.isMember("error"))<<queued.toStyledString();
    f.Complete();const auto before=f.Snapshot();
    f.Sql("CREATE TRIGGER refuse_auto_shield_ready BEFORE UPDATE ON orchard_wallet_snapshots BEGIN SELECT RAISE(ABORT,'shield Ready refusal'); END");
    const auto failed=f.FinishRpc(request);f.Sql("DROP TRIGGER refuse_auto_shield_ready");SpendRpcRefused(failed);
    EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.broadcasts,0u);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    {auto use=WalletService::AcquireWalletUse(f.wallet);EXPECT_EQ(use->OrchardProofs().Query(orchard::Hash{81}),wallet::OrchardProofJobs::State::Succeeded);}
    const auto finished=f.FinishRpc(request);ASSERT_FALSE(finished.isMember("error"))<<finished.toStyledString();EXPECT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();
}
TEST(OrchardProofStatus, ShieldReopenRejectsForeignJobThenRestoresOriginalPlan) {
    ShieldRpcFixture f;const auto request=f.Request();
    const auto queued=f.QueueRpc(request);ASSERT_FALSE(queued.isMember("error"))<<queued.toStyledString();
    f.Complete();const auto reserved=f.Snapshot();
    const auto original=f.Account(3).account.Operations().Entries().at(orchard::Hash{81});
    ASSERT_EQ(original.phase,wallet::OrchardOperationQueue::Phase::Reserved);
    ASSERT_TRUE(original.transaction.empty());
    f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
    ASSERT_EQ(f.Snapshot(),reserved);
    RegisterOrchardAccountRpc();const auto* finish=g_rpcRegistry.lookup("wallet.orchard.finishshield");ASSERT_NE(finish,nullptr);
    // A job retained by this same service still belongs to the old session.
    // Its presence must refuse ownership, not disclose it as current work.
    const auto foreign=(*finish)(f.execution,OrchardBoundParamsForTest(f.execution,request));
    SpendRpcRefused(foreign);EXPECT_EQ(f.Snapshot(),reserved);EXPECT_EQ(f.broadcasts,0u);
    // Explicit fixture loss of the old process-local result through the normal
    // executor API. This is not a whole-process restart qualification.
    {auto use=WalletService::AcquireWalletUse(f.wallet);
     auto old_result=use->OrchardProofs().TakeResult(orchard::Hash{81});ASSERT_TRUE(old_result);}
    const auto result=(*finish)(f.execution,OrchardBoundParamsForTest(f.execution,request));
    ASSERT_TRUE(result.isMember("error"));EXPECT_EQ(result.size(),4u);
    EXPECT_EQ(result["error_code"].asString(),"proof_not_ready");
    EXPECT_EQ(result["proof_state"].asString(),"queued");EXPECT_TRUE(result["reservation_retained"].asBool());
    EXPECT_EQ(f.Snapshot(),reserved);EXPECT_EQ(f.broadcasts,0u);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    const auto retained=f.Account(3).account.Operations().Entries().at(orchard::Hash{81});
    EXPECT_EQ(retained.phase,wallet::OrchardOperationQueue::Phase::Reserved);
    EXPECT_EQ(retained.request_commitment,original.request_commitment);EXPECT_TRUE(retained.transaction.empty());
    EXPECT_EQ(retained.message,original.message);EXPECT_EQ(retained.nullifiers,original.nullifiers);
    ASSERT_TRUE(retained.recovery);ASSERT_TRUE(original.recovery);
    EXPECT_TRUE(std::equal(retained.recovery->Bytes().begin(),retained.recovery->Bytes().end(),original.recovery->Bytes().begin(),original.recovery->Bytes().end()));
    // Let the normal executor finish. Do not assume a second call is still queued.
    f.Complete();
    {auto use=WalletService::AcquireWalletUse(f.wallet);const auto proof=use->OrchardProofs().CopyResult(orchard::Hash{81},f.Account(3).account.Operations());
     ASSERT_TRUE(proof);EXPECT_EQ(proof->Authorization().SigningDigest(),original.message);}
    const auto finished=(*finish)(f.execution,OrchardBoundParamsForTest(f.execution,request));
    ASSERT_FALSE(finished.isMember("error"))<<finished.toStyledString();EXPECT_TRUE(finished["admitted"].asBool());
    EXPECT_EQ(f.broadcasts,1u);EXPECT_TRUE(f.ready_at_broadcast);EXPECT_TRUE(f.autocommit_at_broadcast);
    const auto ready=f.Account(3).account.Operations().Entries().at(orchard::Hash{81});
    EXPECT_EQ(ready.request_commitment,original.request_commitment);EXPECT_EQ(ready.message,original.message);
    EXPECT_EQ(ready.transaction,f.broadcast_body);EXPECT_FALSE(ready.transaction.empty());
}
TEST(OrchardProofStatus, IdleQueuedCancelledAndMissingJobsRetainReservation) {
    ShieldReservationFixture f;wallet::OrchardProofJobs jobs;
    const auto queued=f.Queue(jobs);ASSERT_TRUE(queued->enqueued);ASSERT_TRUE(queued->durable);
    const auto reserved=f.Snapshot();const auto id=orchard::Hash{81};
    const auto refuse=[&](std::optional<wallet::OrchardProofJobs::State> expected) {
        bool typed=false;
        try { (void)f.Finish(jobs); }
        catch(const wallet::OrchardProofUnavailable& error) {
            typed=true;EXPECT_EQ(error.ObservedState(),expected);
        }
        EXPECT_TRUE(typed);EXPECT_EQ(f.Snapshot(),reserved);
        const auto account=f.Account(3);const auto& entry=account.account.Operations().Entries().at(id);
        EXPECT_EQ(entry.phase,wallet::OrchardOperationQueue::Phase::Reserved);EXPECT_TRUE(entry.transaction.empty());
        EXPECT_EQ(entry.request_commitment,queued->durable->request_commitment);
        EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    };
    ASSERT_EQ(jobs.Query(id),wallet::OrchardProofJobs::State::Queued);
    refuse(wallet::OrchardProofJobs::State::Queued);
    // The normal executor starts idle. No worker, artificial barrier, timeout,
    // synchronization change or scheduling assumption is used by this case.
    ASSERT_TRUE(jobs.Cancel(id));ASSERT_EQ(jobs.Query(id),wallet::OrchardProofJobs::State::Cancelled);
    refuse(wallet::OrchardProofJobs::State::Cancelled);
    jobs.Forget(id);ASSERT_FALSE(jobs.Query(id));refuse(wallet::OrchardProofJobs::State::Queued);
    EXPECT_EQ(jobs.Query(id),wallet::OrchardProofJobs::State::Queued);
    EXPECT_EQ(f.Snapshot(),reserved);
}
TEST(OrchardShieldFinishById, StoredSelectionAndAdmissionResultSurviveReopen) {
    for(const uint64_t fee:{1000u,10000u}) {
        ShieldRpcFixture f;const auto request=f.Request(fee);ASSERT_FALSE(f.QueueRpc(request).isMember("error"));f.Complete();
        din::Json id;id["account"]=request["account"];id["request_id"]=request["request_id"];
        RegisterOrchardAccountRpc();const auto* method=g_rpcRegistry.lookup("wallet.orchard.finishshield");ASSERT_NE(method,nullptr);
        const auto finished=(*method)(f.execution,OrchardBoundParamsForTest(f.execution,id));ASSERT_FALSE(finished.isMember("error"))<<finished.toStyledString();
        EXPECT_EQ(finished["admitted"].asBool(),fee==10000);EXPECT_EQ(f.broadcasts,fee==10000?1u:0u);
        const auto before=f.Snapshot();const auto account=f.Account(3);const auto bytes=account.account.Operations().Entries().at(orchard::Hash{81}).transaction;ASSERT_FALSE(bytes.empty());
        f.Reopen();ASSERT_TRUE(f.wallet->EnsureRuntimeWalletBindings());
        const auto again=(*method)(f.execution,OrchardBoundParamsForTest(f.execution,id));ASSERT_FALSE(again.isMember("error"))<<again.toStyledString();
        EXPECT_EQ(again["txid"],finished["txid"]);EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.Account(3).account.Operations().Entries().at(orchard::Hash{81}).transaction,bytes);
        if(fee==1000){EXPECT_FALSE(again["admitted"].asBool());EXPECT_EQ(again["submission_code"],finished["submission_code"]);EXPECT_EQ(f.broadcasts,0u);}
        else {EXPECT_TRUE(again["already_in_mempool"].asBool());EXPECT_TRUE(f.ready_at_broadcast);EXPECT_TRUE(f.autocommit_at_broadcast);EXPECT_EQ(f.broadcasts,1u);}
    }
}
TEST(OrchardShieldFinishById, WrongKindUnknownAndArchivedNeverSubmit) {
    ShieldRpcFixture f;const auto request=f.Request();ASSERT_FALSE(f.QueueRpc(request).isMember("error"));f.Complete();
    din::Json id;id["account"]=request["account"];id["request_id"]=request["request_id"];const auto before=f.Snapshot();
    SpendRpcRefused(rpc_context_wallet_orchard_finishspend(f.execution,id));EXPECT_EQ(f.Snapshot(),before);EXPECT_EQ(f.broadcasts,0u);
    auto unknown=id;unknown["request_id"]=std::string(63,'0')+"2";SpendRpcRefused(f.FinishRpc(unknown));EXPECT_EQ(f.Snapshot(),before);
    const auto finished=f.FinishRpc(id);ASSERT_FALSE(finished.isMember("error"))<<finished.toStyledString();ASSERT_TRUE(finished["admitted"].asBool());
    ASSERT_TRUE(f.Mine(MempoolTransaction::FromOrchard(orchard::TransactionEnvelope::DecodeExact(f.broadcast_body))));
    EXPECT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(),Recovery::AppliedPrefix);f.ArchiveConfirmedShield();const auto archived=f.Snapshot();
    SpendRpcRefused(f.FinishRpc(id));EXPECT_EQ(f.Snapshot(),archived);EXPECT_EQ(f.broadcasts,1u);
}

#endif
} // namespace dinero
