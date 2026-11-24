#pragma once
// v8.2.4 (#939 follow-up): Orchard spends for native wallets on a full-archive
// v3 node. Included after orchard_service_proofs_checks.h (ServiceProofTerminal)
// and wallet_v3_recovery_route_checks.h (NativeV3IndexScope). Orchard-origin only.

namespace dinero {

// A native-complete wallet on v3 spends a delivered Orchard note through the
// real RPC route: queue -> proof -> finish (authorize at the v3 source, commit
// Ready, admit to the mempool) -> mine -> v3 delivery. The account's balance
// drops by exactly the unshielded amount plus fee, the transparent receipts
// stay certified, and the native class is untouched.
TEST(WalletV3NativeOrchardSpend, NativeWalletSpendsDeliveredNoteOnV3) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-orchard-spend-index");
    VerifyCatalogReuse verify_reuse;
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    f.context.tx_ingress=f.f.ingress.get();
    auto ctx=f.RequestContext();
    const auto created=rpc_context_wallet_orchard_createaccount(ctx,OrchardCreationRequest());
    ASSERT_FALSE(created.isMember("error")) << created.toStyledString();
    const auto [body,bundle]=f.Shield(f.AccountKeys());(void)bundle;
    (void)f.Mine(body);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ASSERT_EQ(f.Account().account.Scan().BalanceUna(),500000u);

    // Unshield 100000 to an external transparent script, fee 10000.
    const std::vector<uint8_t> program(32,0x5a);
    din::Json request,output;
    request["account"]=Json::UInt64(3);
    request["expected_revision"]=Json::UInt64(f.Account().revision);
    std::vector<uint8_t> id(32);id[0]=0x91;
    request["request_id"]=util::hex(id);
    request["fee_una"]=Json::UInt64(10000);
    request["payments"]=din::arr();request["outputs"]=din::arr();
    output["address"]=bech32::Encode("rdin",1,program,bech32::Encoding::BECH32M);
    output["amount_una"]=Json::UInt64(100000);
    request["outputs"].append(output);
    const auto queued=rpc_context_wallet_orchard_queuespend(ctx,request);
    ASSERT_FALSE(queued.isMember("error")) << queued.toStyledString();
    EXPECT_EQ(queued["durable_state"].asString(),"reserved");
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        orchard::Hash op{};std::copy(id.begin(),id.end(),op.begin());
        ASSERT_EQ(ServiceProofTerminal(use->OrchardProofs(),op),
            wallet::OrchardProofJobs::State::Succeeded);
    }
    din::Json finish;finish["account"]=Json::UInt64(3);finish["request_id"]=request["request_id"];
    const auto finished=rpc_context_wallet_orchard_finishspend(ctx,finish);
    ASSERT_FALSE(finished.isMember("error")) << finished.toStyledString();
    EXPECT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();
    const auto txid=uint256::FromHexUnsafe(finished["txid"].asString());
    const auto pooled=f.f.ingress->mempool().getMempoolEntry(txid);
    ASSERT_TRUE(bool(pooled));
    ASSERT_TRUE(f.Mine(pooled->tx));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),390000u);
    const auto transparent=rpc_context_wallet_getbalance(ctx,din::obj());
    EXPECT_FALSE(transparent.isMember("error")) << transparent.toStyledString();
    auto use=WalletService::AcquireWalletUse(f.wallet);
    EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
        WalletManager::NativeV3State::Complete);
    EXPECT_FALSE(HasLegacyStateColumn(use->Wallet()));
    f.context.tx_ingress=nullptr;
}

// Round trip on v3: unshield to the wallet's own pooled Taproot address, see
// the coin through the transparent v3 receipt, then shield it back into the
// Orchard account. Selection reads only receipt-certified coins; the shield's
// transparent change must not invalidate or escape the sealed receipt domain.
TEST(WalletV3NativeOrchardSpend, NativeWalletRoundTripsUnshieldAndShieldOnV3) {
    OrchardCreationFixture f;
    NativeV3IndexScope v3(f,"v3-native-shield-index");
    VerifyCatalogReuse verify_reuse;
    std::string error;
    const auto deliver=[&]{
        ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
            WalletService::CanonicalRecovery::AppliedPrefix) << error;
    };
    deliver();
    f.context.tx_ingress=f.f.ingress.get();
    auto ctx=f.RequestContext();
    const auto created=rpc_context_wallet_orchard_createaccount(ctx,OrchardCreationRequest());
    ASSERT_FALSE(created.isMember("error")) << created.toStyledString();
    const auto [body,bundle]=f.Shield(f.AccountKeys());(void)bundle;
    (void)f.Mine(body);
    deliver();
    const auto own=rpc_context_wallet_getnewaddress(ctx,din::obj());
    ASSERT_FALSE(own.isMember("error")) << own.toStyledString();
    const auto own_address=own.isMember("address")?own["address"].asString():own.asString();
    const auto run=[&](bool shield,uint8_t tag,const din::Json& recipient){
        din::Json request;
        request["account"]=Json::UInt64(3);
        request["expected_revision"]=Json::UInt64(f.Account().revision);
        std::vector<uint8_t> id(32);id[0]=tag;request["request_id"]=util::hex(id);
        request["fee_una"]=Json::UInt64(10000);request["payments"]=din::arr();
        if(shield)request["payments"].append(recipient);
        else{request["outputs"]=din::arr();request["outputs"].append(recipient);}
        const auto queued=shield?rpc_context_wallet_orchard_queueshield(ctx,request)
                                :rpc_context_wallet_orchard_queuespend(ctx,request);
        EXPECT_FALSE(queued.isMember("error")) << queued.toStyledString();
        if(queued.isMember("error"))return false;
        {
            auto use=WalletService::AcquireWalletUse(f.wallet);
            orchard::Hash op{};std::copy(id.begin(),id.end(),op.begin());
            EXPECT_EQ(ServiceProofTerminal(use->OrchardProofs(),op),wallet::OrchardProofJobs::State::Succeeded);
        }
        din::Json finish;finish["account"]=Json::UInt64(3);finish["request_id"]=request["request_id"];
        const auto finished=shield?rpc_context_wallet_orchard_finishshield(ctx,shield?request:finish)
                                  :rpc_context_wallet_orchard_finishspend(ctx,finish);
        EXPECT_FALSE(finished.isMember("error")) << finished.toStyledString();
        EXPECT_TRUE(finished["admitted"].asBool()) << finished.toStyledString();
        if(!finished["admitted"].asBool())return false;
        const auto pooled=f.f.ingress->mempool().getMempoolEntry(uint256::FromHexUnsafe(finished["txid"].asString()));
        EXPECT_TRUE(bool(pooled));if(!pooled)return false;
        EXPECT_TRUE(f.Mine(pooled->tx));
        return true;
    };
    din::Json out;out["address"]=own_address;out["amount_una"]=Json::UInt64(100000);
    ASSERT_TRUE(run(false,0x93,out));
    deliver();
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),390000u);
    const auto funded=rpc_context_wallet_getbalance(ctx,din::obj());
    ASSERT_FALSE(funded.isMember("error")) << funded.toStyledString();
    EXPECT_NEAR(funded["spendable"].asDouble()*1e8,100000.0,0.5) << funded.toStyledString();
    const auto address_rows=[&]{auto use=WalletService::AcquireWalletUse(f.wallet);
        return NativeTestCount(use->Wallet(),"SELECT count(*) FROM addresses");};
    const auto rows_before=address_rows();
    din::Json pay;pay["address"]=created["address"];pay["amount_una"]=Json::UInt64(20000);
    ASSERT_TRUE(run(true,0x94,pay));
    // The shield's 70000 change went to a pooled change row: nothing derived.
    EXPECT_EQ(address_rows(),rows_before);
    deliver();
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),410000u);
    const auto after=rpc_context_wallet_getbalance(ctx,din::obj());
    ASSERT_FALSE(after.isMember("error")) << after.toStyledString();
    EXPECT_NEAR(after["spendable"].asDouble()*1e8,70000.0,0.5) << after.toStyledString();
    // A reorg un-confirms the wallet's own confirmed shield (the owner's Hotel
    // wallet holds exactly such a shield): delivery rewinds through it, and
    // every reuse of a remembered catalog is also restored and compared
    // (VerifyCatalogReuse above).
    // An idle pass first, so the reorg meets an armed idle skip: the moved
    // head must bring the complete pass (and its reconcile) back.
    const auto skipped=RuntimeWalletRecovery::IdleCatalogPassesSkipped();
    deliver();
    EXPECT_EQ(RuntimeWalletRecovery::IdleCatalogPassesSkipped(),skipped+1) << "idle pass was not skipped";
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    deliver();
    EXPECT_EQ(RuntimeWalletRecovery::IdleCatalogPassesSkipped(),skipped+1) << "reorg pass was skipped";
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),390000u) << "un-confirmed shield still counted";
    EXPECT_EQ(f.Account().account.Delivery().sequence,NativeOrchardHead(f));
    auto use=WalletService::AcquireWalletUse(f.wallet);
    EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
        WalletManager::NativeV3State::Complete);
    f.context.tx_ingress=nullptr;
}

} // namespace dinero
