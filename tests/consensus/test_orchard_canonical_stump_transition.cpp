// Canonical full-forest versus roots-only transition characterization.
// Canonical full forest is the independent oracle. No network or concurrency.
#include "consensus/utreexo_accumulator.h"
#include "consensus/orchard_canonical_stump_transition.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace dinero::consensus {
namespace {
UtreexoHash DifferentialLeaf(uint64_t id) {
    UtreexoHash a(32, 0), b(32, 0);
    for (unsigned i = 0; i < 8; ++i) a[i] = uint8_t(id >> (i * 8));
    b[0] = 0x73; b[1] = 0x74; b[2] = 0x75; b[3] = 0x6d;
    return HashNode(a, b);
}
void CompareCanonicalDeletion(uint64_t count, std::vector<uint64_t> deleted,
                              size_t added, bool reverse) {
    UtreexoForest forest;
    forest.setCanonicalEmptyRoots(true);
    std::vector<UtreexoHash> leaves;
    for (uint64_t i = 0; i < count; ++i) {
        leaves.push_back(DifferentialLeaf(i + 1));
        ASSERT_EQ(forest.add(leaves.back()), i);
    }
    auto stump = UtreexoStump::fromForest(forest);
    ASSERT_EQ(stump.getCommitment(), forest.getCommitment());
    if (reverse) std::reverse(deleted.begin(), deleted.end());
    BlockUtreexoProof proof;
    proof.numLeaves = count;
    std::vector<std::pair<uint64_t,UtreexoHash>> removals;
    for (const auto pos : deleted) {
        ASSERT_LT(pos, count);
        const auto path = forest.prove(pos);
        ASSERT_TRUE(path.has_value());
        proof.targets.push_back(leaves[pos]);
        proof.positions.push_back(pos);
        proof.proof_hashes.insert(proof.proof_hashes.end(),
            path->siblings.begin(), path->siblings.end());
        removals.emplace_back(pos, leaves[pos]);
    }
    const auto original = forest.dumpInternalState();
    ASSERT_TRUE(stump.verifyBlockProof(proof));
    ASSERT_EQ(forest.dumpInternalState(), original);
    auto expected = forest.clone();
    ASSERT_TRUE(expected.removeAtKnownPositions(removals));
    std::vector<UtreexoHash> additions;
    for (size_t i = 0; i < added; ++i) {
        additions.push_back(DifferentialLeaf(1000 + i));
        ASSERT_EQ(expected.add(additions.back()), count + i);
    }
    const auto saved_parent = stump.serialize();
    const auto next = OrchardCanonicalStumpTransition(stump, proof, additions, expected.getCommitment());
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->getNumLeaves(), expected.getNumLeaves());
    EXPECT_EQ(next->getRoots(), expected.getRoots());
    EXPECT_EQ(next->getCommitment(), expected.getCommitment());
    EXPECT_EQ(stump.serialize(), saved_parent);
    EXPECT_EQ(forest.dumpInternalState(), original);
}
}
TEST(OrchardCanonicalStumpTransition, EverySmallDeletionSubset) {
    for (uint64_t n = 1; n <= 8; ++n) {
        for (uint64_t mask = 0; mask < (uint64_t(1) << n); ++mask) {
            std::vector<uint64_t> deleted;
            for (uint64_t i = 0; i < n; ++i) if ((mask >> i) & 1) deleted.push_back(i);
            SCOPED_TRACE(::testing::Message() << "leaves=" << n << " mask=" << mask);
            CompareCanonicalDeletion(n, deleted, 0, false);
        }
    }
}
TEST(OrchardCanonicalStumpTransition, DeletedSubtreesThenCarryAdditions) {
    for (const uint64_t n : {1,2,3,4,7,8,13,16,17,31,32,33}) {
        std::vector<uint64_t> all, alternating, siblings;
        for (uint64_t i=0; i<n; ++i) {
            all.push_back(i);
            if (i%2==0) alternating.push_back(i);
            if (i<2 || (i>=4 && i<8)) siblings.push_back(i);
        }
        for (const auto& deleted : {all, alternating, siblings})
            for (const size_t additions : {size_t(1),size_t(3),size_t(8)})
                for (const bool reverse : {false,true}) {
                    SCOPED_TRACE(::testing::Message() << "leaves=" << n
                        << " deleted=" << deleted.size() << " added=" << additions
                        << " reverse=" << reverse);
                    CompareCanonicalDeletion(n, deleted, additions, reverse);
                }
    }
}

TEST(OrchardCanonicalStumpTransition, RejectsMalformedProofWithoutMutation) {
    UtreexoForest forest;
    forest.setCanonicalEmptyRoots(true);
    std::vector<UtreexoHash> leaves;
    for (uint64_t i=0;i<13;++i) {
        leaves.push_back(DifferentialLeaf(i+1)); forest.add(leaves.back());
    }
    const auto parent=UtreexoStump::fromForest(forest);
    const auto original=parent.serialize();
    BlockUtreexoProof proof;
    proof.numLeaves=13;
    proof.positions={1,7,12};
    for (auto pos:proof.positions) {
        auto path=forest.prove(pos); ASSERT_TRUE(path);
        proof.targets.push_back(leaves[pos]);
        proof.proof_hashes.insert(proof.proof_hashes.end(),path->siblings.begin(),path->siblings.end());
    }
    auto expected=forest.clone();
    ASSERT_TRUE(expected.removeAtKnownPositions({{1,leaves[1]},{7,leaves[7]},{12,leaves[12]}}));
    auto root=expected.getCommitment();
    ASSERT_TRUE(OrchardCanonicalStumpTransition(parent,proof,{},root));
    // Position 12 is the original height-zero tree: one independent path.
    // A wrong target must be rejected by the parent-root binding itself,
    // without another proof path masking its omission via overlap checks.
    {
        BlockUtreexoProof single; single.numLeaves=13;
        single.positions={12}; single.targets={leaves[12]};
        auto expected_single=forest.clone();
        ASSERT_TRUE(expected_single.removeAtKnownPositions({{12,leaves[12]}}));
        ASSERT_TRUE(OrchardCanonicalStumpTransition(parent,single,{},expected_single.getCommitment()));
        single.targets[0][0]^=1;
        EXPECT_FALSE(OrchardCanonicalStumpTransition(parent,single,{},expected_single.getCommitment()));
        EXPECT_EQ(parent.serialize(),original);
    }
    const auto refuse=[&](const BlockUtreexoProof& bad) {
        EXPECT_FALSE(OrchardCanonicalStumpTransition(parent,bad,{},root));
        EXPECT_EQ(parent.serialize(),original);
    };
    auto bad=proof; ++bad.numLeaves; refuse(bad);
    bad=proof; bad.positions.pop_back(); refuse(bad);
    bad=proof; bad.positions[0]=13; refuse(bad);
    bad=proof; bad.positions[1]=bad.positions[0]; refuse(bad);
    bad=proof; bad.targets[0].resize(31); refuse(bad);
    bad=proof; bad.targets[0][0]^=1; refuse(bad);
    bad=proof; bad.proof_hashes.pop_back(); refuse(bad);
    bad=proof; bad.proof_hashes.push_back(UtreexoHash(32,0)); refuse(bad);
    bad=proof; bad.proof_hashes[0].resize(31); refuse(bad);
    bad=proof; bad.proof_hashes[0][0]^=1; refuse(bad);
    root[0]^=1; EXPECT_FALSE(OrchardCanonicalStumpTransition(parent,proof,{},root));
    EXPECT_EQ(parent.serialize(),original);
    auto roots=parent.getAllRoots(); roots[0]=std::nullopt;
    auto missing=UtreexoStump::fromRoots(std::vector<std::optional<UtreexoHash>>(roots.begin(),roots.end()),13);
    EXPECT_FALSE(OrchardCanonicalStumpTransition(missing,proof,{},expected.getCommitment()));
    BlockUtreexoProof empty; empty.numLeaves=13;
    EXPECT_FALSE(OrchardCanonicalStumpTransition(parent,empty,{UtreexoHash(31,1)},root));
    EXPECT_FALSE(OrchardCanonicalStumpTransition(parent,empty,{UtreexoHash(32,0)},root));
    empty.proof_hashes.push_back(UtreexoHash(32,0)); refuse(empty);
    EXPECT_EQ(parent.serialize(),original);
}

TEST(OrchardCanonicalStumpTransition, SuccessiveDeletionsAndCapacityBoundary) {
    UtreexoForest forest; forest.setCanonicalEmptyRoots(true);
    for (uint64_t i=0;i<16;++i) forest.add(DifferentialLeaf(i+1));
    auto stump=UtreexoStump::fromForest(forest);
    for (const std::vector<uint64_t>& positions : {
            std::vector<uint64_t>{0,1,2,3}, {4,5,6,7}, {8,9,10,11,12,13,14,15}}) {
        BlockUtreexoProof proof; proof.numLeaves=forest.getNumLeaves();
        std::vector<std::pair<uint64_t,UtreexoHash>> removals;
        for (auto pos:positions) {
            auto path=forest.prove(pos); ASSERT_TRUE(path);
            auto leaf=DifferentialLeaf(pos+1);
            proof.positions.push_back(pos); proof.targets.push_back(leaf);
            proof.proof_hashes.insert(proof.proof_hashes.end(),path->siblings.begin(),path->siblings.end());
            removals.emplace_back(pos,leaf);
        }
        ASSERT_TRUE(forest.removeAtKnownPositions(removals));
        auto next=OrchardCanonicalStumpTransition(stump,proof,{},forest.getCommitment());
        ASSERT_TRUE(next); stump=*next;
        EXPECT_EQ(stump.getRoots(),forest.getRoots());
    }
    const std::vector<UtreexoHash> added{DifferentialLeaf(100),DifferentialLeaf(101)};
    BlockUtreexoProof empty; empty.numLeaves=forest.getNumLeaves();
    for (const auto& leaf:added) forest.add(leaf);
    auto next=OrchardCanonicalStumpTransition(stump,empty,added,forest.getCommitment());
    ASSERT_TRUE(next); EXPECT_EQ(next->getRoots(),forest.getRoots());
    // Shape-only synthetic boundary; no claim of authenticating this parent.
    std::vector<std::optional<UtreexoHash>> roots(41);
    roots[40]=DifferentialLeaf(999);
    const auto full=UtreexoStump::fromRoots(roots,MAX_UTREEXO_LEAVES);
    empty.numLeaves=MAX_UTREEXO_LEAVES;
    EXPECT_FALSE(OrchardCanonicalStumpTransition(full,empty,added,full.getCommitment()));
    EXPECT_TRUE(OrchardCanonicalStumpTransition(full,empty,{},full.getCommitment()));
}
TEST(OrchardCanonicalStumpTransition, RejectsDuplicateAdditionsEvenWithMatchingArithmetic) {
    UtreexoForest forest; forest.setCanonicalEmptyRoots(true);
    const auto parent=UtreexoStump::fromForest(forest);
    const auto leaf=DifferentialLeaf(123);
    const std::vector<UtreexoHash> duplicates{leaf,leaf};
    auto unvalidated=parent;
    unvalidated.add(duplicates);
    BlockUtreexoProof proof; proof.numLeaves=0;
    EXPECT_FALSE(OrchardCanonicalStumpTransition(parent,proof,duplicates,unvalidated.getCommitment()));
    EXPECT_EQ(parent.getNumLeaves(),0U);
    EXPECT_EQ(forest.add(leaf),0U);
    EXPECT_EQ(forest.add(leaf),UINT64_MAX);
    const std::vector<UtreexoHash> singleton{leaf};
    EXPECT_TRUE(OrchardCanonicalStumpTransition(parent,proof,singleton,forest.getCommitment()));
}
} // namespace dinero::consensus
