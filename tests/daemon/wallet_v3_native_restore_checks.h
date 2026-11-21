#pragma once
// v8.2.4 (#939 follow-up): restoring a wallet from its seed on a full-archive
// v3 node. Included with the native v3 Orchard spend checks (Orchard-origin
// only): uses CanonicalRecoveryFixture, NativeV3IndexScope and NativeTestCount.

#include "wallet/bip39.h"

namespace dinero {

namespace {
constexpr const char* kV3RestoreMnemonic =
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
}

din::Json RestoreOnV3(CanonicalRecoveryFixture& f,const char* name,const char* mnemonic,
                     const char* password="restore-pass"){
    ExecutionContext ctx;ctx.daemon=&f.context;
    din::Json params(Json::arrayValue);
    params.append(name);params.append(mnemonic);params.append("");params.append(password);
    return rpc_context_wallet_restore(ctx,params);
}
void ExpectRestoredOnV3(CanonicalRecoveryFixture& f,const din::Json& restored,const char* name){
    ASSERT_FALSE(restored.isMember("error")) << restored.toStyledString();
    EXPECT_TRUE(restored["success"].asBool()) << restored.toStyledString();
    EXPECT_FALSE(restored.isMember("warning")) << restored.toStyledString();
    EXPECT_EQ(restored["history_recovery"].asString(),"native_v3_after_unlock") << restored.toStyledString();
    auto use=WalletService::AcquireWalletUse(f.wallet);
    ASSERT_EQ(use->Wallet().getCurrentWalletName(),name);
    auto lease=use->Wallet().AcquireDatabaseLease();
    EXPECT_FALSE(lease->SealedFreshBirthHeight(lease->Session()).has_value())
        << "a restored seed must never skip the historical replay";
}

// The seed owns a pre-v3 transparent coin (H101). On a v3 node wallet.restore
// creates the wallet without a legacy rescan; after unlock the native route
// enrolls it with a FULL replay (no fresh birth: a restored seed has history),
// so the historical coin is found and the balance is certified.
TEST(WalletV3NativeRestore, RestoredSeedRecoversTransparentHistoryOnV3) {
    CanonicalRecoveryFixture f(true,true);   // "canonical-recovery" owns the H101 coin
    (void)f.MineToWallet();
    NativeV3IndexScope v3(f,"v3-native-restore-index");
    const auto restored=RestoreOnV3(f,"restored-on-v3",kV3RestoreMnemonic);
    ExpectRestoredOnV3(f,restored,"restored-on-v3");
    WalletService::AcquireWalletUse(f.wallet)->Wallet().unlockWallet("restore-pass",0);
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
            WalletManager::NativeV3State::Complete);
    }
    ExecutionContext ctx;ctx.daemon=&f.context;ctx.walletName="restored-on-v3";
    const auto balance=rpc_context_wallet_getbalance(ctx,din::obj());
    ASSERT_FALSE(balance.isMember("error")) << balance.toStyledString();
    // H101 historical coin (spendable) and the H102 coinbase (immature): both
    // predate the restore and are found only by the full replay.
    EXPECT_NEAR(balance["breakdown"]["spendable"].asDouble(),0.01,1e-9) << balance.toStyledString();
    EXPECT_NEAR(balance["breakdown"]["unspendable"].asDouble(),100.0,1e-9) << balance.toStyledString();
}

// An Orchard note was paid to the seed's account 0 before the node went v3.
// After restoring that seed on v3, Orchard account-0 recovery finds it from
// the authenticated v3 source.
TEST(WalletV3NativeRestore, RestoredSeedRecoversOrchardAccount0OnV3) {
    CanonicalRecoveryFixture f(true);
    std::vector<uint8_t> seed;
    ASSERT_TRUE(bip39::MnemonicToSeed(kV3RestoreMnemonic,"",seed));
    const auto keys=orchard::WalletKeys::FromSeed(seed,0);
    const auto [body,bundle]=f.Shield(keys);(void)bundle;
    (void)f.Mine(body);
    NativeV3IndexScope v3(f,"v3-native-restore-orchard-index");
    // The Qt wizard's order: restore with no password, then encrypt, then unlock.
    const auto restored=RestoreOnV3(f,"restored-orchard-on-v3",kV3RestoreMnemonic,"");
    ExpectRestoredOnV3(f,restored,"restored-orchard-on-v3");
    {
        // Right after the restore, before any worker pass: pending and reported.
        EXPECT_EQ(WalletService::AcquireWalletUse(f.wallet)->Wallet().getSetting(
            WalletManager::kOrchardAccount0RestorePendingSetting),"1");
        ExecutionContext early;early.daemon=&f.context;early.walletName="restored-orchard-on-v3";
        const auto info=rpc_context_wallet_getwalletinfo(early,din::obj());
        ASSERT_TRUE(info.isMember("orchard_account0_recovery")) << info.toStyledString();
        EXPECT_EQ(info["orchard_account0_recovery"]["state"].asString(),"in_progress");
    }
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        use->Wallet().encryptWallet("restore-pass");
        use->Wallet().unlockWallet("restore-pass",0);
    }
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        EXPECT_EQ(use->Wallet().AcquireDatabaseLease()->NativeV3TransparentState(),
            WalletManager::NativeV3State::Complete);
    }
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        // One full replay only: no pool extension is due after the restore
        // (the receive pool was topped up; a restored seed has no PQ master
        // on any route, so no P2MR target opens).
        EXPECT_FALSE(use->Wallet().AcquireDatabaseLease()->NativeV3PoolNeedsExtension());
    }
    ExecutionContext ctx;ctx.daemon=&f.context;ctx.walletName="restored-orchard-on-v3";
    EXPECT_EQ(restored["orchard_account0_recovery"].asString(),"automatic_after_unlock");
    // No manual RPC: the ordinary worker passes finish account-0 recovery.
    const auto pending=[&]{return WalletService::AcquireWalletUse(f.wallet)->Wallet().getSetting(
        WalletManager::kOrchardAccount0RestorePendingSetting)=="1";};
    for(int i=0;i<8&&pending();++i)
        ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
            WalletService::CanonicalRecovery::AppliedPrefix) << error;
    EXPECT_FALSE(pending()) << "account-0 recovery completes through the normal flow";
    EXPECT_FALSE(rpc_context_wallet_getwalletinfo(ctx,din::obj()).isMember("orchard_account0_recovery"));
    din::Json account;account["account"]=Json::UInt64(0);
    const auto shown=rpc_context_wallet_orchard_getbalance(ctx,account);
    ASSERT_FALSE(shown.isMember("error")) << shown.toStyledString();
    EXPECT_EQ(shown["confirmed_una"].asUInt64(),500000u) << shown.toStyledString();
    // The transparent side of the restored wallet stays certified.
    const auto balance=rpc_context_wallet_getbalance(ctx,din::obj());
    EXPECT_FALSE(balance.isMember("error")) << balance.toStyledString();
}

// v8.2.4 (#939): the ordinary desktop restore on a v2 (full-archive) node
// also recovers Orchard account 0 with no manual RPC: wallet.restore marks
// the wallet and the runtime worker advances the recovery until complete.
TEST(OrchardAccount0Recovery, AutomaticAfterV2DesktopRestore) {
    Account0RecoveryFixture f;   // v2 index; "before-recovery" exists from the same seed
    const auto keys=f.AccountKeys(0);
    const auto [body,bundle]=f.Shield(keys);(void)bundle;
    (void)f.Mine(body);f.MineEmpty();
    ExecutionContext ctx;ctx.daemon=&f.context;
    din::Json params(Json::arrayValue);
    params.append("restored-v2");params.append(RecoveryTestPhrase);params.append("");params.append("");
    const auto restored=rpc_context_wallet_restore(ctx,params);
    ASSERT_TRUE(restored["success"].asBool()) << restored.toStyledString();
    EXPECT_EQ(restored["orchard_account0_recovery"].asString(),"automatic_after_unlock") << restored.toStyledString();
    EXPECT_FALSE(restored.isMember("history_recovery")) << "v2 keeps its legacy rescan";
    const auto pending=[&]{return WalletService::AcquireWalletUse(f.wallet)->Wallet().getSetting(
        WalletManager::kOrchardAccount0RestorePendingSetting)=="1";};
    ASSERT_TRUE(pending());
    ASSERT_FALSE(f.Catalog()->KnownInventory()) << "nothing recovered before the worker runs";
    for(int i=0;i<8&&pending();++i)(void)f.wallet->RecoverActiveWalletFromCanonicalSource();
    EXPECT_FALSE(pending()) << "account-0 recovery completes through the normal worker passes";
    const auto catalog=f.Catalog();
    ASSERT_TRUE(catalog&&catalog->recovered);
    EXPECT_EQ(f.Account(0).account.Scan().BalanceUna(),500000u);
    ctx.walletName="restored-v2";
    din::Json account;account["account"]=Json::UInt64(0);
    const auto shown=rpc_context_wallet_orchard_getbalance(ctx,account);
    ASSERT_FALSE(shown.isMember("error")) << shown.toStyledString();
    EXPECT_EQ(shown["confirmed_una"].asUInt64(),500000u) << shown.toStyledString();
}

// A recovery that cannot proceed is never silent: the worker records the
// reason and wallet.getwalletinfo reports it while the mark stays set.
TEST(WalletV3NativeRestore, RefusedAutomaticRecoveryIsReported) {
    CanonicalRecoveryFixture f(false,false);   // generated wallet: account-0 recovery refuses
    (void)f.MineToWallet();
    NativeV3IndexScope v3(f,"v3-native-restore-refused-index");
    WalletService::AcquireWalletUse(f.wallet)->Wallet().setSetting(
        WalletManager::kOrchardAccount0RestorePendingSetting,"1");
    std::string error;
    for(int i=0;i<2;++i)(void)f.wallet->RecoverActiveWalletFromCanonicalSource(&error);
    ExecutionContext ctx;ctx.daemon=&f.context;ctx.walletName="canonical-recovery";
    const auto info=rpc_context_wallet_getwalletinfo(ctx,din::obj());
    ASSERT_TRUE(info.isMember("orchard_account0_recovery")) << info.toStyledString();
    EXPECT_EQ(info["orchard_account0_recovery"]["state"].asString(),"in_progress");
    EXPECT_FALSE(info["orchard_account0_recovery"]["detail"].asString().empty()) << info.toStyledString();
}

// The automatic recovery is bound to the restored wallet's session: after
// a switch to another wallet, a pass for the old session writes nothing,
// and the new wallet's markers are never touched by the old recovery.
TEST(OrchardAccount0Recovery, AutomaticRecoveryStaysWithItsWallet) {
    Account0RecoveryFixture f;
    ExecutionContext ctx;ctx.daemon=&f.context;
    din::Json params(Json::arrayValue);
    params.append("restored-a");params.append(RecoveryTestPhrase);params.append("");params.append("");
    ASSERT_TRUE(rpc_context_wallet_restore(ctx,params)["success"].asBool());
    uint64_t session_a=0;
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        session_a=use->Wallet().AcquireDatabaseLease()->Session();
        ASSERT_EQ(use->Wallet().getSetting(WalletManager::kOrchardAccount0RestorePendingSetting),"1");
        use->Wallet().open("before-recovery");   // switch to wallet B
        EXPECT_EQ(use->Wallet().getSetting(WalletManager::kOrchardAccount0RestorePendingSetting),"");
        use->Wallet().setSetting(WalletManager::kOrchardAccount0RestoreStatusSetting,"b-untouched");
    }
    {
        auto use=WalletService::AcquireWalletUse(f.wallet);
        auto index=ChainstateService::AcquireWalletIndexUse(f.f.service);
        f.f.service->continueRestoredOrchardAccount0(use->Wallet(),session_a);   // stale session
        f.f.service->continueRestoredOrchardAccount0(use->Wallet(),0);           // B: nothing pending
        EXPECT_EQ(use->Wallet().getSetting(WalletManager::kOrchardAccount0RestoreStatusSetting),"b-untouched");
        EXPECT_EQ(use->Wallet().getSetting(WalletManager::kOrchardAccount0RestorePendingSetting),"");
        use->Wallet().open("restored-a");
        EXPECT_EQ(use->Wallet().getSetting(WalletManager::kOrchardAccount0RestorePendingSetting),"1")
            << "A's pending recovery survives the switch";
    }
}

} // namespace dinero
