#pragma once
// v8.2.5 (#939 follow-up): restoring a wallet from its seed on a full-archive
// v3 node. Included with the native v3 Orchard spend checks (Orchard-origin
// only): uses CanonicalRecoveryFixture, NativeV3IndexScope and NativeTestCount.

#include "wallet/bip39.h"

namespace dinero {

namespace {
constexpr const char* kV3RestoreMnemonic =
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
}

din::Json RestoreOnV3(CanonicalRecoveryFixture& f,const char* name,const char* mnemonic){
    ExecutionContext ctx;ctx.daemon=&f.context;
    din::Json params(Json::arrayValue);
    params.append(name);params.append(mnemonic);params.append("");params.append("restore-pass");
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
    const auto restored=RestoreOnV3(f,"restored-orchard-on-v3",kV3RestoreMnemonic);
    ExpectRestoredOnV3(f,restored,"restored-orchard-on-v3");
    WalletService::AcquireWalletUse(f.wallet)->Wallet().unlockWallet("restore-pass",0);
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ExecutionContext ctx;ctx.daemon=&f.context;ctx.walletName="restored-orchard-on-v3";
    auto recovered=rpc_context_wallet_orchard_recoveraccount0(ctx,din::obj());
    ASSERT_FALSE(recovered.isMember("error")) << recovered.toStyledString();
    for(int i=0;i<4&&!recovered["complete"].asBool();++i){
        ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
            WalletService::CanonicalRecovery::AppliedPrefix) << error;
        recovered=rpc_context_wallet_orchard_recoveraccount0(ctx,din::obj());
        ASSERT_FALSE(recovered.isMember("error")) << recovered.toStyledString();
    }
    EXPECT_TRUE(recovered["complete"].asBool()) << recovered.toStyledString();
    din::Json account;account["account"]=Json::UInt64(0);
    const auto shown=rpc_context_wallet_orchard_getbalance(ctx,account);
    ASSERT_FALSE(shown.isMember("error")) << shown.toStyledString();
    EXPECT_EQ(shown["confirmed_una"].asUInt64(),500000u) << shown.toStyledString();
    // The transparent side of the restored wallet stays certified.
    const auto balance=rpc_context_wallet_getbalance(ctx,din::obj());
    EXPECT_FALSE(balance.isMember("error")) << balance.toStyledString();
}

} // namespace dinero
