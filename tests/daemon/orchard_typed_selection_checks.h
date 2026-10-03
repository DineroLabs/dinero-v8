#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
Transaction SelectionSpend(const OrchardAdmissionFixture& fixture,
    const OutPoint& point, const consensus::UTXOEntry& coin, uint64_t fee) {
    Transaction tx;
    tx.version = 2;
    tx.vin.emplace_back();
    tx.vin.back().prevout.txid = point.txid;
    tx.vin.back().prevout.vout = point.vout;
    tx.vin.back().sequence = 0xfffffffd;
    tx.vout.emplace_back(AmountUna::Una(coin.value.GetUna() - fee), fixture.script);
    const auto digest = consensus::ScriptVerifier::ComputeTaprootSighash(
        tx, 0, {coin.value.GetUna()}, {coin.scriptPubKey});
    OrchardAdmissionFixture::Require(digest.size() == 32);
    std::unique_ptr<secp256k1_context, decltype(&secp256k1_context_destroy)> context(
        secp256k1_context_create(SECP256K1_CONTEXT_NONE), secp256k1_context_destroy);
    secp256k1_keypair pair;
    OrchardAdmissionFixture::Require(secp256k1_keypair_create(context.get(), &pair, fixture.secret.data()));
    std::vector<uint8_t> signature(64);
    std::array<uint8_t,32> auxiliary{};
    OrchardAdmissionFixture::Require(secp256k1_schnorrsig_sign32(context.get(), signature.data(),
        digest.data(), &pair, auxiliary.data()));
    tx.vin.back().witness = {signature};
    return tx;
}
consensus::OrchardTransactionContext SelectionContext(const OrchardAdmissionFixture& fixture) {
    consensus::OrchardTransactionContext result;
    result.height = 102;
    result.parent_hash = fixture.tip.hash;
    result.activation_height = 102;
    result.domain.network_code = 2;
    result.domain.branch_id = Params().orchard_branch_id;
    const auto genesis = uint256::FromHexUnsafe(Params().genesis_hash);
    std::copy(genesis.begin(), genesis.end(), result.domain.genesis_wire.begin());
    return result;
}
}
TEST(OrchardTypedSelection, RealShieldSnapshotAndBudgets) {
    OrchardAdmissionFixture fixture;
    auto& pool = fixture.ingress->mempool();
    const auto first = fixture.Shield(1);
    const auto second = fixture.Shield(2);
    ASSERT_TRUE(fixture.ingress->SubmitBody(first, TxOrigin::INTERNAL).accepted());
    ASSERT_TRUE(fixture.ingress->SubmitBody(second, TxOrigin::INTERNAL).accepted());
    const auto selected = pool.CaptureTypedBlockSelection(1000000, 4000000, 102);
    ASSERT_TRUE(selected.available);
    EXPECT_EQ(selected.parent_hash, fixture.tip.hash);
    EXPECT_EQ(selected.parent_height, 101u);
    ASSERT_EQ(selected.transactions.size(), 2u);
    std::map<uint256,std::vector<uint8_t>> expected{
        {first.GetTxid().AsUint256(),first.Serialize()}, {second.GetTxid().AsUint256(),second.Serialize()}};
    for (const auto& body : selected.transactions) {
        const auto id = body.GetTxid().AsUint256();
        EXPECT_EQ(body.Serialize(), expected.at(id));
        EXPECT_EQ(selected.metadata.at(id).fee, 100000u);
    }
    const auto one = pool.CaptureTypedBlockSelection(
        std::max(first.GetSize(), second.GetSize()), std::max(first.GetWeight(), second.GetWeight()), 102);
    ASSERT_TRUE(one.available);
    EXPECT_EQ(one.transactions.size(), 1u);
    const auto none = pool.CaptureTypedBlockSelection(0, 0, 102);
    ASSERT_TRUE(none.available);
    EXPECT_TRUE(none.transactions.empty());
    // The explicitly historical compatibility API cannot convert Orchard.
    const auto historical = pool.CaptureBlockSelection(1000000, 4000000, 102);
    ASSERT_TRUE(historical.available);
    EXPECT_TRUE(historical.transactions.empty());
    EXPECT_EQ(pool.size(), 2u);
    fixture.CheckUnpublished();
}
TEST(OrchardTypedSelection, MixedParentFirstAndExactFees) {
    OrchardAdmissionFixture fixture;
    auto& pool = fixture.ingress->mempool();
    const auto shield = fixture.Shield(1);
    ASSERT_TRUE(fixture.ingress->SubmitBody(shield, TxOrigin::INTERNAL).accepted());
    const OutPoint point(fixture.blocks[2].vtx.front().GetTxid(), 0);
    const auto parent = SelectionSpend(fixture, point, fixture.replay->ProvenUtxos().at(point), 100000);
    const auto parent_body = MempoolTransaction(parent);
    const auto child = SelectionSpend(fixture, OutPoint(parent.GetTxid(), 0), parent_body.OutputCoin(0, 102), 200000);
    const auto admitted_parent = fixture.ingress->SubmitBody(parent_body, TxOrigin::INTERNAL);
    ASSERT_TRUE(admitted_parent.accepted()) << admitted_parent.message;
    const auto admitted_child = fixture.ingress->SubmitBody(MempoolTransaction(child), TxOrigin::INTERNAL);
    ASSERT_TRUE(admitted_child.accepted()) << admitted_child.message;
    const auto selected = pool.CaptureTypedBlockSelection(1000000, 4000000, 102);
    ASSERT_TRUE(selected.available);
    ASSERT_EQ(selected.transactions.size(), 3u);
    std::set<TxId> seen;
    uint64_t fees = 0;
    for (const auto& body : selected.transactions) {
        if (body.GetTxid() == child.GetTxid()) EXPECT_TRUE(seen.contains(parent.GetTxid()));
        EXPECT_TRUE(seen.insert(body.GetTxid()).second);
        fees += selected.metadata.at(body.GetTxid().AsUint256()).fee;
    }
    EXPECT_TRUE(seen.contains(shield.GetTxid()));
    EXPECT_EQ(fees, 400000u);
    EXPECT_EQ(pool.size(), 3u);
    fixture.CheckUnpublished();
}
TEST(OrchardTypedSelection, SelectedOwnerRefusalPreservesPool) {
    OrchardAdmissionFixture fixture;
    auto& pool = fixture.ingress->mempool();
    const auto body = fixture.Shield();
    ASSERT_TRUE(fixture.ingress->SubmitBody(body, TxOrigin::INTERNAL).accepted());
    EXPECT_FALSE(pool.CaptureTypedBlockSelection(1000000, 4000000, 101).available);
    fixture.service->setRuntimeBlockNotifications(nullptr);
    EXPECT_FALSE(pool.CaptureTypedBlockSelection(1000000, 4000000, 102).available);
    fixture.service->setRuntimeBlockNotifications(std::make_shared<OrchardAdmissionFixture::AdmissionNotifications>());
    ASSERT_EQ(fixture.db.setValidatedTip(fixture.token, fixture.blocks[100].GetHash(), 100), Status::Ok);
    EXPECT_FALSE(pool.CaptureTypedBlockSelection(1000000, 4000000, 102).available);
    ASSERT_EQ(fixture.db.setValidatedTip(fixture.token, fixture.tip.hash, 101), Status::Ok);
    const auto selected = pool.CaptureTypedBlockSelection(1000000, 4000000, 102);
    ASSERT_TRUE(selected.available);
    ASSERT_EQ(selected.transactions.size(), 1u);
    EXPECT_EQ(selected.transactions.front().Serialize(), body.Serialize());
    EXPECT_EQ(pool.size(), 1u);
    EXPECT_TRUE(pool.isOutputSpentInMempool(body.Inputs().front()));
    fixture.CheckUnpublished();
}
TEST(OrchardTypedSelection, SharedSequenceResourcesAndCoinbaseRefusal) {
    OrchardAdmissionFixture fixture;
    const auto body = fixture.Shield();
    const auto parsed = ParsedTransaction::DecodeExact(body.Serialize(), TransactionReadMode::StagedOrchard);
    FirstBoundaryView view(*fixture.replay);
    const auto context = SelectionContext(fixture);
    const auto checked = consensus::CheckOrchardTransactionCoinsUnderChainstateLock({&parsed,1}, context, view, {});
    EXPECT_EQ(checked.TotalFees(), 100000u);
    EXPECT_EQ(checked.Resources().bundles, 1u);
    EXPECT_EQ(checked.Authorizations().size(), 1u);
    const auto coinbase = ParsedTransaction::DecodeExact(fixture.blocks[1].vtx.front().Serialize(), TransactionReadMode::StagedOrchard);
    EXPECT_THROW(consensus::CheckOrchardTransactionCoinsUnderChainstateLock({&coinbase,1}, context, view, {}), consensus::OrchardBlockCoinError);
    // Repetition is deliberately not a valid candidate; body resources must
    // refuse before duplicate/authorization work. No pool insertion is used.
    std::vector<ParsedTransaction> over_budget(9, parsed);
    try {
        (void)consensus::CheckOrchardTransactionCoinsUnderChainstateLock(over_budget, context, view, {});
        FAIL() << "block bundle budget not enforced";
    } catch (const consensus::OrchardResourceError& error) {
        EXPECT_EQ(error.Code(), consensus::OrchardResourceErrorCode::Bundles);
    }
    EXPECT_EQ(fixture.ingress->mempool().size(), 0u);
    fixture.CheckUnpublished();
}
#else
TEST(OrchardTypedSelection, DefaultValidatorUnavailable) {
    MempoolChainstateReadGuard owner;
    EXPECT_EQ(owner.ValidateBlockSelection({}, 1).result.code, TxRejectCode::UNAVAILABLE);
}
#endif
