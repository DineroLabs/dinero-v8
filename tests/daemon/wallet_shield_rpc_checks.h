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
        const auto* handler=g_rpcRegistry.lookup(name);ASSERT_NE(handler,nullptr);const auto result=(*handler)(context,ShieldRpcShape());
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
#endif
} // namespace dinero
