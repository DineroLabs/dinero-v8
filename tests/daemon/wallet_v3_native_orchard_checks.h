#pragma once
// #939 phase 3: Orchard delivery for native wallets on a full-archive v3 node.
// Included after orchard_account_creation_checks.h (OrchardCreationFixture)
// and wallet_v3_recovery_route_checks.h (NativeV3IndexScope, NativeTestCount).

namespace dinero {

namespace {
uint64_t NativeOrchardHead(CanonicalRecoveryFixture& f) {
    const auto view=f.f.service->getRuntimeAccountReplay();
    OrchardAdmissionFixture::Require(view.ok());
    return (*view)->Head().sequence;
}
} // namespace

// A native-complete wallet on v3 creates an Orchard account, and an Orchard
// note sent to it is delivered by the v3 route: the account cursor advances
// only when delivery runs, matches the authenticated source, the note shows
// in the Orchard balance, the transparent receipts stay valid, and it all
// survives a restart. A reorg through the delivered event is rolled back by
// the authenticated disconnect event.
TEST(WalletV3NativeOrchard, NativeWalletReceivesOrchardNoteThroughV3Delivery) {
    OrchardCreationFixture f;   // native create() wallet, tip at activation
    NativeV3IndexScope v3(f,"v3-native-orchard-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    RegisterOrchardAccountRpc();
    const auto* create=g_rpcRegistry.lookup("wallet.orchard.createaccount");
    const auto* orchard_balance=g_rpcRegistry.lookup("wallet.orchard.getbalance");
    const auto* queue_shield=g_rpcRegistry.lookup("wallet.orchard.queueshield");
    const auto* queue_spend=g_rpcRegistry.lookup("wallet.orchard.queuespend");
    ASSERT_NE(create,nullptr);ASSERT_NE(orchard_balance,nullptr);
    ASSERT_NE(queue_shield,nullptr);ASSERT_NE(queue_spend,nullptr);
    auto ctx=f.RequestContext();
    const auto created=(*create)(ctx,OrchardBoundParamsForTest(ctx,OrchardCreationRequest()));
    ASSERT_FALSE(created.isMember("error")) << created.toStyledString();
    EXPECT_EQ(f.Account().account.Delivery().sequence,0u);

    const auto keys=f.AccountKeys();
    const auto [body,bundle]=f.Shield(keys);(void)bundle;
    (void)f.Mine(body);
    // Mining alone never moves the account: only delivery does.
    EXPECT_EQ(f.Account().account.Delivery().sequence,0u);
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    const auto check_delivered=[&](uint64_t expected_una,const char* phase) {
        SCOPED_TRACE(phase);
        const auto view=f.f.service->getRuntimeAccountReplay();
        ASSERT_TRUE(view.ok());
        const auto account=f.Account();
        EXPECT_EQ(account.account.Scan().BalanceUna(),expected_una);
        EXPECT_EQ(account.account.Delivery().sequence,(*view)->Head().sequence);
        EXPECT_EQ(account.account.Delivery().digest,(*view)->Head().digest);
        EXPECT_EQ(account.account.Scan().Checkpoint(),(*view)->Point((*view)->Head()).checkpoint);
        din::Json request;request["account"]=3;
        const auto shown=(*orchard_balance)(ctx,OrchardBoundParamsForTest(ctx,request));
        ASSERT_FALSE(shown.isMember("error")) << shown.toStyledString();
        EXPECT_EQ(shown["confirmed_una"].asUInt64(),expected_una);
        EXPECT_TRUE(shown["account_caught_up_to_captured_source"].asBool()) << shown.toStyledString();
        // The Orchard route wrote neither receipt: transparent reads stay certified.
        const auto transparent=rpc_context_wallet_getbalance(ctx,din::obj());
        EXPECT_FALSE(transparent.isMember("error")) << transparent.toStyledString();
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
            WalletManager::NativeV3State::Complete);
        EXPECT_FALSE(HasLegacyStateColumn(use->Wallet()));
    };
    check_delivered(500000,"delivered");
    // Orchard spends are not served on v3 yet: refused before any effect.
    for(const auto* method:{queue_shield,queue_spend}) {
        din::Json request;request["account"]=3;
        const auto refused=(*method)(ctx,OrchardBoundParamsForTest(ctx,request));
        EXPECT_EQ(refused["error_code"].asString(),"wallet_native_orchard_spend_unavailable_on_v3")
            << refused.toStyledString();
    }

    f.wallet->Stop();
    f.context.wallet.reset();
    f.wallet.reset();
    f.wallet=std::make_shared<WalletService>();
    f.context.wallet=f.wallet;
    auto reopened=std::make_unique<UTXOIndex>(v3.path.string());
    ASSERT_TRUE(reopened->InitializeFullArchiveV3MetadataOnly());
    auto closed=ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(*f.f.service,std::move(reopened));
    ASSERT_TRUE(closed);closed.reset();
    ASSERT_TRUE(f.wallet->Init(f.context));
    ASSERT_TRUE(f.wallet->Start());
    ASSERT_TRUE(WalletService::AcquireWalletUse(f.wallet)->Wallet().isWalletLocked());
    // A new block while locked: transparent delivery still applies, but the
    // Orchard account (which needs the seed) waits for the unlock.
    f.MineEmpty();
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    WalletService::AcquireWalletUse(f.wallet)->Wallet().unlockWallet("canonical-fixture-pass",0);
    ctx=f.RequestContext();
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),500000u);
    EXPECT_LT(f.Account().account.Delivery().sequence,NativeOrchardHead(f));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    check_delivered(500000,"restarted");
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    check_delivered(500000,"restarted and redelivered");

    // Reorg through the delivered note: disconnect the empty block and the
    // block that carried the shield.
    const auto before_reorg=NativeOrchardHead(f);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    EXPECT_EQ(f.Account().account.Scan().BalanceUna(),500000u) << "no movement before delivery";
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    EXPECT_GT(NativeOrchardHead(f),before_reorg);
    check_delivered(0,"reorged");
    f.wallet->Stop();
}

} // namespace dinero
