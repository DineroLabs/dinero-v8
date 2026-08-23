// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//
// Unit coverage for the M3 shielded witness builder.
//
// Fixtures build synthetic chains of single-tx blocks that each carry
// a serialised ShieldedBundle. The replay path consumes the same wire
// bytes the M2 RPC ships, so witnesses exercised here are byte-shape-
// identical to the ones a client will request in production.
//

#include "consensus/shielded/shielded_witness.h"

#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_tx.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

using dinero::Block;
using dinero::Transaction;
using dinero::consensus::shielded::BlockByHeightLookup;
using dinero::consensus::shielded::BuildWitnessByIndex;
using dinero::consensus::shielded::CommitmentTree;
using dinero::consensus::shielded::Hash;
using dinero::consensus::shielded::HASH_BYTES;
using dinero::consensus::shielded::SerializeShieldedBundle;
using dinero::consensus::shielded::ShieldedBundle;
using dinero::consensus::shielded::ShieldedOutput;
using dinero::consensus::shielded::ShieldedWitness;
using dinero::consensus::shielded::ShieldedWitnessError;
using dinero::consensus::shielded::ShieldedWitnessRequest;

namespace {

Hash MakeHash(uint8_t b) {
    Hash h{};
    h.fill(b);
    return h;
}

std::array<uint8_t, 33> MakeCv(uint8_t b) {
    std::array<uint8_t, 33> cv{};
    cv[0] = 0x02;
    for (size_t i = 1; i < 33; ++i) cv[i] = b;
    return cv;
}

ShieldedOutput MakeOutput(uint8_t commit_byte) {
    ShieldedOutput o{};
    o.commitment = MakeHash(commit_byte);
    o.cv         = MakeCv(commit_byte);
    o.encrypted_note.assign(611, /*placeholder*/ 0xCC);
    o.zk_proof.clear();
    return o;
}

Transaction MakeShieldedTxWithOutputs(std::vector<uint8_t> commit_bytes) {
    ShieldedBundle bundle{};
    for (uint8_t b : commit_bytes) bundle.outputs.push_back(MakeOutput(b));
    Transaction tx{};
    tx.version              = Transaction::TX_VERSION_SHIELDED;
    tx.lockTime             = 0;
    tx.witness_version      = 0xFF;
    tx.shielded_bundle_bytes = SerializeShieldedBundle(bundle);
    return tx;
}

Transaction MakeTransparentTx() {
    Transaction tx{};
    tx.version  = Transaction::TX_VERSION_SEGWIT;
    tx.lockTime = 0;
    return tx;
}

// Coinbase stand-in. Real blocks always carry one at vtx[0], and the output
// feed that BuildWitnessByIndex replays starts at index 1 to match the
// consensus apply loop — so fixtures must include it or the witness tree
// would be built from an empty feed.
// Shape does not matter beyond "not shielded" — the feed only inspects
// tx.version and tx.shielded_bundle_bytes, never the coinbase's inputs.
Transaction MakeCoinbaseTx() {
    return MakeTransparentTx();
}

// Prepends the coinbase so `txs` sit at vtx[1..N], as in a real block.
Block MakeBlockWith(std::vector<Transaction> txs) {
    Block block{};
    block.vtx.push_back(MakeCoinbaseTx());
    for (auto& tx : txs) {
        block.vtx.push_back(std::move(tx));
    }
    return block;
}

// Static synthetic chain: height → Block. lookup() returns nullopt for
// any height not in the map.
struct StaticChain {
    std::map<uint32_t, Block> blocks;
    BlockByHeightLookup lookup() {
        return [this](uint32_t h) -> std::optional<Block> {
            auto it = blocks.find(h);
            if (it == blocks.end()) return std::nullopt;
            return it->second;
        };
    }
};

// Independently compute the root a fresh CommitmentTree converges to
// after appending the given canonical-order commitments. Used as a
// trust-anchor in tests.
Hash IndependentRoot(const std::vector<Hash>& leaves) {
    CommitmentTree tree;
    for (const auto& l : leaves) tree.Append(l);
    return tree.Root();
}

// In daemon-canonical bundle order, outputs sort by commitment value.
// Test bundles use ascending bytes so the canonical order matches the
// test's natural reading order: 0x10, 0x11, 0x12, ...
std::vector<Hash> CanonicalLeavesAscending(uint8_t start, size_t n) {
    std::vector<Hash> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(MakeHash(start + static_cast<uint8_t>(i)));
    return out;
}

}  // namespace

// ── 0-leaf tree ─────────────────────────────────────────────────────

TEST(ShieldedWitness, RejectsLeafZero_OnEmptyTree) {
    StaticChain chain;
    chain.blocks[100] = MakeBlockWith({MakeTransparentTx()});
    chain.blocks[101] = MakeBlockWith({MakeTransparentTx()});

    ShieldedWitnessRequest req{};
    req.leaf_index                 = 0;
    req.anchor_height              = 101;
    req.anchor_root                = IndependentRoot({});  // empty-tree root
    req.shielded_activation_height = 100;

    ShieldedWitness out{};
    EXPECT_EQ(BuildWitnessByIndex(req, chain.lookup(), &out),
              ShieldedWitnessError::LeafOutOfRange);
}

// ── Synthetic 5-leaf tree — happy path ──────────────────────────────

TEST(ShieldedWitness, FiveLeafTree_ReturnsPathThatVerifiesToRoot) {
    StaticChain chain;
    chain.blocks[100] = MakeBlockWith({MakeShieldedTxWithOutputs({0x10, 0x11, 0x12})});
    chain.blocks[101] = MakeBlockWith({MakeShieldedTxWithOutputs({0x13, 0x14})});

    const auto leaves = CanonicalLeavesAscending(0x10, 5);
    const Hash expected_root = IndependentRoot(leaves);

    // Request leaf 2 (commitment 0x12), at anchor_height=101.
    ShieldedWitnessRequest req{};
    req.leaf_index                 = 2;
    req.anchor_height              = 101;
    req.anchor_root                = expected_root;
    req.shielded_activation_height = 100;

    ShieldedWitness out{};
    ASSERT_EQ(BuildWitnessByIndex(req, chain.lookup(), &out),
              ShieldedWitnessError::Ok);

    EXPECT_EQ(out.leaf_index,    2u);
    EXPECT_EQ(out.anchor_height, 101u);
    EXPECT_EQ(out.tree_size,     5u);
    EXPECT_EQ(out.anchor_root,   expected_root);
    EXPECT_EQ(out.commitment,    MakeHash(0x12));
    EXPECT_EQ(out.auth_path.leaf_index, 2u);

    // Sanity: GetAuthPath on an independently rebuilt tree returns the
    // same siblings — i.e., the replay matches what a wallet-side full
    // tree would produce for the same leaf at the same anchor.
    {
        CommitmentTree tree;
        for (const auto& l : leaves) tree.Append(l);
        auto ref_path = tree.GetAuthPath(2);
        ASSERT_TRUE(ref_path.has_value());
        EXPECT_EQ(out.auth_path.siblings, ref_path->siblings);
    }
}

// ── Anchor validation: wrong client anchor → AnchorMismatch ─────────

TEST(ShieldedWitness, WrongAnchorRoot_ReturnsAnchorMismatch) {
    StaticChain chain;
    chain.blocks[100] = MakeBlockWith({MakeShieldedTxWithOutputs({0x10, 0x11})});

    Hash bogus_root = MakeHash(0xDE);
    ShieldedWitnessRequest req{};
    req.leaf_index                 = 0;
    req.anchor_height              = 100;
    req.anchor_root                = bogus_root;
    req.shielded_activation_height = 100;

    ShieldedWitness out{};
    EXPECT_EQ(BuildWitnessByIndex(req, chain.lookup(), &out),
              ShieldedWitnessError::AnchorMismatch);
}

// ── Leaf-index past tree size → LeafOutOfRange ─────────────────────

TEST(ShieldedWitness, LeafPastTreeSize_ReturnsLeafOutOfRange) {
    StaticChain chain;
    chain.blocks[100] = MakeBlockWith({MakeShieldedTxWithOutputs({0x10, 0x11})});

    const Hash root = IndependentRoot(CanonicalLeavesAscending(0x10, 2));
    ShieldedWitnessRequest req{};
    req.leaf_index                 = 99;  // tree has only 2 leaves
    req.anchor_height              = 100;
    req.anchor_root                = root;
    req.shielded_activation_height = 100;

    ShieldedWitness out{};
    EXPECT_EQ(BuildWitnessByIndex(req, chain.lookup(), &out),
              ShieldedWitnessError::LeafOutOfRange);
}

// ── Missing block during replay → MissingBlock ──────────────────────

TEST(ShieldedWitness, MissingHistoricalBlock_ReturnsMissingBlock) {
    StaticChain chain;
    chain.blocks[100] = MakeBlockWith({MakeShieldedTxWithOutputs({0x10})});
    // height 101 missing
    chain.blocks[102] = MakeBlockWith({MakeShieldedTxWithOutputs({0x11})});

    ShieldedWitnessRequest req{};
    req.leaf_index                 = 0;
    req.anchor_height              = 102;
    req.anchor_root                = IndependentRoot({MakeHash(0x10)});
    req.shielded_activation_height = 100;

    ShieldedWitness out{};
    EXPECT_EQ(BuildWitnessByIndex(req, chain.lookup(), &out),
              ShieldedWitnessError::MissingBlock);
}

// ── Malformed historical bundle → BundleDecodeFailed ───────────────

TEST(ShieldedWitness, MalformedHistoricalBundle_ReturnsBundleDecodeFailed) {
    StaticChain chain;
    Transaction bad{};
    bad.version              = Transaction::TX_VERSION_SHIELDED;
    bad.shielded_bundle_bytes = {0x99, 0x88, 0x77};  // junk
    chain.blocks[100] = MakeBlockWith({bad});

    ShieldedWitnessRequest req{};
    req.leaf_index                 = 0;
    req.anchor_height              = 100;
    req.anchor_root                = IndependentRoot({});
    req.shielded_activation_height = 100;

    ShieldedWitness out{};
    EXPECT_EQ(BuildWitnessByIndex(req, chain.lookup(), &out),
              ShieldedWitnessError::BundleDecodeFailed);
}

// ── Null out pointer → MissingBlock (defensive) ─────────────────────

TEST(ShieldedWitness, NullOutPointer_ReturnsMissingBlock) {
    StaticChain chain;
    ShieldedWitnessRequest req{};
    req.shielded_activation_height = 100;
    req.anchor_height              = 99;  // empty range, lookup never called

    EXPECT_EQ(BuildWitnessByIndex(req, chain.lookup(), /*out=*/nullptr),
              ShieldedWitnessError::MissingBlock);
}

// ── Replay built from block bytes only (no live GetAuthPath) ────────
//
// The plan explicitly says: "build after frontier-only state is
// simulated by using only block replay, not live chainstate
// GetAuthPath." This test exercises the same code path with a chain
// whose blocks carry shielded output bytes — i.e., the daemon's live
// state could be a frontier (no leaves_ retained) and this still works
// because we replay public bytes from scratch.
TEST(ShieldedWitness, ReplayIsIndependentOfLiveTreeState) {
    StaticChain chain;
    chain.blocks[200] = MakeBlockWith({MakeShieldedTxWithOutputs({0x10, 0x11, 0x12, 0x13})});

    const auto leaves = CanonicalLeavesAscending(0x10, 4);
    const Hash root = IndependentRoot(leaves);

    ShieldedWitnessRequest req{};
    req.leaf_index                 = 3;
    req.anchor_height              = 200;
    req.anchor_root                = root;
    req.shielded_activation_height = 200;

    ShieldedWitness out{};
    ASSERT_EQ(BuildWitnessByIndex(req, chain.lookup(), &out),
              ShieldedWitnessError::Ok);
    EXPECT_EQ(out.commitment, MakeHash(0x13));
    EXPECT_EQ(out.tree_size,  4u);
    EXPECT_EQ(out.anchor_root, root);
}
