#pragma once
// Included after selected_parent_history_checks.h in the actual service fixture.
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
OrchardParentReplay::Target ParentReplayTarget(const SelectedParentFixture& f) {
    return {f.tip.height, f.tip.hash, ChainworkFromHex(f.tip.chainwork)};
}
void AppendParentReplay(OrchardParentReplay& replay, const std::vector<Block>& blocks) {
    arith_uint256 work{0};
    for (uint32_t height = 0; height < blocks.size(); ++height) {
        work += GetBlockProof(blocks[height].header.difficulty);
        replay.Append(blocks[height], height, work);
    }
}
constexpr OrchardParentReplay::Limits parent_replay_limits{100000, 256 * 1024 * 1024};
}

TEST(OrchardParentReplay, FullIndependentPrefixMatchesSelectedRetirement) {
    SelectedParentFixture f;
    const auto expected = f.Read(); ASSERT_TRUE(expected);
    OrchardParentReplay replay(ParentReplayTarget(f), parent_replay_limits);
    EXPECT_THROW(replay.Record(), std::runtime_error);
    EXPECT_THROW(replay.ProvenState(), std::runtime_error);
    AppendParentReplay(replay, f.blocks);
    EXPECT_THROW(replay.Accounting(), std::runtime_error);
    replay.Finish();
    EXPECT_EQ(replay.Record(), *expected);
    EXPECT_EQ(replay.ValidatedTarget().hash, f.tip.hash);
    EXPECT_EQ(replay.Accounting().blocks_read, 3u);
    EXPECT_EQ(replay.Accounting().shielded_transactions, 0u);
    EXPECT_EQ(replay.ProvenState().RecordsDigestHex(), f.replay->RecordsDigestHex());
    EXPECT_EQ(replay.ProvenState().UtreexoRootHex(), f.replay->UtreexoRootHex());
    f.CheckUnpublished();
}

TEST(OrchardParentReplay, CanonicalIndexCannotChooseDetachedBranch) {
    SelectedParentFixture f;
    const auto expected = f.Read(); ASSERT_TRUE(expected);
    // The immutable branch remains independently provable when the selected
    // index disagrees. The CURRENT selected consumer must still refuse it.
    ASSERT_EQ(f.db.putHeightIndex(f.token, 1, f.blocks.front().GetHash()), Status::Ok);
    EXPECT_FALSE(f.Read());
    OrchardParentReplay replay(ParentReplayTarget(f), parent_replay_limits);
    AppendParentReplay(replay, f.blocks); replay.Finish();
    EXPECT_EQ(replay.Record(), *expected);
    EXPECT_FALSE(f.Read());
    f.CheckUnpublished();
}

TEST(OrchardParentReplay, WrongWorkPermanentlyPoisonsOwner) {
    SelectedParentFixture f;
    OrchardParentReplay replay(ParentReplayTarget(f), parent_replay_limits);
    auto work = GetBlockProof(f.blocks.front().header.difficulty);
    replay.Append(f.blocks.front(), 0, work);
    const auto correct = work + GetBlockProof(f.blocks[1].header.difficulty);
    EXPECT_THROW(replay.Append(f.blocks[1], 1, correct + arith_uint256(1)), std::runtime_error);
    EXPECT_THROW(replay.Append(f.blocks[1], 1, correct), std::runtime_error);
    EXPECT_THROW(replay.Finish(), std::runtime_error);
    EXPECT_THROW(replay.ProvenState(), std::runtime_error);
    f.CheckUnpublished();
}

TEST(OrchardParentReplay, AlteredBodyAndIncompleteInputNeverExposeState) {
    SelectedParentFixture f;
    OrchardParentReplay replay(ParentReplayTarget(f), parent_replay_limits);
    auto work = GetBlockProof(f.blocks.front().header.difficulty);
    replay.Append(f.blocks.front(), 0, work);
    auto changed = f.blocks[1];
    changed.vtx.front().vout.front().value = AmountUna::Una(1);
    work += GetBlockProof(changed.header.difficulty);
    EXPECT_THROW(replay.Append(changed, 1, work), std::runtime_error);
    EXPECT_THROW(replay.Record(), std::runtime_error);
    EXPECT_THROW(replay.Append(f.blocks[1], 1, work), std::runtime_error);
    OrchardParentReplay incomplete(ParentReplayTarget(f), parent_replay_limits);
    incomplete.Append(f.blocks.front(), 0, GetBlockProof(f.blocks.front().header.difficulty));
    EXPECT_THROW(incomplete.Finish(), std::runtime_error);
    EXPECT_THROW(incomplete.Append(f.blocks[1], 1, work), std::runtime_error);
    EXPECT_THROW(incomplete.Accounting(), std::runtime_error);
    f.CheckUnpublished();
}

TEST(OrchardParentReplay, TargetDomainAndMaterialLimitsFailClosed) {
    SelectedParentFixture f;
    auto wrong = ParentReplayTarget(f); wrong.hash.SetNull();
    EXPECT_THROW((OrchardParentReplay(wrong, parent_replay_limits)), std::runtime_error);
    EXPECT_THROW((OrchardParentReplay(ParentReplayTarget(f), {3, 256 * 1024 * 1024})), std::runtime_error);
    OrchardParentReplay too_small(ParentReplayTarget(f), {4, 1});
    EXPECT_THROW(too_small.Append(f.blocks.front(), 0,
        GetBlockProof(f.blocks.front().header.difficulty)), std::runtime_error);
    EXPECT_THROW(too_small.Record(), std::runtime_error);
    OrchardParentReplay changed_domain(ParentReplayTarget(f), parent_replay_limits);
    AppendParentReplay(changed_domain, f.blocks);
    const auto branch = Params().orchard_branch_id;
    MutableParams().orchard_branch_id = branch + 1;
    EXPECT_THROW(changed_domain.Finish(), std::runtime_error);
    MutableParams().orchard_branch_id = branch;
    EXPECT_THROW(changed_domain.Finish(), std::runtime_error);
    EXPECT_THROW(changed_domain.Record(), std::runtime_error);
    f.CheckUnpublished();
}
#endif

#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardParentReplay, EmptyAndTruncatedBundleReadsPreserveSighashRules) {
    namespace shielded = consensus::shielded;
    shielded::ShieldedBundle decoded;
    EXPECT_EQ(shielded::DeserializeShieldedBundle(nullptr, 0, &decoded),
        shielded::BundleDecodeError::Truncated);
    std::vector<uint8_t> empty;
    EXPECT_EQ(shielded::DeserializeShieldedBundle(empty, &decoded),
        shielded::BundleDecodeError::Truncated);
    Transaction pending;
    pending.version = Transaction::TX_VERSION_SHIELDED_V2;
    EXPECT_EQ(shielded::ComputeShieldedTxSighash(pending),
        shielded::ComputeShieldedTxSighash(pending, shielded::ShieldedProofEncoding::Full));

    shielded::ShieldedBundle bundle;
    bundle.spends.resize(1);
    bundle.spends.front().zk_proof.resize(256, 0x12);
    bundle.outputs.resize(1);
    bundle.outputs.front().encrypted_note.resize(32, 0x34);
    bundle.outputs.front().zk_proof.resize(256, 0x56);
    const auto serialized = shielded::SerializeShieldedBundle(bundle);
    ASSERT_FALSE(serialized.empty());
    for (size_t length = 0; length < serialized.size(); ++length) {
        SCOPED_TRACE(length);
        const std::vector<uint8_t> prefix(serialized.begin(), serialized.begin()+length);
        shielded::ShieldedBundle partial;
        EXPECT_EQ(shielded::DeserializeShieldedBundle(prefix, &partial),
            shielded::BundleDecodeError::Truncated);
    }
    EXPECT_EQ(shielded::DeserializeShieldedBundle(serialized, &decoded),
        shielded::BundleDecodeError::Ok);
    EXPECT_EQ(shielded::SerializeShieldedBundle(decoded), serialized);
}
#endif
