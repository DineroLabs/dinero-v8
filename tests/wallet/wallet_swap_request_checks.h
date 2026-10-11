// Serialized functional cases using the actual encrypted pending-payment owner.
// Synthetic funded inputs and ingress only; no node startup or external broadcast.
class WalletSwapRequestOwner : public WalletRequestOwner {};
TEST_F(WalletSwapRequestOwner, SeparateNamespacesSurviveReopen) {
    auto& wallet=service->get();const auto vault=request();auto pool=vault,swap=vault;
    pool.request->domain=dinero::PendingPaymentRequestDomain::PoolPayout;
    swap.request->domain=dinero::PendingPaymentRequestDomain::SwapFunding;
    ASSERT_TRUE(retain(shifted(0),vault).success);
    ASSERT_TRUE(retain(shifted(8),pool).success);
    EXPECT_FALSE(find(swap));ASSERT_TRUE(retain(shifted(16),swap).success);
    const auto a=find(vault),b=find(pool),c=find(swap);ASSERT_TRUE(a);ASSERT_TRUE(b);ASSERT_TRUE(c);
    EXPECT_NE(a->txid,b->txid);EXPECT_NE(a->txid,c->txid);EXPECT_NE(b->txid,c->txid);
    EXPECT_EQ(c->intent,swap);EXPECT_EQ(format(),"DNPP03");
    const auto saved=envelope(wallet.getCurrentDatabase());
    wallet.open("owner");wallet.unlockWallet("historical-rpc",0);
    EXPECT_EQ(find(vault)->signed_body,a->signed_body);EXPECT_EQ(find(pool)->signed_body,b->signed_body);
    EXPECT_EQ(find(swap)->signed_body,c->signed_body);EXPECT_EQ(envelope(wallet.getCurrentDatabase()),saved);
    const auto listing=rpc_context_wallet_listpendingpayments(ctx,din::Json());ASSERT_FALSE(listing.isMember("error"));
    ASSERT_EQ(listing["payments"].size(),3u);EXPECT_EQ(listing["payments"][2]["request"]["domain"].asString(),"swap_funding");
}
TEST_F(WalletSwapRequestOwner, ExactIntentAndSingleRecipientRemainRequired) {
    auto& wallet=service->get();auto swap=request();swap.request->domain=dinero::PendingPaymentRequestDomain::SwapFunding;
    const auto input=shifted(0);
    for(int mode=0;mode<3;++mode) {
        auto invalid=swap;
        if(mode==0)invalid.additional_recipients.push_back({modern_address,1});
        if(mode==1)invalid.request->maximum_fee_una=0;
        if(mode==2){std::array<uint8_t,32> origin{};origin[0]=1;invalid.request->pool_origins.push_back(origin);}
        EXPECT_THROW(find(invalid),std::runtime_error);EXPECT_FALSE(retain(input,invalid).success);
        EXPECT_TRUE(wallet.getPendingPayments().empty());EXPECT_EQ(count(wallet.getCurrentDatabase(),"transactions"),0);
    }
    ASSERT_TRUE(retain(input,swap).success);const auto saved=envelope(wallet.getCurrentDatabase());
    for(int mode=0;mode<4;++mode) {
        auto changed=swap;
        if(mode==0)++changed.amount_una;
        if(mode==1)++changed.request->fee_rate_hint;
        if(mode==2)++changed.request->maximum_fee_una;
        if(mode==3)changed.request->audit_context="changed swap intent";
        EXPECT_THROW(find(changed),std::runtime_error);EXPECT_FALSE(retain(input,changed).success);
        EXPECT_EQ(envelope(wallet.getCurrentDatabase()),saved);
    }
    EXPECT_EQ(wallet.getPendingPayments().size(),1u);
}
class WalletSwapRequestDispatch : public WalletRequestDispatch {};
TEST_F(WalletSwapRequestDispatch, RequiresBoundDispatchBeforePaymentEffects) {
    auto p=requested();p["request"]["domain"]="swap_funding";
    Json::Value recipients(Json::arrayValue);recipients.append(p["recipients"][0]);p["recipients"]=recipients;
    preflight();auto identity=dinero::CaptureWalletSigningIdentity(service->get(),"owner");
    identity.database_id=service->get().AcquireDatabaseLease()->ReadDeliveryIdentity();
    const auto generic=rpc_context_wallet_sendmany(ctx,p);EXPECT_TRUE(generic.isMember("error"));
    EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
    auto stale=identity;++stale.session;
    EXPECT_TRUE(dinero::DispatchBoundWalletRequest(ctx,p,service,stale).isMember("error"));
    EXPECT_TRUE(dinero::DispatchBoundWalletRequest(ctx,p,{},identity).isMember("error"));
    EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
    EXPECT_TRUE(service->get().getPendingPayments().empty());
}
TEST_F(WalletSwapRequestDispatch, UnknownOutcomeResolvesSameBodyAfterReopen) {
    auto p=requested();p["request"]["domain"]="swap_funding";p["request"]["audit_context"]="synthetic swap funding";
    Json::Value recipients(Json::arrayValue);recipients.append(p["recipients"][0]);p["recipients"]=recipients;
    preflight();ingress->submit=[&](const dinero::Transaction& tx)->dinero::TxAcceptResult {
        auto& wallet=service->get();EXPECT_TRUE(sqlite3_get_autocommit(wallet.getCurrentDatabase()));
        const auto payments=wallet.getPendingPayments();EXPECT_EQ(payments.size(),1u);
        if(!payments.empty()) {
            EXPECT_EQ(payments[0].signed_body,tx.Serialize(dinero::TxSerializationMode::WithWitness));
            EXPECT_TRUE(payments[0].intent.additional_recipients.empty());EXPECT_EQ(payments[0].intent.amount_una,20000u);
            EXPECT_EQ(payments[0].intent.request->domain,dinero::PendingPaymentRequestDomain::SwapFunding);
        }
        EXPECT_EQ(count(wallet.getCurrentDatabase(),"transactions"),1);
        EXPECT_TRUE(wallet.isUTXOLocked(hd.GetTxIdHex(),hd.vout));verify(tx,{hd});
        throw std::runtime_error("fixture funding outcome unavailable");
    };
    auto first=dinero::CaptureWalletSigningIdentity(service->get(),"owner");
    first.database_id=service->get().AcquireDatabaseLease()->ReadDeliveryIdentity();
    const auto result=dinero::DispatchBoundWalletRequest(ctx,p,service,first);
    ASSERT_TRUE(result.isMember("error"));EXPECT_TRUE(result["payment_retained"].asBool());
    EXPECT_EQ(result["submission_status"].asString(),"outcome_unknown");ASSERT_EQ(ingress->submits,1);
    auto& wallet=service->get();const auto retained=wallet.getPendingPayments().at(0);
    daemon.chainstate.reset();daemon.tx_ingress=nullptr;
    wallet.open("owner");wallet.unlockWallet("historical-rpc",0);
    const auto saved=envelope(wallet.getCurrentDatabase());const auto changes=sqlite3_total_changes(wallet.getCurrentDatabase());
    EXPECT_TRUE(dinero::DispatchBoundWalletRequest(ctx,p,service,first).isMember("error"));
    EXPECT_TRUE(rpc_context_wallet_sendmany(ctx,p).isMember("error"));
    auto current=dinero::CaptureWalletSigningIdentity(wallet,"owner");
    current.database_id=wallet.AcquireDatabaseLease()->ReadDeliveryIdentity();
    const auto retry=dinero::DispatchBoundWalletRequest(ctx,p,service,current);
    ASSERT_FALSE(retry.isMember("error"))<<retry.toStyledString();EXPECT_EQ(retry["txid"].asString(),retained.txid);
    EXPECT_EQ(retry["hex"].asString(),util::hex(retained.signed_body));EXPECT_TRUE(retry["payment_retained"].asBool());
    EXPECT_FALSE(retry["submitted_this_call"].asBool());EXPECT_EQ(retry["submission_status"].asString(),"not_attempted");
    EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);
    EXPECT_EQ(envelope(wallet.getCurrentDatabase()),saved);EXPECT_EQ(sqlite3_total_changes(wallet.getCurrentDatabase()),changes);
}


TEST_F(WalletSwapRequestOwner, FinalLookupAndSigningRequireRecordedPayer) {
    auto& wallet=service->get();auto swap=request();swap.request->domain=dinero::PendingPaymentRequestDomain::SwapFunding;
    auto selected=identity();selected.database_id=wallet.AcquireDatabaseLease()->ReadDeliveryIdentity();
    auto wrong=selected;(*wrong.database_id)[0]^=0x80;const auto input=shifted(0);
    EXPECT_THROW(dinero::FindRetainedWalletPayment(wallet,wrong,swap),std::runtime_error);
    auto refusal=dinero::SignAndStageWalletPayment(wallet,wrong,input,swap);
    EXPECT_FALSE(refusal.success);EXPECT_NE(refusal.error.find("payer database identity"),std::string::npos);
    EXPECT_TRUE(wallet.getPendingPayments().empty());
    const auto accepted=dinero::SignAndStageWalletPayment(wallet,selected,input,swap);ASSERT_TRUE(accepted.success)<<accepted.error;
    const auto retained=dinero::FindRetainedWalletPayment(wallet,selected,swap);ASSERT_TRUE(retained);
    const auto saved=envelope(wallet.getCurrentDatabase());
    EXPECT_THROW(dinero::FindRetainedWalletPayment(wallet,wrong,swap),std::runtime_error);
    EXPECT_EQ(dinero::FindRetainedWalletPayment(wallet,selected,swap)->signed_body,retained->signed_body);
    EXPECT_EQ(envelope(wallet.getCurrentDatabase()),saved);
}
TEST_F(WalletSwapRequestDispatch, RetainOnlyPreflightsWithoutSubmissionAndResolvesAfterReopen) {
    auto p=requested();p["request"]["domain"]="swap_funding";
    Json::Value recipients(Json::arrayValue);recipients.append(p["recipients"][0]);p["recipients"]=recipients;
    preflight();auto& wallet=service->get();auto selected=dinero::CaptureWalletSigningIdentity(wallet,"owner");
    selected.database_id=wallet.AcquireDatabaseLease()->ReadDeliveryIdentity();
    auto wrong=selected;(*wrong.database_id)[0]^=0x40;
    EXPECT_TRUE(dinero::DispatchBoundWalletRequest(ctx,p,service,wrong,dinero::WalletRequestDispatchMode::RetainOnly).isMember("error"));
    EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);EXPECT_TRUE(wallet.getPendingPayments().empty());
    const auto result=dinero::DispatchBoundWalletRequest(ctx,p,service,selected,dinero::WalletRequestDispatchMode::RetainOnly);
    ASSERT_FALSE(result.isMember("error"))<<result.toStyledString();ASSERT_EQ(wallet.getPendingPayments().size(),1u);
    const auto payment=wallet.getPendingPayments().at(0);
    EXPECT_EQ(result["hex"].asString(),util::hex(payment.signed_body));EXPECT_EQ(result["txid"].asString(),payment.txid);
    EXPECT_FALSE(result["submitted_this_call"].asBool());EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,0);
    daemon.chainstate.reset();daemon.tx_ingress=nullptr;wallet.open("owner");wallet.unlockWallet("historical-rpc",0);
    auto reopened=dinero::CaptureWalletSigningIdentity(wallet,"owner");reopened.database_id=selected.database_id;
    const auto retry=dinero::DispatchBoundWalletRequest(ctx,p,service,reopened,dinero::WalletRequestDispatchMode::RetainOnly);
    ASSERT_FALSE(retry.isMember("error"))<<retry.toStyledString();EXPECT_EQ(retry["hex"].asString(),result["hex"].asString());
    EXPECT_EQ(wallet.getPendingPayments().size(),1u);EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,0);
}


class WalletSwapFundingAdapter : public WalletRequestDispatch {
protected:
    dinero::swap::DinFundingRequest funding_request() {
        dinero::swap::DinFundingRequest r;
        r.binding.wallet_id=service->get().AcquireDatabaseLease()->ReadDeliveryIdentity();
        r.binding.fee_rate_hint=1;r.binding.maximum_fee_una=10000;r.offer_id.fill(0x68);r.amount_una=20000;
        dinero::swap::DinHtlcTerms terms;terms.payment_hash.fill(0x27);terms.refund_locktime_unix=1800200000;
        std::array<uint8_t,32> secret{};secret.back()=13;int parity=0;
        if(!dinero::TaprootKeys::DeriveXOnlyPubkey(secret,terms.claim_pubkey,parity))throw std::runtime_error("fixture claim key");
        secret.back()=17;
        if(!dinero::TaprootKeys::DeriveXOnlyPubkey(secret,terms.refund_pubkey,parity))throw std::runtime_error("fixture refund key");
        const auto output=dinero::swap::BuildDinHtlc(terms);
        r.address=AddressCodec::encodeP2TR(Network::REGTEST,std::vector<uint8_t>(output.output_key.begin(),output.output_key.end()));
        return r;
    }
};
TEST_F(WalletSwapFundingAdapter, PrepareResolveAndSubmitExactRetainedBodyAfterReopen) {
    using dinero::swap::DinFundingOperation;
    auto request=funding_request();auto owner=dinero::MakeRetainedSwapFundingOwner(daemon);preflight();
    const auto prepared=owner(request,DinFundingOperation::Prepare,{});
    auto& wallet=service->get();ASSERT_EQ(wallet.getPendingPayments().size(),1u);
    const auto payment=wallet.getPendingPayments().at(0);ASSERT_TRUE(payment.intent.request);
    EXPECT_EQ(payment.intent.request->owner,request.offer_id);EXPECT_EQ(payment.intent.request->domain,dinero::PendingPaymentRequestDomain::SwapFunding);
    EXPECT_EQ(payment.intent.address,request.address);EXPECT_EQ(payment.intent.amount_una,request.amount_una);
    EXPECT_EQ(prepared.raw,payment.signed_body);EXPECT_EQ(prepared.txid,payment.txid);EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,0);
    daemon.chainstate.reset();wallet.open("owner");wallet.unlockWallet("historical-rpc",0);
    const auto saved=envelope(wallet.getCurrentDatabase());const auto changes=sqlite3_total_changes(wallet.getCurrentDatabase());
    EXPECT_EQ(owner(request,DinFundingOperation::Resolve,{}).raw,prepared.raw);
    EXPECT_EQ(owner(request,DinFundingOperation::Prepare,{}).raw,prepared.raw);
    ingress->submit=[&](const dinero::Transaction& tx)->dinero::TxAcceptResult {
        EXPECT_TRUE(sqlite3_get_autocommit(wallet.getCurrentDatabase()));EXPECT_EQ(tx.Serialize(dinero::TxSerializationMode::WithWitness),prepared.raw);
        verify(tx,{hd});throw std::runtime_error("fixture lost adapter submission reply");
    };
    EXPECT_THROW(owner(request,DinFundingOperation::SubmitRetained,prepared.raw),std::runtime_error);
    ingress->submit=[&](const dinero::Transaction& tx) {
        EXPECT_EQ(tx.Serialize(dinero::TxSerializationMode::WithWitness),prepared.raw);verify(tx,{hd});
        return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());
    };
    EXPECT_EQ(owner(request,DinFundingOperation::SubmitRetained,prepared.raw).txid,prepared.txid);
    EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,2);EXPECT_EQ(wallet.getPendingPayments().size(),1u);
    EXPECT_EQ(envelope(wallet.getCurrentDatabase()),saved);EXPECT_EQ(sqlite3_total_changes(wallet.getCurrentDatabase()),changes);
}
TEST_F(WalletSwapFundingAdapter, RefusesMissingChangedPayerIntentAndBodyBeforeSubmission) {
    using dinero::swap::DinFundingOperation;
    auto request=funding_request();auto owner=dinero::MakeRetainedSwapFundingOwner(daemon);preflight();
    EXPECT_THROW(owner(request,DinFundingOperation::Resolve,{}),std::runtime_error);
    EXPECT_THROW(owner(request,DinFundingOperation::SubmitRetained,{1}),std::runtime_error);
    EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);EXPECT_TRUE(service->get().getPendingPayments().empty());
    auto wrong=request;wrong.binding.wallet_id[0]^=0x80;
    EXPECT_THROW(owner(wrong,DinFundingOperation::Prepare,{}),std::runtime_error);EXPECT_EQ(ingress->tests,0);
    const auto prepared=owner(request,DinFundingOperation::Prepare,{});const auto saved=envelope(service->get().getCurrentDatabase());
    for(int mode=0;mode<5;++mode) {
        auto changed=request;
        if(mode==0)++changed.amount_una;
        if(mode==1)++changed.binding.maximum_fee_una;
        if(mode==2)++changed.binding.fee_rate_hint;
        if(mode==3)changed.offer_id.back()^=1;
        if(mode==4)changed.address=modern_address;
        EXPECT_THROW(owner(changed,DinFundingOperation::Resolve,{}),std::runtime_error);
        EXPECT_THROW(owner(changed,DinFundingOperation::SubmitRetained,prepared.raw),std::runtime_error);
    }
    auto body=prepared.raw;body.back()^=1;
    EXPECT_THROW(owner(request,DinFundingOperation::SubmitRetained,body),std::runtime_error);
    EXPECT_THROW(owner(request,DinFundingOperation::SubmitRetained,{}),std::runtime_error);
    service->get().open("other");
    EXPECT_THROW(owner(request,DinFundingOperation::Resolve,{}),std::runtime_error);
    EXPECT_THROW(owner(request,DinFundingOperation::SubmitRetained,prepared.raw),std::runtime_error);
    service->get().open("owner");service->get().unlockWallet("historical-rpc",0);
    EXPECT_EQ(envelope(service->get().getCurrentDatabase()),saved);EXPECT_EQ(ingress->submits,0);EXPECT_EQ(ingress->tests,1);
}
