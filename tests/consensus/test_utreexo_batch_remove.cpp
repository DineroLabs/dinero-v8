#include "consensus/utreexo_accumulator.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using dinero::consensus::UtreexoForest;
using dinero::consensus::UtreexoHash;

UtreexoHash Leaf(uint64_t value) {
  UtreexoHash hash(32);
  for (size_t i = 0; i < hash.size(); ++i) {
    hash[i] = static_cast<uint8_t>((value >> ((i % 8) * 8)) ^ (i * 29));
  }
  return hash;
}

TEST(UtreexoBatchRemove, MatchesSequentialTrustedRemovalExactly) {
  UtreexoForest original;
  original.setCanonicalEmptyRoots(true);
  for (uint64_t i = 0; i < 4096; ++i) {
    EXPECT_NE(original.add(Leaf(i)), UINT64_MAX);
  }

  const std::vector<uint64_t> positions{0, 1, 17, 511, 2048, 4095};
  std::vector<std::pair<uint64_t, UtreexoHash>> removals;
  for (const uint64_t position : positions) {
    removals.emplace_back(position, Leaf(position));
  }

  UtreexoForest sequential = original.clone();
  for (const auto &[position, hash] : removals) {
    EXPECT_TRUE(sequential.removeAtKnownPosition(position, hash));
  }

  UtreexoForest batched = original.clone();
  EXPECT_TRUE(batched.removeAtKnownPositions(removals));
  EXPECT_EQ(batched.dumpInternalState(), sequential.dumpInternalState());
}

TEST(UtreexoBatchRemove, InvalidRequestIsAtomic) {
  UtreexoForest forest;
  forest.setCanonicalEmptyRoots(true);
  for (uint64_t i = 0; i < 64; ++i) {
    EXPECT_NE(forest.add(Leaf(i)), UINT64_MAX);
  }
  const std::string before = forest.dumpInternalState();
  EXPECT_FALSE(forest.removeAtKnownPositions({{4, Leaf(4)}, {9, Leaf(10)}}));
  EXPECT_EQ(forest.dumpInternalState(), before);
}

TEST(UtreexoBatchRemove, MainnetScaleCompletesWithinTemplateBudget) {
  constexpr uint64_t kLeaves = 281365;
  UtreexoForest forest;
  forest.setCanonicalEmptyRoots(true);
  for (uint64_t i = 0; i < kLeaves; ++i) {
    EXPECT_NE(forest.add(Leaf(i)), UINT64_MAX);
  }

  std::vector<std::pair<uint64_t, UtreexoHash>> removals;
  for (uint64_t i = 0; i < 32; ++i) {
    const uint64_t position = (i * 7919) % kLeaves;
    removals.emplace_back(position, Leaf(position));
  }

  const auto start = std::chrono::steady_clock::now();
  EXPECT_TRUE(forest.removeAtKnownPositions(removals));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, std::chrono::seconds(10));
}

TEST(UtreexoBatchProof, CachedGenerationMatchesIndividualProofs) {
  UtreexoForest forest;
  forest.setCanonicalEmptyRoots(true);
  for (uint64_t i = 0; i < 4096; ++i) {
    EXPECT_NE(forest.add(Leaf(i)), UINT64_MAX);
  }

  const std::vector<uint64_t> positions{0, 17, 511, 2048, 4095};
  std::vector<UtreexoHash> targets;
  std::vector<UtreexoHash> expected_hashes;
  for (const uint64_t position : positions) {
    targets.push_back(Leaf(position));
    const auto proof = forest.prove(position);
    ASSERT_TRUE(proof.has_value());
    expected_hashes.insert(expected_hashes.end(), proof->siblings.begin(),
                           proof->siblings.end());
  }

  const auto batch = forest.generateBlockProof(targets);
  EXPECT_EQ(batch.targets, targets);
  EXPECT_EQ(batch.positions, positions);
  EXPECT_EQ(batch.proof_hashes, expected_hashes);
}

TEST(UtreexoBatchProof, MainnetScaleCompletesWithinTemplateBudget) {
  constexpr uint64_t kLeaves = 281365;
  UtreexoForest forest;
  forest.setCanonicalEmptyRoots(true);
  for (uint64_t i = 0; i < kLeaves; ++i) {
    EXPECT_NE(forest.add(Leaf(i)), UINT64_MAX);
  }

  std::vector<UtreexoHash> targets;
  for (uint64_t i = 0; i < 32; ++i) {
    targets.push_back(Leaf((i * 7919) % kLeaves));
  }

  const auto start = std::chrono::steady_clock::now();
  const auto proof = forest.generateBlockProof(targets);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(proof.targets, targets);
  EXPECT_LT(elapsed, std::chrono::seconds(10));
}

} // namespace
