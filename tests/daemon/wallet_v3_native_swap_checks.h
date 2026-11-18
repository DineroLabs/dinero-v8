#pragma once
// v8.2.5: DIN↔BTC swap funding from a native wallet on a full-archive v3
// node. Orchard-origin only; uses the native v3 route helpers
// (NativeV3IndexScope, NativeTestCount, ExpectNativeReceiptsCurrent).
#include "daemon/services/swap_service.h"
#include "wallet/swap/htlc.h"
#include "wallet/swap/runner.h"
#include "rpc/swap_rpc.h"

void registerWalletMethodsContext();  // src/rpc/methods_wallet_context.cpp

namespace dinero {

namespace {
swap::DinFundingRequest V3SwapFundingRequest(WalletManager& wallet) {
    swap::DinFundingRequest r;
    r.binding.wallet_id = wallet.AcquireDatabaseLease()->ReadDeliveryIdentity();
    r.binding.fee_rate_hint = 1;
    r.binding.maximum_fee_una = 10000;
    r.offer_id.fill(0x6a);
    r.amount_una = 20000;
    swap::DinHtlcTerms terms;
    terms.payment_hash.fill(0x27);
    terms.refund_locktime_unix = 1800200000;
    std::array<uint8_t, 32> secret{};
    int parity = 0;
    secret.back() = 13;
    if (!TaprootKeys::DeriveXOnlyPubkey(secret, terms.claim_pubkey, parity)) throw std::runtime_error("claim key");
    secret.back() = 17;
    if (!TaprootKeys::DeriveXOnlyPubkey(secret, terms.refund_pubkey, parity)) throw std::runtime_error("refund key");
    const auto output = swap::BuildDinHtlc(terms);
    r.address = AddressCodec::encodeP2TR(Network::REGTEST,
        std::vector<uint8_t>(output.output_key.begin(), output.output_key.end()));
    return r;
}
}  // namespace

// A native-complete wallet on v3 funds a swap HTLC: the bound swap_funding
// request is retained with change on a pooled (already covered) address, no
// address row is derived, both receipts stay certified, the exact retained
// body is submitted, mined and delivered, and the HTLC output exists.
TEST(WalletV3NativeSwap, SwapFundingRetainsWithPooledChangeAndKeepsReceipts) {
    CanonicalRecoveryFixture f(true, true);   // native wallet with a funded H101 coin
    (void)f.MineToWallet();
    NativeV3IndexScope v3(f, "v3-native-swap-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ExpectNativeReceiptsCurrent(f, 102);
    f.context.tx_ingress = f.f.ingress.get();
    auto request = V3SwapFundingRequest(WalletService::AcquireWalletUse(f.wallet)->Wallet());
    const auto rows_before = NativeTestCount(WalletService::AcquireWalletUse(f.wallet)->Wallet(),
        "SELECT count(*) FROM addresses");
    auto owner = MakeRetainedSwapFundingOwner(f.context);

    swap::PreparedDinFunding prepared;
    ASSERT_NO_THROW(prepared = owner(request, swap::DinFundingOperation::Prepare, {}));
    {
        auto use = WalletService::AcquireWalletUse(f.wallet);
        const auto payments = use->Wallet().getPendingPayments();
        ASSERT_EQ(payments.size(), 1u);
        ASSERT_TRUE(payments.front().intent.request);
        EXPECT_EQ(payments.front().intent.request->domain, PendingPaymentRequestDomain::SwapFunding);
        EXPECT_EQ(payments.front().intent.address, request.address);
        EXPECT_EQ(payments.front().intent.amount_una, request.amount_una);
        EXPECT_EQ(prepared.raw, payments.front().signed_body);
        EXPECT_EQ(NativeTestCount(use->Wallet(), "SELECT count(*) FROM addresses"), rows_before)
            << "swap funding derived a script on a native v3 wallet";
    }
    ExpectNativeReceiptsCurrent(f, 102);
    // Resolve returns the same retained body; nothing new is created.
    EXPECT_EQ(owner(request, swap::DinFundingOperation::Resolve, {}).raw, prepared.raw);

    const auto submitted = owner(request, swap::DinFundingOperation::SubmitRetained, prepared.raw);
    EXPECT_EQ(submitted.txid, prepared.txid);
    const auto pooled = f.f.ingress->mempool().getMempoolEntry(uint256::FromHexUnsafe(prepared.txid));
    ASSERT_TRUE(bool(pooled));
    ASSERT_TRUE(f.Mine(pooled->tx));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ExpectNativeReceiptsCurrent(f, 103);
    // The HTLC output is on chain with the exact amount.
    bool found = false;
    for (uint32_t vout = 0; vout < 4; ++vout) {
        const auto coin = f.f.db.getCoin(uint256::FromHexUnsafe(prepared.txid), vout);
        if (coin.ok() && coin->amount == int64_t(request.amount_una)) found = true;
    }
    EXPECT_TRUE(found) << "HTLC output missing";
    f.context.tx_ingress = nullptr;
}

// v8.2.5: an ordinary one-recipient wallet.sendmany works on a native v3
// wallet too (it shares the bound path): change on a pooled address, no new
// address row, receipts certified, mined and delivered.
TEST(WalletV3NativeSwap, SendManyOneRecipientPaysWithPooledChange) {
    CanonicalRecoveryFixture f(true, true);
    (void)f.MineToWallet();
    NativeV3IndexScope v3(f, "v3-native-sendmany-index");
    std::string error;
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    f.context.tx_ingress = f.f.ingress.get();
    const auto rows_before = NativeTestCount(WalletService::AcquireWalletUse(f.wallet)->Wallet(),
        "SELECT count(*) FROM addresses");
    ExecutionContext request; request.daemon = &f.context; request.walletName = "canonical-recovery";
    din::Json many(Json::arrayValue); din::Json recipients = din::obj();
    recipients[OrchardMiningPayout] = 0.001; many.append(recipients);
    const auto sent = rpc_context_wallet_sendmany(request, many);
    ASSERT_TRUE(sent.isMember("txid")) << sent.toStyledString();
    EXPECT_EQ(NativeTestCount(WalletService::AcquireWalletUse(f.wallet)->Wallet(),
        "SELECT count(*) FROM addresses"), rows_before);
    ExpectNativeReceiptsCurrent(f, 102);
    const auto pooled = f.f.ingress->mempool().getMempoolEntry(uint256::FromHexUnsafe(sent["txid"].asString()));
    ASSERT_TRUE(bool(pooled));
    ASSERT_TRUE(f.Mine(pooled->tx));
    ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
        WalletService::CanonicalRecovery::AppliedPrefix) << error;
    ExpectNativeReceiptsCurrent(f, 103);
    f.context.tx_ingress = nullptr;
}

namespace {
// A legacy-class wallet (no Orchard owner) enrolled on a v3 node: the shape
// of the owner's Windows and Dell wallets.
struct LegacyV3SwapFixture {
    CanonicalRecoveryFixture f{true, true};
    std::unique_ptr<UTXOIndex> original;
    LegacyV3SwapFixture() {
        (void)f.PrepareFundedHistoricalWallet();
        f.DowngradeToPreIdentityTransparentWallet();
        (void)f.MineToWallet();
        const auto index_path = f.f.path / "v3-legacy-swap-index";
        { UTXOIndex v2(index_path.string()); OrchardAdmissionFixture::Require(v2.Initialize()); }
        (void)UpgradeFullArchiveWalletIndexSchemaToV3(index_path);
        auto metadata = std::make_unique<UTXOIndex>(index_path.string());
        OrchardAdmissionFixture::Require(metadata->InitializeFullArchiveV3MetadataOnly());
        {
            auto use = WalletService::AcquireWalletUse(f.wallet);
            use->Wallet().setUTXOIndex(nullptr);
            use->Wallet().DisableLegacyRescansForFullArchiveV3();
        }
        original = ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(*f.f.service, std::move(metadata));
        std::string error;
        OrchardAdmissionFixture::Require(f.wallet->RecoverActiveWalletFromCanonicalSource(&error) ==
            WalletService::CanonicalRecovery::AppliedPrefix);
    }
    ~LegacyV3SwapFixture() {
        auto replacement = ShieldedStateStartupTestAccess::SwapWalletIndexForRpc(*f.f.service, std::move(original));
        (void)replacement;
    }
};
}  // namespace

// A legacy-class wallet (no Orchard owner, so no binding authority) is
// refused by swap funding with a clear "create a new wallet" message, before
// any payment, address row or receipt change.
TEST(WalletV3NativeSwap, LegacyClassSwapFundingRefusesClearlyWithoutEffects) {
    LegacyV3SwapFixture x;
    auto& f = x.f;
    {
        auto use = WalletService::AcquireWalletUse(f.wallet);
        ASSERT_EQ(use->Wallet().AcquireDatabaseLease()->LegacyTransparentState(),
            WalletManager::LegacyTransparentState::Complete);
    }
    f.context.tx_ingress = f.f.ingress.get();
    auto request = V3SwapFundingRequest(WalletService::AcquireWalletUse(f.wallet)->Wallet());
    const auto rows_before = NativeTestCount(WalletService::AcquireWalletUse(f.wallet)->Wallet(),
        "SELECT count(*) FROM addresses");
    auto owner = MakeRetainedSwapFundingOwner(f.context);
    try {
        (void)owner(request, swap::DinFundingOperation::Prepare, {});
        ADD_FAILURE() << "legacy-class swap funding was not refused";
    } catch (const std::exception& e) {
        EXPECT_STREQ(e.what(), WalletManager::kSwapLegacyWalletUnsupported);
    }
    EXPECT_TRUE(WalletService::AcquireWalletUse(f.wallet)->Wallet().getPendingPayments().empty());
    EXPECT_EQ(NativeTestCount(WalletService::AcquireWalletUse(f.wallet)->Wallet(),
        "SELECT count(*) FROM addresses"), rows_before);
    f.context.tx_ingress = nullptr;
}

// Swap offers/accepts need a DIN payout address. On a native v3 wallet it is
// the next sealed pooled address, so the payout is delivered and the receipts
// stay certified.
TEST(WalletV3NativeSwap, PayoutAddressComesFromTheSealedPool) {
    registerWalletMethodsContext();
    {
        CanonicalRecoveryFixture f(true, true);
        (void)f.MineToWallet();
        NativeV3IndexScope v3(f, "v3-native-payout-index");
        std::string error;
        ASSERT_EQ(f.wallet->RecoverActiveWalletFromCanonicalSource(&error),
            WalletService::CanonicalRecovery::AppliedPrefix) << error;
        ExecutionContext ctx; ctx.daemon = &f.context; ctx.walletName = "canonical-recovery";
        std::string address;
        ASSERT_NO_THROW(address = SwapWalletPayoutAddress(ctx));
        EXPECT_EQ(NativeTestCount(WalletService::AcquireWalletUse(f.wallet)->Wallet(),
            "SELECT count(*) FROM native_v3_address_pool WHERE issued=1 AND address='" + address + "'"), 1)
            << "native payout comes from the sealed pool";
        ExpectNativeReceiptsCurrent(f, 102);
    }
}

}  // namespace dinero
