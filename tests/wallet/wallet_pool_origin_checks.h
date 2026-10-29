// Patched owner and bound-handler paths using synthetic funded coins.
// Allocation references come from this fixture, not a production pool owner.
namespace wallet_pool_origin_checks {
std::array<uint8_t,32> Origin(uint16_t value) {
    std::array<uint8_t,32> result{};
    result[30]=static_cast<uint8_t>(value>>8);
    result[31]=static_cast<uint8_t>(value);
    return result;
}
din::Json Origins(std::initializer_list<uint16_t> values) {
    din::Json result(Json::arrayValue);
    for(const auto value:values) {
        const auto origin=Origin(value);
        result.append(util::hex(std::vector<uint8_t>(origin.begin(),origin.end())));
    }
    return result;
}
}
class WalletPoolOriginsOwner : public WalletRequestOwner {};
TEST_F(WalletPoolOriginsOwner, ExactBatchReferencesAndReopen) {
    using wallet_pool_origin_checks::Origin;
    auto& wallet=service->get();const auto vault=request();
    ASSERT_TRUE(retain(shifted(0),vault).success);const auto prior=find(vault);ASSERT_TRUE(prior);
    EXPECT_EQ(format(),"DNPP03");
    auto pool=vault;pool.request->domain=dinero::PendingPaymentRequestDomain::PoolPayout;
    pool.request->pool_origins={Origin(1),Origin(7)};
    const auto input=shifted(8);const auto staged=retain(input,pool);
    ASSERT_TRUE(staged.success)<<staged.error;verify(staged.signed_tx.tx,input.selected_utxos);
    const auto payment=find(pool);ASSERT_TRUE(payment);EXPECT_EQ(payment->intent,pool);
    EXPECT_EQ(format(),"DNPP04");
    auto disjoint=pool;disjoint.request->id[1]=1;disjoint.request->pool_origins={Origin(2),Origin(9)};
    ASSERT_TRUE(retain(shifted(16),disjoint).success);const auto other=find(disjoint);ASSERT_TRUE(other);
    const auto saved=envelope(wallet.getCurrentDatabase());
    EXPECT_EQ(find(vault)->signed_body,prior->signed_body);EXPECT_EQ(find(pool)->signed_body,payment->signed_body);
    EXPECT_NE(other->txid,payment->txid);EXPECT_EQ(wallet.getPendingPayments().size(),3u);
    wallet.open("owner");wallet.unlockWallet("historical-rpc",0);
    EXPECT_EQ(find(pool)->intent,pool);EXPECT_EQ(find(pool)->signed_body,payment->signed_body);
    EXPECT_EQ(find(disjoint)->signed_body,other->signed_body);EXPECT_EQ(find(vault)->signed_body,prior->signed_body);
    EXPECT_EQ(envelope(wallet.getCurrentDatabase()),saved);EXPECT_EQ(wallet.getLockedUTXOs().size(),6u);
    const auto listing=rpc_context_wallet_listpendingpayments(ctx,din::Json());ASSERT_FALSE(listing.isMember("error"));
    ASSERT_EQ(listing["payments"].size(),3u);EXPECT_FALSE(listing["payments"][0]["request"].isMember("pool_origins"));
    EXPECT_EQ(listing["payments"][1]["request"]["pool_origins"],wallet_pool_origin_checks::Origins({1,7}));
    EXPECT_EQ(listing["payments"][2]["request"]["pool_origins"],wallet_pool_origin_checks::Origins({2,9}));
    EXPECT_EQ(envelope(wallet.getCurrentDatabase()),saved);
}
TEST_F(WalletPoolOriginsOwner, RegroupedOrMalformedReferencesRefuse) {
    using wallet_pool_origin_checks::Origin;
    auto& wallet=service->get();auto pool=request();pool.request->domain=dinero::PendingPaymentRequestDomain::PoolPayout;
    pool.request->pool_origins={Origin(1),Origin(7)};
    ASSERT_TRUE(retain(shifted(0),pool).success);const auto original=find(pool);ASSERT_TRUE(original);
    const auto unused=shifted(8);const auto saved=envelope(wallet.getCurrentDatabase());
    const auto changes=sqlite3_total_changes(wallet.getCurrentDatabase());
    for(int mode=0;mode<11;++mode) {
        auto invalid=pool;
        if(mode==0)invalid.request->id[1]=1;
        if(mode==1)invalid.request->owner[1]=1;
        if(mode==2){invalid.request->id[1]=1;invalid.request->pool_origins={Origin(7)};}
        if(mode==3){invalid.request->id[1]=1;invalid.request->pool_origins={Origin(1),Origin(7),Origin(8)};}
        if(mode==4)invalid.request->pool_origins={Origin(1),Origin(8)};
        if(mode==5)invalid.request->pool_origins={Origin(1),Origin(1)};
        if(mode==6)invalid.request->pool_origins={Origin(7),Origin(1)};
        if(mode==7)invalid.request->pool_origins={Origin(0),Origin(7)};
        if(mode==8){invalid.request->pool_origins.clear();for(uint16_t i=1;i<=257;++i)invalid.request->pool_origins.push_back(Origin(i));}
        if(mode==9)invalid.request->domain=dinero::PendingPaymentRequestDomain::VaultWithdrawal;
        if(mode==10)invalid.request->pool_origins.clear();
        EXPECT_THROW(find(invalid),std::runtime_error)<<mode;
        SigningHook hook{wallet};sqlite3_trace_v2(wallet.getCurrentDatabase(),SQLITE_TRACE_STMT,SigningHook::trace,&hook);
        const auto refused=retain(unused,invalid);sqlite3_trace_v2(wallet.getCurrentDatabase(),0,nullptr,nullptr);
        EXPECT_FALSE(refused.success)<<mode;EXPECT_TRUE(refused.signed_tx.tx.vin.empty());EXPECT_FALSE(hook.fired);
        EXPECT_EQ(envelope(wallet.getCurrentDatabase()),saved);EXPECT_EQ(sqlite3_total_changes(wallet.getCurrentDatabase()),changes);
        EXPECT_EQ(count(wallet.getCurrentDatabase(),"transactions"),1);EXPECT_EQ(wallet.getLockedUTXOs().size(),2u);
    }
    EXPECT_EQ(find(pool)->signed_body,original->signed_body);
}
class WalletPoolOriginsDispatch : public WalletRequestDispatch {};
TEST_F(WalletPoolOriginsDispatch, UnknownOutcomeAndRegroupRefusal) {
    auto p=requested();p["request"]["domain"]="pool_payout";
    p["request"]["pool_origins"]=wallet_pool_origin_checks::Origins({1,7});preflight();
    ingress->submit=[&](const dinero::Transaction& tx)->dinero::TxAcceptResult {
        at_submission(tx);const auto record=service->get().getPendingPayments().at(0);
        EXPECT_EQ(record.intent.request->pool_origins.size(),2u);
        throw std::runtime_error("fixture unknown submission outcome");
    };
    const auto selected=dinero::CaptureWalletSigningIdentity(service->get(),"owner");
    const auto result=dinero::DispatchBoundWalletRequest(ctx,p,service,selected);
    ASSERT_TRUE(result.isMember("error"));EXPECT_TRUE(result["payment_retained"].asBool());
    EXPECT_EQ(result["submission_status"].asString(),"outcome_unknown");
    EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);
    auto& wallet=service->get();const auto record=wallet.getPendingPayments().at(0);
    const auto saved=envelope(wallet.getCurrentDatabase());
    const auto regrouped=[&] {
        for(int mode=0;mode<3;++mode) {
            auto q=p;q["request"]["id"]=std::string(32,'8');
            if(mode==1)q["request"]["pool_origins"]=wallet_pool_origin_checks::Origins({7});
            if(mode==2)q["request"]["pool_origins"]=wallet_pool_origin_checks::Origins({1,7,9});
            const auto current=dinero::CaptureWalletSigningIdentity(wallet,"owner");
            const auto refused=dinero::DispatchBoundWalletRequest(ctx,q,service,current);
            EXPECT_TRUE(refused.isMember("error"));EXPECT_FALSE(refused.isMember("txid"));
            EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);EXPECT_EQ(envelope(wallet.getCurrentDatabase()),saved);
        }
    };
    regrouped();check_retry(p,record.txid,record.signed_body);
    wallet.open("owner");wallet.unlockWallet("historical-rpc",0);
    regrouped();daemon.chainstate.reset();daemon.tx_ingress=nullptr;check_retry(p,record.txid,record.signed_body);
    EXPECT_EQ(wallet.getPendingPayments().at(0).signed_body,record.signed_body);
}
TEST_F(WalletPoolOriginsDispatch, StrictReferenceJsonHasNoPaymentEffects) {
    auto p=requested();p["request"]["domain"]="pool_payout";
    p["request"]["pool_origins"]=wallet_pool_origin_checks::Origins({1,7});preflight();
    ingress->submit=[](const dinero::Transaction& tx){return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    const auto selected=dinero::CaptureWalletSigningIdentity(service->get(),"owner");
    for(int mode=0;mode<11;++mode) {
        auto q=p;
        if(mode==0)q["request"]["pool_origins"]="not an array";
        if(mode==1)q["request"]["pool_origins"]=din::Json(Json::arrayValue);
        if(mode==2)q["request"]["pool_origins"][0]=1;
        if(mode==3)q["request"]["pool_origins"][0]=std::string(63,'1');
        if(mode==4)q["request"]["pool_origins"][0]=std::string(64,'z');
        if(mode==5)q["request"]["pool_origins"]=wallet_pool_origin_checks::Origins({0,7});
        if(mode==6)q["request"]["pool_origins"]=wallet_pool_origin_checks::Origins({7,1});
        if(mode==7)q["request"]["pool_origins"]=wallet_pool_origin_checks::Origins({1,1});
        if(mode==8)q["request"]["domain"]="vault_withdrawal";
        if(mode==9){q["request"]["pool_origins"]=din::Json(Json::arrayValue);for(int i=0;i<257;++i)q["request"]["pool_origins"].append(p["request"]["pool_origins"][0]);}
        if(mode==10)q["request"]["pool_origins"][0]=std::string("1\0",2)+std::string(62,'1');
        const auto refused=dinero::DispatchBoundWalletRequest(ctx,q,service,selected);
        EXPECT_TRUE(refused.isMember("error"))<<mode;EXPECT_FALSE(refused.isMember("txid"));
        EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
        EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_TRUE(service->get().getLockedUTXOs().empty());
        EXPECT_EQ(count(service->get().getCurrentDatabase(),"transactions"),0);
    }
}
