// Canonical full-forest versus roots-only transition characterization.
// Canonical full forest is the independent oracle. No network or concurrency.
#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_stump.h"
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
    EXPECT_EQ(stump.serialize(), saved_parent);
    stump = *next;
    EXPECT_EQ(stump.getNumLeaves(), expected.getNumLeaves());
    EXPECT_EQ(stump.getRoots(), expected.getRoots());
    EXPECT_EQ(stump.getCommitment(), expected.getCommitment());
    EXPECT_EQ(forest.dumpInternalState(), original);
}
}
TEST(OrchardCompactStumpCharacterization, EverySmallDeletionSubset) {
    for (uint64_t n = 1; n <= 8; ++n) {
        for (uint64_t mask = 0; mask < (uint64_t(1) << n); ++mask) {
            std::vector<uint64_t> deleted;
            for (uint64_t i = 0; i < n; ++i) if ((mask >> i) & 1) deleted.push_back(i);
            SCOPED_TRACE(::testing::Message() << "leaves=" << n << " mask=" << mask);
            CompareCanonicalDeletion(n, deleted, 0, false);
        }
    }
}
TEST(OrchardCompactStumpCharacterization, DeletedSubtreesThenCarryAdditions) {
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
} // namespace dinero::consensus
