// Patched actual handler paths with synthetic funded wallet coins and an
// explicit ingress implementation. No network broadcast or unsafe controls.
class WalletRequestDispatch : public WalletBatchRpc {
protected:
    din::Json requested() {
        din::Json p,recipients(Json::arrayValue),first,second,binding;
        first["address"]=modern_address;first["amount_una"]=din::Json::UInt64(20000);recipients.append(first);
        second["address"]=service->get().getNewAddress();second["amount_una"]=din::Json::UInt64(30000);recipients.append(second);
        binding["domain"]="vault_withdrawal";binding["owner"]=std::string(64,'4');binding["id"]=std::string(32,'7');
        binding["fee_rate_hint"]=din::Json::UInt64(1);binding["maximum_fee_una"]=din::Json::UInt64(10000);binding["audit_context"]="synthetic vault request";
        p["recipients"]=recipients;p["request"]=binding;return p;
    }
    void preflight() {
        ingress->test=[&](const dinero::Transaction& tx){before_preflight(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    }
    void check_retained_submission(const dinero::Transaction& tx) {
        at_submission(tx);const auto record=service->get().getPendingPayments().at(0);
        ASSERT_TRUE(record.intent.request);EXPECT_EQ(record.intent.request->audit_context,"synthetic vault request");
        EXPECT_EQ(record.intent.request->fee_rate_hint,1u);EXPECT_EQ(record.intent.request->maximum_fee_una,10000u);
        EXPECT_LE(record.fee_una,10000u);
    }
    void check_retry(const din::Json& p,const std::string& txid,const std::vector<uint8_t>& body) {
        auto& w=service->get();const auto saved=envelope(w.getCurrentDatabase());const auto changes=sqlite3_total_changes(w.getCurrentDatabase());
        const auto tests=ingress->tests,submits=ingress->submits;
        const auto retry=rpc_context_wallet_sendmany(ctx,p);
        ASSERT_FALSE(retry.isMember("error"))<<retry.toStyledString();EXPECT_EQ(retry["txid"].asString(),txid);
        EXPECT_EQ(retry["hex"].asString(),util::hex(body));EXPECT_TRUE(retry["payment_retained"].asBool());
        EXPECT_EQ(retry["status"].asString(),"retained_request");EXPECT_FALSE(retry["submitted_this_call"].asBool());
        EXPECT_EQ(retry["submission_status"].asString(),"not_attempted");EXPECT_FALSE(retry.isMember("accepted"));
        EXPECT_EQ(retry["total_amount_una"].asUInt64(),50000u);EXPECT_EQ(ingress->tests,tests);EXPECT_EQ(ingress->submits,submits);
        EXPECT_EQ(envelope(w.getCurrentDatabase()),saved);EXPECT_EQ(sqlite3_total_changes(w.getCurrentDatabase()),changes);
        EXPECT_EQ(w.getPendingPayments().size(),1u);EXPECT_TRUE(w.isUTXOLocked(hd.GetTxIdHex(),hd.vout));
    }
};
TEST_F(WalletRequestDispatch, RetainOnceAndResolveWithoutChainOrIngress) {
    const auto p=requested();preflight();
    ingress->submit=[&](const dinero::Transaction& tx){check_retained_submission(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    const auto result=rpc_context_wallet_sendmany(ctx,p);ASSERT_FALSE(result.isMember("error"))<<result.toStyledString();
    EXPECT_TRUE(result["payment_retained"].asBool());EXPECT_TRUE(result["submitted_this_call"].asBool());
    EXPECT_EQ(result["submission_status"].asString(),"accepted");EXPECT_EQ(result["total_amount_una"].asUInt64(),50000u);
    EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);
    auto& w=service->get();const auto record=w.getPendingPayments().at(0);EXPECT_EQ(record.txid,result["txid"].asString());
    daemon.chainstate.reset();daemon.tx_ingress=nullptr;check_retry(p,record.txid,record.signed_body);
    w.open("owner");w.unlockWallet("historical-rpc",0);check_retry(p,record.txid,record.signed_body);
}
TEST_F(WalletRequestDispatch, RejectedSubmissionRetryRetainsSameBody) {
    const auto p=requested();preflight();
    ingress->submit=[&](const dinero::Transaction& tx){check_retained_submission(tx);return dinero::TxAcceptResult::Rejected(dinero::TxRejectCode::INSUFFICIENT_FEE,"fixture changed admission policy");};
    const auto result=rpc_context_wallet_sendmany(ctx,p);ASSERT_TRUE(result.isMember("error"));
    EXPECT_TRUE(result["payment_retained"].asBool());EXPECT_EQ(result["submission_status"].asString(),"rejected");
    EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);const auto record=service->get().getPendingPayments().at(0);
    check_retry(p,record.txid,record.signed_body);
}
TEST_F(WalletRequestDispatch, UnknownSubmissionRetryAndChangedPayloadRefuse) {
    const auto p=requested();preflight();
    ingress->submit=[&](const dinero::Transaction& tx)->dinero::TxAcceptResult{check_retained_submission(tx);throw std::runtime_error("fixture outcome unavailable");};
    const auto result=rpc_context_wallet_sendmany(ctx,p);ASSERT_TRUE(result.isMember("error"));EXPECT_TRUE(result["payment_retained"].asBool());
    EXPECT_TRUE(result["submitted_this_call"].asBool());EXPECT_EQ(result["submission_status"].asString(),"outcome_unknown");
    auto& w=service->get();const auto record=w.getPendingPayments().at(0);const auto saved=envelope(w.getCurrentDatabase());
    for(int mode=0;mode<6;++mode) {
        auto changed=p;
        if(mode==0)changed["recipients"][0]["amount_una"]=din::Json::UInt64(20001);
        if(mode==1)changed["recipients"][0]["address"]=p["recipients"][1]["address"];
        if(mode==2)changed["request"]["fee_rate_hint"]=din::Json::UInt64(2);
        if(mode==3)changed["request"]["maximum_fee_una"]=din::Json::UInt64(9999);
        if(mode==4)changed["request"]["audit_context"]="changed";
        if(mode==5){changed["recipients"][0]=p["recipients"][1];changed["recipients"][1]=p["recipients"][0];}
        const auto refused=rpc_context_wallet_sendmany(ctx,changed);EXPECT_TRUE(refused.isMember("error"));EXPECT_FALSE(refused.isMember("txid"));
        EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);EXPECT_EQ(envelope(w.getCurrentDatabase()),saved);
    }
    check_retry(p,record.txid,record.signed_body);
}
TEST_F(WalletRequestDispatch, MalformedRequestAndFeeLimitHaveNoPaymentEffects) {
    const auto p=requested();preflight();
    ingress->submit=[](const dinero::Transaction& tx){return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    for(int mode=0;mode<11;++mode) {
        auto malformed=p;
        if(mode==0)malformed["request"]["domain"]="unknown";
        if(mode==1)malformed["request"]["owner"]=std::string(64,'0');
        if(mode==2)malformed["request"]["id"]=std::string(32,'z');
        if(mode==3)malformed["request"]["fee_rate_hint"]="1";
        if(mode==4)malformed["recipients"][0]["amount_una"]=20000.0;
        if(mode==5)malformed["preview"]=true;
        if(mode==6)malformed["request"]["audit_context"]=std::string("a\0b",3);
        if(mode==7)malformed["request"].removeMember("maximum_fee_una");
        if(mode==8)malformed["recipients"][0]["amount_una"]=din::Json::UInt64(0);
        if(mode==9)malformed["request"]["maximum_fee_una"]=din::Json::UInt64(0);
        if(mode==10)malformed["request"]["fee_rate_hint"]=-1;
        const auto refused=rpc_context_wallet_sendmany(ctx,malformed);EXPECT_TRUE(refused.isMember("error"))<<mode;
        EXPECT_FALSE(refused.isMember("txid"));EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
        EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(count(service->get().getCurrentDatabase(),"transactions"),0);
    }
}
TEST_F(WalletRequestDispatch, GuardedPreflightAndSelectedSessionRemainRequired) {
    const auto p=requested();preflight();
    ingress->submit=[&](const dinero::Transaction& tx){check_retained_submission(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    auto& w=service->get();ctx.walletName="other";
    EXPECT_TRUE(rpc_context_wallet_sendmany(ctx,p).isMember("error"));EXPECT_EQ(ingress->tests,0);ctx.walletName="owner";
    w.lockWallet();EXPECT_TRUE(rpc_context_wallet_sendmany(ctx,p).isMember("error"));EXPECT_EQ(ingress->tests,0);w.unlockWallet("historical-rpc",0);
    const auto result=rpc_context_wallet_sendmany(ctx,p);ASSERT_FALSE(result.isMember("error"))<<result.toStyledString();
    const auto record=w.getPendingPayments().at(0);dinero::UnsignedTransaction input;ASSERT_TRUE(dinero::TransactionSerializer::Deserialize(input.tx,util::hex(record.signed_body)));
    input.selected_utxos={hd};input.fee=record.fee_una;const auto selected=dinero::CaptureWalletSigningIdentity(w,"owner");
    SigningHook hook{w};sqlite3_trace_v2(w.getCurrentDatabase(),SQLITE_TRACE_STMT,SigningHook::trace,&hook);
    const auto duplicate=dinero::SignWalletRequestPreview(w,selected,input,record.intent);
    sqlite3_trace_v2(w.getCurrentDatabase(),0,nullptr,nullptr);EXPECT_FALSE(duplicate.success);EXPECT_FALSE(hook.fired);EXPECT_TRUE(duplicate.signed_tx.tx.vin.empty());
    auto missing=record.intent;missing.request.reset();EXPECT_FALSE(dinero::SignWalletRequestPreview(w,selected,input,missing).success);
    w.open("owner");w.unlockWallet("historical-rpc",0);EXPECT_FALSE(dinero::SignWalletRequestPreview(w,selected,input,record.intent).success);
    check_retry(p,record.txid,record.signed_body);
}
