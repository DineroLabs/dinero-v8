#pragma once
#include "rpc/methods_utreexo.h"
din::Json rpc_context_wallet_snapshot(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_getbalance(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_getwalletinfo(const ExecutionContext&, const din::Json&);
namespace din { Json rpc_getaddressbatch(const ExecutionContext&, const Json&);
                Json rpc_getaddresshistory(const ExecutionContext&, const Json&); }
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
TEST(WalletProofContext, IndependentFullSourceRetainsNormalBalanceSurfaces) {
    CanonicalRecoveryFixture f; ExecutionContext ctx; ctx.daemon = &f.context;
    EXPECT_FALSE(f.f.service->requiresImportedRuntimeBalanceReconciliation());
    // Regression of the existing source path only, not a new all-store
    // baseline/readiness receipt or permission to promote assumed history.
    const auto balance = rpc_context_wallet_getbalance(ctx, din::obj());
    ASSERT_FALSE(balance.isMember("error")) << balance.toStyledString();
    EXPECT_TRUE(balance["confirmed"].isNumeric());
    const auto info = rpc_context_wallet_getwalletinfo(ctx, din::obj());
    ASSERT_FALSE(info.isMember("error")) << info.toStyledString();
    EXPECT_TRUE(info["balance"].isNumeric());
    const auto snapshot = rpc_context_wallet_snapshot(ctx, din::obj());
    ASSERT_FALSE(snapshot.isMember("error")) << snapshot.toStyledString();
    EXPECT_TRUE(snapshot["balances"]["confirmed"].isNumeric());
}
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
// A full-source history reply must not claim completeness over a canonical
// height it could not read. This fixture's canonical chain has height hashes
// but no readable block bodies, and both address history surfaces once
// skipped every unread height silently and still answered
// history_complete=true. A missing height hash is skipped the same way.
void ExpectUnreadCoverage(const din::Json& reply, int tip) {
    ASSERT_FALSE(reply.isMember("error")) << reply.toStyledString();
    EXPECT_FALSE(reply["history_complete"].asBool()) << reply.toStyledString();
    const auto& coverage = reply["history_coverage"];
    EXPECT_TRUE(coverage.isObject()) << reply.toStyledString();
    if (!coverage.isObject()) return;
    EXPECT_EQ(coverage["scan_from_height"].asInt(), tip) << reply.toStyledString();
    EXPECT_EQ(coverage["lowest_scanned_height"].asInt(), 0) << reply.toStyledString();
    EXPECT_EQ(coverage["stop_reason"].asString(), "genesis") << reply.toStyledString();
    EXPECT_EQ(coverage["skipped_heights"].asUInt64(), static_cast<uint64_t>(tip) + 1) << reply.toStyledString();
    EXPECT_EQ(coverage["unresolved_inputs"].asUInt64(), 0u) << reply.toStyledString();
    EXPECT_FALSE(coverage["chain_changed"].asBool()) << reply.toStyledString();
    const auto& listed = coverage["skipped_height_list"];
    ASSERT_EQ(listed.size(), std::min<::Json::ArrayIndex>(32, tip + 1)) << reply.toStyledString();
    EXPECT_EQ(listed[0].asInt(), tip) << reply.toStyledString();
}
TEST(WalletProofContext, AddressHistoryReportsUnreadCanonicalHeights) {
    CanonicalRecoveryFixture f; ExecutionContext ctx; ctx.daemon = &f.context;
    // A full source scans down to genesis; AssumeUTXO would skip the loop.
    ASSERT_FALSE(f.f.service->IsAssumeUTXOActive());
    const auto tip = f.f.db.getTip(); ASSERT_TRUE(tip.ok()); ASSERT_GT(tip->height, 2);
    std::string address;
    { auto use = WalletService::AcquireWalletUse(f.wallet); address = use->Wallet().getNewAddress(); }
    const auto history = [&] {
        auto params = din::arr(); params.append(address); params.append(200);
        return din::rpc_getaddresshistory(ctx, params);
    };
    int batch_count = 0;
    const auto batch = [&] {
        // A distinct history_count per call keeps each reply out of the cache.
        auto params = din::obj(); params["addresses"] = din::arr(); params["addresses"].append(address);
        params["history_count"] = ++batch_count;
        const auto reply = din::rpc_getaddressbatch(ctx, params);
        EXPECT_FALSE(reply["batch_meta"]["cache_hit"].asBool()) << reply.toStyledString();
        return reply;
    };
    ASSERT_NO_FATAL_FAILURE(ExpectUnreadCoverage(history(), tip->height));
    ASSERT_NO_FATAL_FAILURE(ExpectUnreadCoverage(batch(), tip->height));
    // A missing canonical height hash is counted, not dropped.
    const int height = tip->height / 2;
    const auto hash = f.f.db.getBlockHashByHeight(height); ASSERT_TRUE(hash.ok());
    ASSERT_EQ(f.f.db.deleteHeightIndex(f.f.token, height), Status::Ok);
    ASSERT_NO_FATAL_FAILURE(ExpectUnreadCoverage(history(), tip->height));
    ASSERT_NO_FATAL_FAILURE(ExpectUnreadCoverage(batch(), tip->height));
    ASSERT_EQ(f.f.db.putHeightIndex(f.f.token, height, *hash), Status::Ok);
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
