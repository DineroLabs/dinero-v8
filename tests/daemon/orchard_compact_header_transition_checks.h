#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
// These three dynamic-parent headers use actual bounded proof search. They are
// deliberately independent of the deterministic capacity fixture nonce table.
BlockHeader CompactTransitionSolvedChild(const consensus::HeaderIndexEntry& parent,uint64_t salt) {
    auto h=parent.header;h.prev_block_hash=parent.hash;h.timestamp+=120;
    h.merkle_root=uint256{};
    for(size_t i=0;i<sizeof(salt);++i)h.merkle_root.begin()[i]=uint8_t(salt>>(8*i));
    h.merkle_root.begin()[31]=0xa7;
    h.difficulty=GetNextWorkRequiredForCandidate(parent.height+1,h.timestamp,
        GetConsensusForCurrentNetwork(),static_cast<const CBlockIndex*>(nullptr),
        &parent,static_cast<NoChainDb*>(nullptr));
    OrchardAdmissionFixture::Require(h.difficulty!=0);
    return OrchardAdmissionFixture::SolveHeader(h);
}
}
TEST(OrchardCompactHeaderTransition, AdvanceRollbackAndAbandonPreserveOwners) {
    CompactStartupFixture fixture;auto headers=CompactBindingSelector(fixture);
    const auto history=CompactBindingHeaders(fixture);
    const auto expected=CompactRetentionPathValues(*headers,history);
    auto current=consensus::HeaderChainSelector::RetainAncestry(headers,expected);ASSERT_TRUE(current);
    auto path=CompactRetentionLocalPath(history,history.size());
    const auto child=CompactTransitionSolvedChild(path.back(),710001);ASSERT_TRUE(headers->AddHeader(child));
    const auto next=headers->GetHeaderValue(child.GetHash());ASSERT_TRUE(next);
    auto abandoned=consensus::HeaderChainSelector::RetainAdjacent(*current,expected.back(),*next);
    ASSERT_TRUE(abandoned);abandoned.reset();
    EXPECT_THROW(headers->Clear(),std::exception);EXPECT_TRUE(headers->MatchesAncestry(expected));
    auto advanced=consensus::HeaderChainSelector::RetainAdjacent(*current,expected.back(),*next);
    ASSERT_TRUE(advanced);current.reset();EXPECT_THROW(headers->Clear(),std::exception);
    auto rolled=consensus::HeaderChainSelector::RetainAdjacent(*advanced,*next,expected.back());
    ASSERT_TRUE(rolled);advanced.reset();EXPECT_THROW(headers->Clear(),std::exception);
    EXPECT_TRUE(headers->MatchesAncestry(expected));
    rolled.reset();EXPECT_NO_THROW(headers->Clear());EXPECT_EQ(headers->GetHeaderCount(),0u);
}
TEST(OrchardCompactHeaderTransition, WrongValuesAndNonAdjacentHeadersRefuse) {
    CompactStartupFixture fixture;auto headers=CompactBindingSelector(fixture);
    const auto history=CompactBindingHeaders(fixture);
    const auto expected=CompactRetentionPathValues(*headers,history);
    auto current=consensus::HeaderChainSelector::RetainAncestry(headers,expected);ASSERT_TRUE(current);
    auto path=CompactRetentionLocalPath(history,history.size());
    const auto child=CompactTransitionSolvedChild(path.back(),720001);
    consensus::HeaderIndexEntry absent(child,&path.back());absent.parent=nullptr;
    EXPECT_FALSE(consensus::HeaderChainSelector::RetainAdjacent(*current,expected.back(),absent));
    ASSERT_TRUE(headers->AddHeader(child));const auto next=headers->GetHeaderValue(child.GetHash());ASSERT_TRUE(next);
    auto wrong=expected.back();wrong.chainwork+=arith_uint256(1);
    EXPECT_FALSE(consensus::HeaderChainSelector::RetainAdjacent(*current,wrong,*next));
    wrong=*next;wrong.chainwork+=arith_uint256(1);
    EXPECT_FALSE(consensus::HeaderChainSelector::RetainAdjacent(*current,expected.back(),wrong));
    EXPECT_FALSE(consensus::HeaderChainSelector::RetainAdjacent(*current,expected.back(),expected.back()));
    EXPECT_FALSE(consensus::HeaderChainSelector::RetainAdjacent(*current,expected.back(),expected[expected.size()-3]));
    path.emplace_back(child,&path.back());const auto grandchild=CompactTransitionSolvedChild(path.back(),720002);
    ASSERT_TRUE(headers->AddHeader(grandchild));const auto skipped=headers->GetHeaderValue(grandchild.GetHash());ASSERT_TRUE(skipped);
    EXPECT_FALSE(consensus::HeaderChainSelector::RetainAdjacent(*current,expected.back(),*skipped));
    // A valid edge does not authorize a transition from a different pinned tip.
    EXPECT_FALSE(consensus::HeaderChainSelector::RetainAdjacent(*current,*next,*skipped));
    EXPECT_THROW(headers->Clear(),std::exception);EXPECT_TRUE(headers->MatchesAncestry(expected));
    current.reset();EXPECT_NO_THROW(headers->Clear());EXPECT_EQ(headers->GetHeaderCount(),0u);
}
#endif
