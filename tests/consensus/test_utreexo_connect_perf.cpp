// ============================================================================
// UTREEXO CONNECT-PATH PERFORMANCE REGRESSION TESTS (block-92742 incident)
// ============================================================================
//
// 2026-08-21: connecting mainnet block 92742 (~1600 spent inputs) took
// ~7 minutes at ~380% CPU on every node. Root cause: the connect path
// re-proved and removed spent targets ONE AT A TIME —
// UtreexoTransitionProof::generate() (invoked twice per connect) called
// prove() + remove() per target, and each of those rehashes the entire
// containing subtree (computeSubtreeHash / recomputePath), so total cost
// was O(targets × forest-size) hashing instead of O(forest-size).
//
// These tests pin:
//   1. proveMany() — the shared-cache batch prover — is byte-identical
//      to per-position prove().
//   2. generate()'s batched PASS 1 produces exactly the state the old
//      sequential find→prove→remove simulation produced.
//   3. generate() on a 1600-target block completes in seconds, not
//      minutes (fails on the pre-fix per-target implementation).
//
// NOTE: failures gate via gtest (exit non-zero), never bare assert() —
// assert() is a no-op under NDEBUG and would not gate release CI.
// ============================================================================

#include "consensus/utreexo_accumulator.h"
#include "consensus/chainparams.h"
#include "consensus/subsidy.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace {

using dinero::Block;
using dinero::Transaction;
using dinero::TxId;
using dinero::TxInput;
using dinero::TxOutput;
using dinero::consensus::BlockUtreexoProof;
using dinero::consensus::UtreexoForest;
using dinero::consensus::UtreexoHash;
using dinero::consensus::UtreexoTransitionProof;

UtreexoHash Leaf(uint64_t value) {
  UtreexoHash hash(32);
  for (size_t i = 0; i < hash.size(); ++i) {
    hash[i] = static_cast<uint8_t>((value >> ((i % 8) * 8)) ^ (i * 29));
  }
  return hash;
}

std::vector<uint8_t> P2TRScript(uint8_t fill) {
  std::vector<uint8_t> script = {0x51, 0x20};
  script.resize(34, fill);
  return script;
}

// Minimal coinbase-only block: generate() only reads block.vtx through
// computeAdditionHashes(), so one coinbase with one output suffices.
Block CoinbaseOnlyBlock(uint32_t height) {
  Block block;
  block.header.version = 1;
  block.header.timestamp = 1772841600 + static_cast<uint64_t>(height) * 120;
  block.header.difficulty = 0x1d00ffff;
  block.header.nonce = 0;
  block.header.ZeroReserved();

  Transaction coinbase;
  coinbase.version = 1;
  coinbase.lockTime = 0;
  coinbase.witness_version = 1;
  TxInput input;
  input.prevout.txid = TxId();
  input.prevout.vout = 0xffffffff;
  input.scriptSig.push_back(static_cast<uint8_t>(height & 0xFF));
  input.scriptSig.push_back(static_cast<uint8_t>((height >> 8) & 0xFF));
  input.sequence = 0xffffffff;
  coinbase.vin.push_back(input);
  TxOutput output;
  output.value = dinero::ConsensusSubsidy::GetBlockSubsidy(height);
  output.scriptPubKey = P2TRScript(0x00);
  coinbase.vout.push_back(output);
  block.vtx.push_back(coinbase);
  return block;
}

UtreexoForest BuildForest(uint64_t leaves) {
  UtreexoForest forest;
  forest.setCanonicalEmptyRoots(true);
  for (uint64_t i = 0; i < leaves; ++i) {
    EXPECT_NE(forest.add(Leaf(i)), UINT64_MAX);
  }
  return forest;
}

// Spend proof shaped like BridgeNode::GenerateProofForBlock produces:
// positions resolved against the live forest, per-target sequential
// proof hashes.
BlockUtreexoProof SpendProofFor(const UtreexoForest& forest,
                                const std::vector<UtreexoHash>& targets) {
  BlockUtreexoProof proof;
  proof.numLeaves = forest.getNumLeaves();
  for (const auto& target : targets) {
    auto position = forest.findLeafPosition(target);
    EXPECT_TRUE(position.has_value());
    proof.targets.push_back(target);
    proof.positions.push_back(position.value_or(0));
  }
  return proof;
}

TEST(UtreexoConnectPerf, ProveManyMatchesProve) {
  UtreexoForest forest = BuildForest(4096);
  // Delete a couple of leaves so the deleted-position path is covered.
  ASSERT_TRUE(forest.removeAtKnownPosition(5, Leaf(5)));
  ASSERT_TRUE(forest.removeAtKnownPosition(100, Leaf(100)));

  const std::vector<uint64_t> positions{0, 1, 5, 17, 100, 511, 2048, 4095, 9999999};
  const auto batched = forest.proveMany(positions);
  ASSERT_EQ(batched.size(), positions.size());

  for (size_t i = 0; i < positions.size(); ++i) {
    const auto single = forest.prove(positions[i]);
    ASSERT_EQ(batched[i].has_value(), single.has_value())
        << "presence mismatch at index " << i << " (position " << positions[i] << ")";
    if (single.has_value()) {
      EXPECT_EQ(batched[i]->position, single->position);
      EXPECT_EQ(batched[i]->numLeaves, single->numLeaves);
      EXPECT_EQ(batched[i]->siblings, single->siblings)
          << "sibling mismatch at index " << i;
    }
  }
  // Deleted and out-of-range positions must be unprovable in both.
  EXPECT_FALSE(batched[2].has_value());  // position 5 (deleted)
  EXPECT_FALSE(batched[4].has_value());  // position 100 (deleted)
  EXPECT_FALSE(batched[8].has_value());  // out of range
}

TEST(UtreexoConnectPerf, GenerateMatchesSequentialProofRemovalSimulation) {
  const uint32_t height = 200;
  UtreexoForest forest = BuildForest(512);

  std::vector<UtreexoHash> targets;
  for (uint64_t i = 0; i < 512; i += 9) {
    targets.push_back(Leaf(i));
  }
  // Duplicate + unknown targets: generate() historically SKIPPED any
  // target it could not find/prove/remove, silently. Pin that.
  targets.push_back(Leaf(0));          // duplicate of an earlier target
  targets.push_back(Leaf(999999));     // never in the forest

  const Block block = CoinbaseOnlyBlock(height);
  const BlockUtreexoProof spend_proof = [&] {
    // Build the proof from the still-live targets only, then append the
    // duplicate/unknown entries with placeholder positions the way a
    // malformed proof would carry them.
    std::vector<UtreexoHash> live(targets.begin(), targets.end() - 2);
    BlockUtreexoProof p = SpendProofFor(forest, live);
    p.targets.push_back(Leaf(0));
    p.positions.push_back(0);
    p.targets.push_back(Leaf(999999));
    p.positions.push_back(0);
    return p;
  }();

  // Old-path simulation, byte-exact: per target find → prove → remove.
  UtreexoForest sequential = forest.clone();
  for (const auto& target : spend_proof.targets) {
    auto position = sequential.findLeafPosition(target);
    if (!position.has_value()) continue;
    auto proof = sequential.prove(position.value());
    if (!proof.has_value()) continue;
    sequential.remove(target, proof.value());
  }
  const auto expected_roots_after_deletions = sequential.getIndexedRoots();
  for (const auto& addition :
       UtreexoTransitionProof::computeAdditionHashes(block, height)) {
    ASSERT_NE(sequential.add(addition), UINT64_MAX);
  }
  const UtreexoHash expected_commitment = sequential.getCommitment();

  const auto tp =
      UtreexoTransitionProof::generate(forest, block, spend_proof, height);

  EXPECT_EQ(tp.roots_after_deletions, expected_roots_after_deletions);
  EXPECT_EQ(tp.commitment_after, expected_commitment);
  EXPECT_EQ(tp.num_leaves_before, static_cast<uint64_t>(512));
  EXPECT_EQ(tp.num_leaves_after,
            tp.num_leaves_before + tp.addition_hashes.size());
}

TEST(UtreexoConnectPerf, GenerateFastForManyTargets) {
  // The block-92742 shape, scaled to test size: many spent inputs on a
  // large forest. Pre-fix, generate() cost O(targets × forest) hashing
  // (~minutes at mainnet scale, tens of seconds here); post-fix it is
  // one batched removal + one root rebuild.
  const uint32_t height = 200;
  const uint64_t kLeaves = 65536;
  const size_t kTargets = 1600;

  UtreexoForest forest = BuildForest(kLeaves);

  std::vector<UtreexoHash> targets;
  targets.reserve(kTargets);
  for (size_t i = 0; i < kTargets; ++i) {
    targets.push_back(Leaf((i * 40) % kLeaves));
  }
  const Block block = CoinbaseOnlyBlock(height);
  const BlockUtreexoProof spend_proof = SpendProofFor(forest, targets);

  const auto start = std::chrono::steady_clock::now();
  const auto tp =
      UtreexoTransitionProof::generate(forest, block, spend_proof, height);
  const double elapsed_secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();

  // Correctness first: the transition must equal a batched simulation.
  UtreexoForest expected = forest.clone();
  std::vector<std::pair<uint64_t, UtreexoHash>> removals;
  for (const auto& target : targets) {
    auto position = expected.findLeafPosition(target);
    ASSERT_TRUE(position.has_value());
    removals.emplace_back(position.value(), target);
  }
  ASSERT_TRUE(expected.removeAtKnownPositions(removals));
  EXPECT_EQ(tp.roots_after_deletions, expected.getIndexedRoots());
  for (const auto& addition :
       UtreexoTransitionProof::computeAdditionHashes(block, height)) {
    ASSERT_NE(expected.add(addition), UINT64_MAX);
  }
  EXPECT_EQ(tp.commitment_after, expected.getCommitment());

  // The regression bound: minutes-scale per-target rehashing fails this.
  EXPECT_LT(elapsed_secs, 10.0)
      << "UtreexoTransitionProof::generate took " << elapsed_secs
      << "s for " << kTargets << " targets on a " << kLeaves
      << "-leaf forest — per-target O(forest) rehashing has regressed";
}

}  // namespace

// ConnectBlock hands its private forest to generate()'s rvalue overload rather
// than letting generate() clone it a second time (block_validation.cpp ~2102).
// Two full deep copies of a ~300k-leaf forest per block became one. That is a
// pure optimisation ONLY if adopting the forest yields the identical proof.
TEST(UtreexoConnectPerf, GenerateOverloadsAgreeByteForByte) {
  UtreexoForest forest = BuildForest(1024);
  const Block block = CoinbaseOnlyBlock(7);
  BlockUtreexoProof spend_proof;

  // Pre-existing path: caller keeps its forest, generate() clones internally.
  const auto from_const_ref =
      UtreexoTransitionProof::generate(forest, block, spend_proof, 7);

  // New path: caller owns a private forest and hands ownership over.
  UtreexoForest owned = forest.clone();
  const auto from_rvalue =
      UtreexoTransitionProof::generate(std::move(owned), block, spend_proof, 7);

  EXPECT_EQ(from_const_ref.serialize(), from_rvalue.serialize())
      << "adopting the forest must produce a byte-identical transition proof; "
         "any difference here is a consensus change, not an optimisation";

  // The const& overload must leave the caller's forest untouched.
  EXPECT_EQ(forest.getNumLeaves(), 1024u);
}

int main(int argc, char** argv) {
  // computeAdditionHashes' activation dispatch consults chain params.
  dinero::SelectParams(dinero::Chain::REGTEST);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
