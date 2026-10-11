#pragma once
#include "rpc/methods_utreexo.h"
din::Json rpc_context_wallet_snapshot(const ExecutionContext&, const din::Json&);
namespace din { Json rpc_getaddressbatch(const ExecutionContext&, const Json&); }
namespace dinero {
namespace {
void ExpectUnavailableProofContext(const din::Json& context) {
    ASSERT_TRUE(context.isObject());
    EXPECT_FALSE(context["available"].asBool()) << context.toStyledString();
    EXPECT_TRUE(context.isMember("error"));
    for (const auto* name : {"tip_height", "tip_hash", "utreexo_root"})
        EXPECT_FALSE(context.isMember(name)) << name << context.toStyledString();
}
void ExpectMatchingProofContext(const din::Json& context,
                                const ChainstateService::UtreexoRpcSnapshot& snapshot) {
    ASSERT_TRUE(context["available"].asBool()) << context.toStyledString();
    EXPECT_FALSE(context.isMember("error"));
    EXPECT_EQ(context["tip_height"].asUInt64(), snapshot.height);
    EXPECT_EQ(context["tip_hash"].asString(), snapshot.block_hash.GetHex());
    EXPECT_EQ(context["utreexo_root"].asString(), util::hex(snapshot.commitment));
}
}
TEST(WalletProofContext, MissingOwnerHasNoTuple) {
    ExecutionContext ctx;
    ASSERT_NO_FATAL_FAILURE(ExpectUnavailableProofContext(din::BuildUtreexoProofContext(ctx)));
    DaemonContext daemon; daemon.chainstate = std::make_shared<ChainstateService>(); ctx.daemon = &daemon;
    ASSERT_NO_FATAL_FAILURE(ExpectUnavailableProofContext(din::BuildUtreexoProofContext(ctx)));
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(WalletProofContext, ActualWalletSnapshotRefusesUnavailableTuple) {
    CanonicalRecoveryFixture f; ExecutionContext ctx; ctx.daemon = &f.context;
    const auto current = f.f.service->getUtreexoRpcSnapshot(); ASSERT_TRUE(current.ok());
    const auto good = rpc_context_wallet_snapshot(ctx, din::obj());
    ASSERT_FALSE(good.isMember("error")) << good.toStyledString();
    ASSERT_NO_FATAL_FAILURE(ExpectMatchingProofContext(good["proof_context"], *current));
    const auto tip = f.f.db.getValidatedTip(); ASSERT_TRUE(tip.ok());
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height - 1), Status::Ok);
    const auto unavailable = rpc_context_wallet_snapshot(ctx, din::obj());
    ASSERT_FALSE(unavailable.isMember("error")) << unavailable.toStyledString();
    ASSERT_NO_FATAL_FAILURE(ExpectUnavailableProofContext(unavailable["proof_context"]));
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height), Status::Ok);
    const auto restored = rpc_context_wallet_snapshot(ctx, din::obj());
    ASSERT_NO_FATAL_FAILURE(ExpectMatchingProofContext(restored["proof_context"], *current));
}
TEST(WalletProofContext, CachedAddressBatchRechecksAvailability) {
    CanonicalRecoveryFixture f; ExecutionContext ctx; ctx.daemon = &f.context;
    std::string address;
    { auto use = WalletService::AcquireWalletUse(f.wallet); address = use->Wallet().getNewAddress(); }
    auto params = din::obj(); params["addresses"] = din::arr(); params["addresses"].append(address); params["history_count"] = 1;
    const auto current = f.f.service->getUtreexoRpcSnapshot(); ASSERT_TRUE(current.ok());
    const auto first = din::rpc_getaddressbatch(ctx, params);
    ASSERT_FALSE(first.isMember("error")) << first.toStyledString();
    ASSERT_NO_FATAL_FAILURE(ExpectMatchingProofContext(first["proof_context"], *current));
    const auto tip = f.f.db.getValidatedTip(); ASSERT_TRUE(tip.ok());
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height - 1), Status::Ok);
    const auto cached = din::rpc_getaddressbatch(ctx, params);
    ASSERT_FALSE(cached.isMember("error")) << cached.toStyledString();
    ASSERT_TRUE(cached["batch_meta"]["cache_hit"].asBool());
    EXPECT_EQ(cached["addresses"], first["addresses"]);
    ASSERT_NO_FATAL_FAILURE(ExpectUnavailableProofContext(cached["proof_context"]));
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height), Status::Ok);
    const auto retry = din::rpc_getaddressbatch(ctx, params);
    ASSERT_FALSE(retry.isMember("error")) << retry.toStyledString();
    EXPECT_EQ(retry["addresses"], first["addresses"]);
    ASSERT_NO_FATAL_FAILURE(ExpectMatchingProofContext(retry["proof_context"], *current));
}
TEST(WalletProofContext, HistoricalCompactAndStoppedOwner) {
    HistoricalPreparationFixture f; ExecutionContext ctx; ctx.daemon = &f.context;
    const auto current = f.service->getUtreexoRpcSnapshot(); ASSERT_TRUE(current.ok());
    ASSERT_TRUE(current->compact);
    ASSERT_NO_FATAL_FAILURE(ExpectMatchingProofContext(din::BuildUtreexoProofContext(ctx), *current));
    f.service->Stop();
    ASSERT_NO_FATAL_FAILURE(ExpectUnavailableProofContext(din::BuildUtreexoProofContext(ctx)));
}
#endif
}
