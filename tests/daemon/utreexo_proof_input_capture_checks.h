#pragma once
#include "rpc/methods_utreexo.h"
#include "daemon/chainstate_recovery_marker.h"
din::Json rpc_context_wallet_verifyutxoproof(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_utxoproof(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_getproofbundle(const ExecutionContext&, const din::Json&);
din::Json rpc_context_wallet_validatestatelessbalance(const ExecutionContext&, const din::Json&);
namespace dinero {
struct UtreexoProofCaptureTestAccess {
    static void ActiveBase(ChainstateService& service, const uint256& hash, uint32_t height) {
        service.assumeutxo_active_ = true; service.assumeutxo_base_block_ = hash;
        service.assumeutxo_base_height_ = height; service.promoted_base_height_ = 0;
    }
    static void PromotedBase(ChainstateService& service, uint32_t height) {
        service.assumeutxo_active_ = false; service.assumeutxo_base_block_.SetNull();
        service.assumeutxo_base_height_ = 0; service.promoted_base_height_ = height;
    }
};
TEST(UtreexoProofInputCapture, UnavailableAndBoundedRequests) {
    auto service = std::make_shared<ChainstateService>();
    const std::vector<OutPoint> empty;
    EXPECT_FALSE(service->CaptureUtreexoProofInputs(empty).ok());
    const std::vector<OutPoint> excessive(1001, OutPoint(TxId(uint256{}), 0));
    EXPECT_EQ(service->CaptureUtreexoProofInputs(excessive).status(), Status::Invalid);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(UtreexoProofInputCapture, CapturedCoinAndForestSurviveRealSelectedAdvance) {
    CanonicalRecoveryFixture f;
    const OutPoint point(f.f.blocks[1].vtx.front().GetTxid(), 0);
    const auto coin = f.f.db.getCoin(point.txid.AsUint256(), point.vout);
    ASSERT_TRUE(coin.ok());
    const std::vector<OutPoint> points{point, OutPoint(TxId(uint256{}), 0)};
    const auto captured = f.f.service->CaptureUtreexoProofInputs(points);
    ASSERT_TRUE(captured.ok());
    ASSERT_EQ(captured->inputs.size(), points.size());
    ASSERT_EQ(captured->inputs[0].status, Status::Ok);
    ASSERT_TRUE(captured->inputs[0].coin);
    EXPECT_EQ(captured->inputs[0].coin->amount, coin->amount);
    EXPECT_EQ(captured->inputs[0].coin->script_pubkey, coin->script_pubkey);
    EXPECT_EQ(captured->inputs[0].coin->height, coin->height);
    EXPECT_EQ(captured->inputs[0].coin->coinbase, coin->coinbase);
    EXPECT_EQ(captured->inputs[1].status, Status::NotFound);
    EXPECT_FALSE(captured->inputs[1].coin);
    std::vector<uint8_t> script;
    ASSERT_TRUE(util::unhex(coin->script_pubkey, script));
    const auto leaf = consensus::HashUTXOForCreationHeight(
        point.txid.AsUint256(), point.vout, coin->amount, script, coin->height, coin->coinbase);
    const auto position = captured->forest.findLeafPosition(leaf);
    ASSERT_TRUE(position);
    f.MineEmpty();
    const auto current = f.f.service->getUtreexoRpcSnapshot();
    ASSERT_TRUE(current.ok());
    EXPECT_NE(current->block_hash, captured->snapshot.block_hash);
    EXPECT_GT(current->height, captured->snapshot.height);
    EXPECT_EQ(captured->forest.getCommitment(), captured->snapshot.commitment);
    const auto proof = captured->forest.prove(*position);
    ASSERT_TRUE(proof);
    EXPECT_EQ(proof->numLeaves, captured->snapshot.num_leaves);
    EXPECT_TRUE(proof->verify(leaf, captured->forest.getRoots()));
}
TEST(UtreexoProofInputCapture, RefusesMismatchNestedCaptureAndCompactGeneration) {
    const std::vector<OutPoint> empty;
    {
    CanonicalRecoveryFixture f;
    const auto good = f.f.service->CaptureUtreexoProofInputs(empty);
    ASSERT_TRUE(good.ok());
    {
        auto selected = f.f.service->AcquireBlockIngressActivationLock();
        EXPECT_EQ(f.f.service->CaptureUtreexoProofInputs(empty).status(), Status::Invalid);
    }
    const auto tip = f.f.db.getValidatedTip(); ASSERT_TRUE(tip.ok());
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height - 1), Status::Ok);
    EXPECT_FALSE(f.f.service->CaptureUtreexoProofInputs(empty).ok());
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height), Status::Ok);
    const auto retry = f.f.service->CaptureUtreexoProofInputs(empty);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry->snapshot.block_hash, good->snapshot.block_hash);
    EXPECT_EQ(retry->snapshot.commitment, good->snapshot.commitment);
    }
    HistoricalPreparationFixture compact;
    ASSERT_TRUE(compact.service->getUtreexoRpcSnapshot().ok());
    EXPECT_EQ(compact.service->CaptureUtreexoProofInputs(empty).status(), Status::Invalid);
}

TEST(UtreexoProofInputCapture, SingleRpcProofAndTupleBindCapturedOwner) {
    CanonicalRecoveryFixture f; ExecutionContext ctx; ctx.daemon = &f.context;
    const OutPoint point(f.f.blocks[1].vtx.front().GetTxid(), 0);
    auto request = din::arr(); request.append(point.txid.AsUint256().GetHex()); request.append(point.vout);
    const auto check = [&] {
        const std::vector<OutPoint> points{point};
        const auto expected = f.f.service->CaptureUtreexoProofInputs(points);
        ASSERT_TRUE(expected.ok()); ASSERT_TRUE(expected->inputs.front().canonical);
        ASSERT_TRUE(expected->inputs.front().coin);
        const auto response = din::rpc_getutxoproof(ctx, request);
        ASSERT_FALSE(response.isMember("error")) << response.toStyledString();
        EXPECT_EQ(response["accumulator_root"].asString(), util::hex(expected->snapshot.commitment));
        EXPECT_EQ(response["block_hash"].asString(), expected->snapshot.block_hash.GetHex());
        EXPECT_EQ(response["height"].asUInt64(), expected->snapshot.height);
        const auto& coin = *expected->inputs.front().coin;
        EXPECT_EQ(response["created_height"].asUInt64(), uint64_t(coin.height));
        EXPECT_EQ(response["coinbase"].asBool(), coin.coinbase);
        EXPECT_EQ(response["script_pubkey"].asString(), coin.script_pubkey);
        consensus::UtreexoHash leaf;
        ASSERT_TRUE(util::unhex(response["leaf_hash"].asString(), leaf));
        consensus::UtreexoProof proof;
        proof.position = response["position"].asUInt64();
        proof.numLeaves = response["num_leaves"].asUInt64();
        for (const auto& encoded : response["siblings"]) {
            consensus::UtreexoHash hash; ASSERT_TRUE(util::unhex(encoded.asString(), hash));
            proof.siblings.push_back(std::move(hash));
        }
        EXPECT_TRUE(proof.verify(leaf, expected->forest.getRoots()));
    };
    ASSERT_NO_FATAL_FAILURE(check());
    f.MineEmpty();
    ASSERT_NO_FATAL_FAILURE(check());
}
TEST(UtreexoProofInputCapture, SingleRpcRefusesInvalidOwnerAndInputWithoutTuple) {
    CanonicalRecoveryFixture f; ExecutionContext ctx; ctx.daemon = &f.context;
    const OutPoint point(f.f.blocks[1].vtx.front().GetTxid(), 0);
    auto request = din::arr(); request.append(point.txid.AsUint256().GetHex()); request.append(point.vout);
    const auto refused = [&](const din::Json& response) {
        EXPECT_TRUE(response.isMember("error")) << response.toStyledString();
        for (const auto* name : {"accumulator_root", "block_hash", "height", "siblings", "leaf_hash"})
            EXPECT_FALSE(response.isMember(name)) << name << response.toStyledString();
    };
    const auto tip = f.f.db.getValidatedTip(); ASSERT_TRUE(tip.ok());
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height - 1), Status::Ok);
    refused(din::rpc_getutxoproof(ctx, request));
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height), Status::Ok);
    ASSERT_FALSE(din::rpc_getutxoproof(ctx, request).isMember("error"));
    for (const auto& bad : std::vector<std::string>{"", std::string(63, '0'), std::string(65, '0'),
             std::string(64, 'g'), std::string(63, 'a') + '\0'}) {
        auto invalid = din::arr(); invalid.append(bad); invalid.append(point.vout);
        const auto response = din::rpc_getutxoproof(ctx, invalid);
        refused(response); EXPECT_EQ(response["error"]["code"].asInt(), -32602);
    }
    auto invalid = request; invalid[1] = -1;
    const auto negative = din::rpc_getutxoproof(ctx, invalid);
    refused(negative); EXPECT_EQ(negative["error"]["code"].asInt(), -32602);
}

TEST(UtreexoProofInputCapture, BatchRpcPreservesOrderAndRefusesUnavailableOwner) {
    CanonicalRecoveryFixture f; ExecutionContext ctx; ctx.daemon = &f.context;
    const OutPoint point(f.f.blocks[1].vtx.front().GetTxid(), 0);
    const auto op = [](std::string txid, uint32_t vout) {
        auto out = din::obj(); out["txid"] = txid; out["vout"] = vout; return out;
    };
    auto entries = din::arr();
    entries.append(op(point.txid.AsUint256().GetHex(), point.vout));
    entries.append(din::Json("invalid"));
    entries.append(op(std::string(64, '0'), 0));
    entries.append(op(std::string(64, 'g'), 0));
    auto request = din::arr(); request.append(entries);
    const auto expected = f.f.service->CaptureUtreexoProofInputs(std::vector<OutPoint>{point});
    ASSERT_TRUE(expected.ok());
    const auto result = din::rpc_getutxoproofs_batch(ctx, request);
    ASSERT_FALSE(result.isMember("error")) << result.toStyledString();
    EXPECT_EQ(result["utreexo_root"].asString(), util::hex(expected->snapshot.commitment));
    EXPECT_EQ(result["block_hash"].asString(), expected->snapshot.block_hash.GetHex());
    EXPECT_EQ(result["height"].asUInt64(), expected->snapshot.height);
    ASSERT_EQ(result["proofs"].size(), 4u);
    EXPECT_EQ(result["successful"].asUInt64(), 1u); EXPECT_EQ(result["failed"].asUInt64(), 3u);
    EXPECT_TRUE(result["proofs"][0]["success"].asBool());
    EXPECT_EQ(result["proofs"][1]["error_code"].asString(), "invalid-format");
    EXPECT_EQ(result["proofs"][2]["error_code"].asString(), "utxo-not-found");
    EXPECT_EQ(result["proofs"][3]["error_code"].asString(), "invalid-txid");
    const auto& encoded = result["proofs"][0]["proof"];
    consensus::UtreexoProof proof;
    proof.position = encoded["position"].asUInt64(); proof.numLeaves = encoded["num_leaves"].asUInt64();
    for (const auto& item : encoded["siblings"]) {
        consensus::UtreexoHash h; ASSERT_TRUE(util::unhex(item.asString(), h)); proof.siblings.push_back(std::move(h));
    }
    const auto& coin = *expected->inputs.front().coin;
    std::vector<uint8_t> script; ASSERT_TRUE(util::unhex(coin.script_pubkey, script));
    const auto leaf = consensus::HashUTXOForCreationHeight(point.txid.AsUint256(), point.vout,
        coin.amount, script, coin.height, coin.coinbase);
    EXPECT_TRUE(proof.verify(leaf, expected->forest.getRoots()));
    const auto tip = f.f.db.getValidatedTip(); ASSERT_TRUE(tip.ok());
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height - 1), Status::Ok);
    const auto unavailable = din::rpc_getutxoproofs_batch(ctx, request);
    EXPECT_TRUE(unavailable.isMember("error"));
    for (const auto* field : {"utreexo_root", "block_hash", "height", "proofs"})
        EXPECT_FALSE(unavailable.isMember(field));
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height), Status::Ok);
    EXPECT_FALSE(din::rpc_getutxoproofs_batch(ctx, request).isMember("error"));
}

TEST(UtreexoProofInputCapture, UpdatesCarryOneVerifiableContextAndRefuseUnavailableOwner) {
    CanonicalRecoveryFixture f; ExecutionContext ctx; ctx.daemon = &f.context;
    const OutPoint point(f.f.blocks[1].vtx.front().GetTxid(), 0);
    auto op = din::obj(); op["txid"] = point.txid.AsUint256().GetHex(); op["vout"] = point.vout;
    auto body = din::obj(); body["outpoints"] = din::arr(); body["outpoints"].append(op);
    auto request = din::arr(); request.append(body);
    const auto update = din::rpc_getproofupdates(ctx, request);
    ASSERT_FALSE(update.isMember("error")) << update.toStyledString();
    EXPECT_EQ(update["status"].asString(), "updated");
    ASSERT_EQ(update["proofs"].size(), 1u);
    ASSERT_TRUE(update["proofs"][0]["success"].asBool());
    auto envelope = op; envelope["proof"] = update["proofs"][0];
    envelope["utreexo_root"] = update["root_to"]; envelope["tip_hash"] = update["block_hash"];
    envelope["enforce_bound_context"] = true;
    auto verification = din::arr(); verification.append(envelope);
    const auto checked = rpc_context_wallet_verifyutxoproof(ctx, verification);
    ASSERT_FALSE(checked.isMember("error")) << checked.toStyledString();
    EXPECT_TRUE(checked["valid"].asBool()) << checked.toStyledString();
    EXPECT_EQ(checked["tip_hash"], update["block_hash"]);
    EXPECT_EQ(checked["tip_height"], update["height"]);
    EXPECT_EQ(checked["utreexo_root"], update["root_to"]);
    auto unchanged = din::arr(); auto root_only = din::obj(); root_only["root_from"] = update["root_to"]; unchanged.append(root_only);
    const auto same = din::rpc_getproofupdates(ctx, unchanged);
    EXPECT_EQ(same["status"].asString(), "no_update_needed") << same.toStyledString();
    EXPECT_EQ(same["block_hash"], update["block_hash"]);
    EXPECT_EQ(same["stump_roots"], update["stump_roots"]);
    verification[0]["expected_tip_hash"] = std::string(64, '0');
    const auto wrong_tip = rpc_context_wallet_verifyutxoproof(ctx, verification);
    EXPECT_FALSE(wrong_tip["valid"].asBool());
    EXPECT_EQ(wrong_tip["error_code"].asString(), "tip-hash-mismatch");
    const auto tip = f.f.db.getValidatedTip(); ASSERT_TRUE(tip.ok());
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height - 1), Status::Ok);
    for (const auto& query : {request, unchanged}) {
        const auto bad = din::rpc_getproofupdates(ctx, query);
        EXPECT_TRUE(bad.isMember("error"));
        for (const auto* key : {"root_to", "block_hash", "height", "proofs", "stump_roots"}) EXPECT_FALSE(bad.isMember(key));
    }
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height), Status::Ok);
    EXPECT_FALSE(din::rpc_getproofupdates(ctx, request).isMember("error"));
}
TEST(UtreexoProofInputCapture, VerificationRejectsMalformedSiblingExactlyOnceInOrder) {
    CanonicalRecoveryFixture f; ExecutionContext ctx; ctx.daemon = &f.context;
    const OutPoint point(f.f.blocks[1].vtx.front().GetTxid(), 0);
    auto query = din::arr(); query.append(point.txid.AsUint256().GetHex()); query.append(point.vout);
    const auto proof = din::rpc_getutxoproof(ctx, query);
    ASSERT_FALSE(proof.isMember("error")) << proof.toStyledString();
    auto valid = din::obj(); valid["txid"] = query[0]; valid["vout"] = query[1]; valid["proof"] = proof;
    for (const auto& sibling : {din::Json(std::string("ag") + std::string(62, '0')), din::Json(7), din::Json(std::string(63, '0'))}) {
        auto bad = valid; bad["proof"]["siblings"] = din::arr(); bad["proof"]["siblings"].append(sibling);
        auto missing = valid; missing["txid"] = std::string(64, '0');
        auto entries = din::arr(); entries.append(valid); entries.append(bad); entries.append(missing); entries.append(valid);
        auto request = din::arr(); request.append(entries);
        const auto response = din::rpc_verifyutxoproofs_batch(ctx, request);
        ASSERT_FALSE(response.isMember("error")) << response.toStyledString();
        ASSERT_EQ(response["results"].size(), 4u);
        EXPECT_EQ(response["valid"].asUInt64(), 2u); EXPECT_EQ(response["invalid"].asUInt64(), 2u);
        EXPECT_TRUE(response["results"][0]["valid"].asBool());
        EXPECT_EQ(response["results"][1]["error_code"].asString(), "invalid-sibling-hash");
        EXPECT_EQ(response["results"][2]["error_code"].asString(), "utxo-not-found");
        EXPECT_TRUE(response["results"][3]["valid"].asBool());
        EXPECT_EQ(response["block_hash"], proof["block_hash"]);
        EXPECT_EQ(response["height"], proof["height"]);
        EXPECT_EQ(response["utreexo_root"], proof["accumulator_root"]);
    }
}
TEST(UtreexoProofInputCapture, WalletBundleAndSingleUseCanonicalFundingMetadata) {
    // Existing fixture mines a genuinely signed donation and explicitly enrolls
    // the observed wallet row. This is not notification/discovery qualification.
    SharedPaymentFixture f; f.PrepareRpc();
    auto query = din::arr(); query.append(f.funding.txid.GetHex()); query.append(f.funding.vout);
    const auto single = rpc_context_wallet_utxoproof(f.execution, query);
    ASSERT_FALSE(single.isMember("error")) << single.toStyledString();
    EXPECT_EQ(single["amount_una"].asInt64(), f.funding.value.GetUna());
    EXPECT_EQ(single["height"].asUInt64(), f.funding.height);
    EXPECT_EQ(single["tip_hash"], single["utreexo_proof"]["block_hash"]);
    EXPECT_EQ(single["tip_height"], single["utreexo_proof"]["height"]);
    EXPECT_EQ(single["utreexo_root"], single["utreexo_proof"]["accumulator_root"]);
    const auto bundle = rpc_context_wallet_getproofbundle(f.execution, din::arr());
    ASSERT_FALSE(bundle.isMember("error")) << bundle.toStyledString();
    ASSERT_EQ(bundle["proofs"].size(), 1u) << bundle.toStyledString();
    EXPECT_EQ(bundle["proofs"][0]["amount_una"], single["amount_una"]);
    EXPECT_EQ(bundle["proofs"][0]["created_height"], single["height"]);
    EXPECT_EQ(bundle["block_hash"], single["tip_hash"]);
    EXPECT_EQ(bundle["accumulator_root"], single["utreexo_root"]);
    const auto tip = f.f.db.getValidatedTip(); ASSERT_TRUE(tip.ok());
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height - 1), Status::Ok);
    const auto bad = rpc_context_wallet_utxoproof(f.execution, query);
    EXPECT_TRUE(bad.isMember("error"));
    for (const auto* key : {"amount", "amount_una", "height", "tip_hash", "utreexo_root", "utreexo_proof"}) EXPECT_FALSE(bad.isMember(key));
    const auto bad_bundle = rpc_context_wallet_getproofbundle(f.execution, din::arr());
    EXPECT_TRUE(bad_bundle.isMember("error")); EXPECT_FALSE(bad_bundle.isMember("proofs"));
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height), Status::Ok);
}

TEST(UtreexoProofInputCapture, ExactFrozenBaseFallbackAndPromotedIdentity) {
    CanonicalRecoveryFixture f; ExecutionContext ctx; ctx.daemon = &f.context;
    const OutPoint point(f.f.blocks[1].vtx.front().GetTxid(), 0);
    const auto coin = f.f.db.getCoin(point.txid.AsUint256(), point.vout); ASSERT_TRUE(coin.ok());
    const auto snapshot = f.f.service->getUtreexoRpcSnapshot(); ASSERT_TRUE(snapshot.ok());
    auto query = din::arr(); query.append(point.txid.AsUint256().GetHex()); query.append(point.vout);
    const auto canonical = din::rpc_getutxoproof(ctx, query); ASSERT_FALSE(canonical.isMember("error"));
    // Retain the exact genuinely validated coin in the frozen store, keeping
    // the selected header/forest intact. Only base enrollment uses test access.
    ASSERT_EQ(f.f.db.replacePreBaseCoins(f.f.token, snapshot->block_hash, snapshot->height,
        {{point.txid.AsUint256(), point.vout, *coin}}), Status::Ok);
    ASSERT_EQ(f.f.db.deleteCoin(f.f.token, point.txid.AsUint256(), point.vout), Status::Ok);
    UtreexoProofCaptureTestAccess::ActiveBase(*f.f.service, snapshot->block_hash, snapshot->height);
    const auto check = [&] {
        const auto capture = f.f.service->CaptureUtreexoProofInputs(std::vector<OutPoint>{point});
        ASSERT_TRUE(capture.ok()); ASSERT_EQ(capture->inputs.front().status, Status::Ok);
        EXPECT_FALSE(capture->inputs.front().canonical);
        const auto proof = din::rpc_getutxoproof(ctx, query);
        ASSERT_FALSE(proof.isMember("error")) << proof.toStyledString();
        for (const auto* field : {"leaf_hash", "siblings", "position", "num_leaves", "amount_una",
                "created_height", "coinbase", "accumulator_root", "block_hash", "height"})
            EXPECT_EQ(proof[field], canonical[field]) << field;
    };
    ASSERT_NO_FATAL_FAILURE(check());
    UtreexoProofCaptureTestAccess::PromotedBase(*f.f.service, snapshot->height);
    ASSERT_NO_FATAL_FAILURE(check());
    auto wrong = snapshot->block_hash; wrong.data[0] ^= 1;
    ASSERT_EQ(f.f.db.replacePreBaseCoins(f.f.token, wrong, snapshot->height,
        {{point.txid.AsUint256(), point.vout, *coin}}), Status::Ok);
    auto bad = f.f.service->CaptureUtreexoProofInputs(std::vector<OutPoint>{point});
    ASSERT_TRUE(bad.ok()); EXPECT_EQ(bad->inputs.front().status, Status::Corruption);
    EXPECT_TRUE(din::rpc_getutxoproof(ctx, query).isMember("error"));
    auto malformed = *coin; malformed.script_pubkey = "invalid-hex";
    ASSERT_EQ(f.f.db.replacePreBaseCoins(f.f.token, snapshot->block_hash, snapshot->height,
        {{point.txid.AsUint256(), point.vout, malformed}}), Status::Ok);
    bad = f.f.service->CaptureUtreexoProofInputs(std::vector<OutPoint>{point});
    ASSERT_TRUE(bad.ok()); EXPECT_EQ(bad->inputs.front().status, Status::Corruption);
    ASSERT_EQ(f.f.db.replacePreBaseCoins(f.f.token, snapshot->block_hash, snapshot->height,
        {{point.txid.AsUint256(), point.vout, *coin}}), Status::Ok);
    ASSERT_NO_FATAL_FAILURE(check());
    EXPECT_FALSE(f.f.service->IsInSafeMode());
    ASSERT_EQ(f.f.db.putCoin(f.f.token, point.txid.AsUint256(), point.vout, *coin), Status::Ok);
}
TEST(UtreexoProofInputCapture, CanonicalCoverageFailureRetainsSafetyNotification) {
    CanonicalRecoveryFixture f; ExecutionContext ctx; ctx.daemon = &f.context;
    ASSERT_FALSE(daemon::kAutomaticChainstateRecoveryArmed);
    // No mining service or workers: serialized coverage classification only.
    ASSERT_FALSE(DaemonContext::instance() && DaemonContext::instance()->mining);
    const OutPoint original(f.f.blocks[1].vtx.front().GetTxid(), 0);
    const auto coin = f.f.db.getCoin(original.txid.AsUint256(), original.vout); ASSERT_TRUE(coin.ok());
    const OutPoint missing(original.txid, 999);
    ASSERT_EQ(f.f.db.putCoin(f.f.token, missing.txid.AsUint256(), missing.vout, *coin), Status::Ok);
    auto query = din::arr(); query.append(missing.txid.AsUint256().GetHex()); query.append(missing.vout);
    const auto response = din::rpc_getutxoproof(ctx, query);
    EXPECT_TRUE(response.isMember("error")); EXPECT_TRUE(f.f.service->IsInSafeMode());
    EXPECT_NE(f.f.service->GetSafeModeReason().find("missing from Utreexo forest"), std::string::npos);
    for (const auto* field : {"accumulator_root", "block_hash", "height", "siblings", "leaf_hash"}) EXPECT_FALSE(response.isMember(field));
    ASSERT_EQ(f.f.db.deleteCoin(f.f.token, missing.txid.AsUint256(), missing.vout), Status::Ok);
}
TEST(UtreexoProofInputCapture, CompactStumpDoesNotPretendToSupplyFullForestProofs) {
    HistoricalPreparationFixture f; ExecutionContext ctx; ctx.daemon = &f.context;
    const auto snapshot = f.service->getUtreexoRpcSnapshot(); ASSERT_TRUE(snapshot.ok()); ASSERT_TRUE(snapshot->compact);
    auto entries = din::arr(); auto query = din::arr(); query.append(entries);
    for (const auto& response : {din::rpc_getutxoproofs_batch(ctx, query), din::rpc_verifyutxoproofs_batch(ctx, query)}) {
        EXPECT_TRUE(response.isMember("error"));
        for (const auto* field : {"utreexo_root", "block_hash", "height", "proofs", "results"}) EXPECT_FALSE(response.isMember(field));
    }
    auto request = din::obj(); request["root_from"] = util::hex(snapshot->commitment);
    query = din::arr(); query.append(request);
    EXPECT_TRUE(din::rpc_getproofupdates(ctx, query).isMember("error"));
    // Read-only commitment availability is a separate, supported compact path.
    EXPECT_TRUE(f.service->getUtreexoRpcSnapshot().ok());
}
TEST(UtreexoProofInputCapture, BalanceDiagnosticUsesCanonicalAmountsAndCompleteTuple) {
    SharedPaymentFixture f; f.PrepareRpc();
    const auto before = f.Snapshot();
    const auto good = rpc_context_wallet_validatestatelessbalance(f.execution, din::arr());
    ASSERT_FALSE(good.isMember("error")) << good.toStyledString();
    EXPECT_TRUE(good["pilot_pass"].asBool()) << good.toStyledString();
    EXPECT_TRUE(good["context"]["selected_tuple_match"].asBool());
    EXPECT_EQ(good["balances"]["verified_una"].asUInt64(), uint64_t(f.funding.value.GetUna()));
    EXPECT_EQ(f.Snapshot(), before);
    // Corrupt only the diagnostic wallet amount, never the authoritative coin.
    f.Sql("UPDATE utxos SET amount=amount+7 WHERE is_spent=0");
    const auto changed = f.Snapshot();
    const auto mismatch = rpc_context_wallet_validatestatelessbalance(f.execution, din::arr());
    ASSERT_FALSE(mismatch.isMember("error")) << mismatch.toStyledString();
    EXPECT_FALSE(mismatch["pilot_pass"].asBool());
    EXPECT_FALSE(mismatch["summary"]["canonical_metadata_match"].asBool());
    EXPECT_EQ(mismatch["balances"]["verified_una"], good["balances"]["verified_una"]);
    EXPECT_EQ(mismatch["balances"]["delta_una"].asUInt64(), 7u);
    EXPECT_EQ(f.Snapshot(), changed);
    const auto tip = f.f.db.getValidatedTip(); ASSERT_TRUE(tip.ok());
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height-1), Status::Ok);
    const auto unavailable = rpc_context_wallet_validatestatelessbalance(f.execution, din::arr());
    EXPECT_TRUE(unavailable.isMember("error"));
    for (const auto* field : {"pilot_pass", "balances", "tip_hash", "tip_height", "utreexo_root"}) EXPECT_FALSE(unavailable.isMember(field));
    ASSERT_EQ(f.f.db.setValidatedTip(f.f.token, tip->hash, tip->height), Status::Ok);
}
#endif
}
