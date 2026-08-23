// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//
// Unit coverage for the M2 shielded output feed extractor.
//
// Fixtures construct ShieldedBundle objects directly (not parsed from
// wire) and serialise them via the daemon's canonical
// SerializeShieldedBundle so the bytes the extractor sees are real
// production-shape wire bytes.
//

#include "consensus/shielded/shielded_output_feed.h"
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
using dinero::consensus::shielded::ExtractShieldedOutputFeed;
using dinero::consensus::shielded::Hash;
using dinero::consensus::shielded::HASH_BYTES;
using dinero::consensus::shielded::kShieldedEncryptedNoteBytes;
using dinero::consensus::shielded::SerializeShieldedBundle;
using dinero::consensus::shielded::ShieldedBundle;
using dinero::consensus::shielded::ShieldedOutput;
using dinero::consensus::shielded::ShieldedOutputFeedError;
using dinero::consensus::shielded::ShieldedOutputFeedResult;
using dinero::consensus::shielded::ShieldedSpend;

namespace {

Hash MakeHash(uint8_t b) {
    Hash h{};
    h.fill(b);
    return h;
}

// 33-byte pedersen-prefixed value commitment. The feed extractor does
// not validate cv; any 33 bytes are fine for these tests.
std::array<uint8_t, 33> MakeCv(uint8_t b) {
    std::array<uint8_t, 33> cv{};
    cv[0] = 0x02;  // pedersen prefix
    for (size_t i = 1; i < 33; ++i) cv[i] = b;
    return cv;
}

ShieldedOutput MakeOutput(uint8_t commit_byte, uint8_t enc_byte,
                          size_t enc_size = kShieldedEncryptedNoteBytes) {
    ShieldedOutput o{};
    o.commitment = MakeHash(commit_byte);
    o.cv         = MakeCv(commit_byte);
    o.encrypted_note.assign(enc_size, enc_byte);
    o.zk_proof.clear();  // feed ignores zk_proof
    return o;
}

ShieldedSpend MakeSpend(uint8_t nullifier_byte) {
    ShieldedSpend s{};
    s.nullifier = MakeHash(nullifier_byte);
    s.anchor    = MakeHash(0);
    s.cv        = MakeCv(0);
    s.zk_proof.clear();
    return s;
}

Transaction MakeShieldedTx(int32_t version, const ShieldedBundle& bundle) {
    Transaction tx{};
    tx.version = version;
    tx.lockTime = 0;
    tx.witness_version = 0xFF;  // not segwit
    tx.shielded_bundle_bytes = SerializeShieldedBundle(bundle);
    return tx;
}

Transaction MakeTransparentTx() {
    Transaction tx{};
    tx.version = Transaction::TX_VERSION_SEGWIT;
    tx.lockTime = 0;
    return tx;
}

// Coinbase stand-in. Real blocks always carry one at vtx[0], and the feed
// walk deliberately starts at index 1 to match the consensus apply loop, so
// fixtures must include it or every block would appear to have no shielded
// transactions at all.
// Shape does not matter beyond "not shielded" — the feed only inspects
// tx.version and tx.shielded_bundle_bytes, never the coinbase's inputs.
Transaction MakeCoinbaseTx() {
    return MakeTransparentTx();
}

// Prepends the coinbase, so `txs` are placed at vtx[1..N] exactly as they
// would sit in a real block. Callers index their transactions from 1.
Block MakeBlockWith(std::vector<Transaction> txs) {
    Block block{};
    // Trivial fixed header bytes — the extractor only calls GetHash, and
    // GetHash is deterministic from header content, which is enough for
    // these tests.
    block.vtx.push_back(MakeCoinbaseTx());
    for (auto& tx : txs) {
        block.vtx.push_back(std::move(tx));
    }
    return block;
}

// Places `txs` verbatim, so the caller owns vtx[0]. Only needed to build the
// malicious shape a miner could craft: a shielded bundle on the coinbase.
Block MakeBlockWithRawVtx(std::vector<Transaction> txs) {
    Block block{};
    block.vtx = std::move(txs);
    return block;
}

}  // namespace

// ── Empty / transparent-only blocks ─────────────────────────────────

TEST(ShieldedOutputFeed, EmptyBlockReturnsZeroEntries) {
    Block block = MakeBlockWith({});
    ShieldedOutputFeedResult out{};
    const auto status = ExtractShieldedOutputFeed(block, /*height=*/100,
                                                   /*first_leaf_index=*/0,
                                                   &out);
    EXPECT_EQ(status, ShieldedOutputFeedError::Ok);
    EXPECT_TRUE(out.outputs.empty());
    EXPECT_TRUE(out.spent_nullifiers.empty());
    EXPECT_EQ(out.next_leaf_index, 0u);
}

TEST(ShieldedOutputFeed, TransparentOnlyBlockReturnsZeroEntries) {
    Block block = MakeBlockWith({MakeTransparentTx(), MakeTransparentTx()});
    ShieldedOutputFeedResult out{};
    const auto status = ExtractShieldedOutputFeed(block, /*height=*/101,
                                                   /*first_leaf_index=*/42,
                                                   &out);
    EXPECT_EQ(status, ShieldedOutputFeedError::Ok);
    EXPECT_TRUE(out.outputs.empty());
    EXPECT_TRUE(out.spent_nullifiers.empty());
    EXPECT_EQ(out.next_leaf_index, 42u)
        << "no outputs seen → next_leaf_index unchanged from first_leaf_index";
}

TEST(ShieldedOutputFeed, ShieldedTxWithEmptyBundleBytesIsSkipped) {
    Transaction tx{};
    tx.version = Transaction::TX_VERSION_SHIELDED;
    tx.lockTime = 0;
    // shielded_bundle_bytes left empty
    Block block = MakeBlockWith({tx});
    ShieldedOutputFeedResult out{};
    EXPECT_EQ(ExtractShieldedOutputFeed(block, 102, 0, &out),
              ShieldedOutputFeedError::Ok);
    EXPECT_TRUE(out.outputs.empty());
    EXPECT_TRUE(out.spent_nullifiers.empty());
    EXPECT_EQ(out.next_leaf_index, 0u);
}

// ── Single-output bundle ─────────────────────────────────────────────

TEST(ShieldedOutputFeed, OneShieldedTxWithOneOutputReturnsExactBytes) {
    ShieldedBundle bundle{};
    bundle.outputs.push_back(MakeOutput(/*commit_byte=*/0xAA,
                                        /*enc_byte=*/0xC1));
    Transaction tx = MakeShieldedTx(Transaction::TX_VERSION_SHIELDED, bundle);
    Block block = MakeBlockWith({tx});

    ShieldedOutputFeedResult out{};
    const auto status = ExtractShieldedOutputFeed(block, /*height=*/12847,
                                                   /*first_leaf_index=*/910,
                                                   &out);
    ASSERT_EQ(status, ShieldedOutputFeedError::Ok);
    ASSERT_EQ(out.outputs.size(), 1u);
    EXPECT_EQ(out.outputs[0].commitment, MakeHash(0xAA));
    EXPECT_EQ(out.outputs[0].encrypted_note.size(), kShieldedEncryptedNoteBytes);
    EXPECT_EQ(out.outputs[0].encrypted_note[0], 0xC1);
    EXPECT_EQ(out.outputs[0].encrypted_note[610], 0xC1);
    EXPECT_EQ(out.outputs[0].leaf_index, 910u);
    EXPECT_EQ(out.outputs[0].height, 12847u);
    EXPECT_EQ(out.outputs[0].tx_index, 1u);  // vtx[0] is the coinbase
    EXPECT_EQ(out.outputs[0].output_index, 0u);
    EXPECT_TRUE(out.spent_nullifiers.empty());
    EXPECT_EQ(out.next_leaf_index, 911u);
}

// ── Multi-tx, multi-output ordering + leaf indexing ─────────────────

TEST(ShieldedOutputFeed, MultipleShieldedTxsPreserveBlockTxOrder_AndLeafIndexes) {
    // vtx[0] is the coinbase (added by MakeBlockWith).
    // vtx[1]: two outputs (commit 0xAA + 0xBB — daemon sorts by commitment
    // so AA emits first within-bundle).
    ShieldedBundle bundle0{};
    bundle0.outputs.push_back(MakeOutput(0xBB, 0x10));
    bundle0.outputs.push_back(MakeOutput(0xAA, 0x20));

    // vtx[2] (transparent): does not contribute.
    // vtx[3]: one output (commit 0xCC).
    ShieldedBundle bundle2{};
    bundle2.outputs.push_back(MakeOutput(0xCC, 0x30));

    std::vector<Transaction> txs;
    txs.push_back(MakeShieldedTx(Transaction::TX_VERSION_SHIELDED, bundle0));
    txs.push_back(MakeTransparentTx());
    txs.push_back(MakeShieldedTx(Transaction::TX_VERSION_SHIELDED_V2, bundle2));
    Block block = MakeBlockWith(std::move(txs));

    ShieldedOutputFeedResult out{};
    ASSERT_EQ(ExtractShieldedOutputFeed(block, /*height=*/200,
                                         /*first_leaf_index=*/900, &out),
              ShieldedOutputFeedError::Ok);

    ASSERT_EQ(out.outputs.size(), 3u);

    // vtx[1] emits 0xAA then 0xBB (canonical sort by commitment).
    EXPECT_EQ(out.outputs[0].commitment, MakeHash(0xAA));
    EXPECT_EQ(out.outputs[0].tx_index,     1u);
    EXPECT_EQ(out.outputs[0].output_index, 0u);
    EXPECT_EQ(out.outputs[0].leaf_index,   900u);

    EXPECT_EQ(out.outputs[1].commitment, MakeHash(0xBB));
    EXPECT_EQ(out.outputs[1].tx_index,     1u);
    EXPECT_EQ(out.outputs[1].output_index, 1u);
    EXPECT_EQ(out.outputs[1].leaf_index,   901u);

    // vtx[2] is transparent (skipped). vtx[3] emits 0xCC.
    EXPECT_EQ(out.outputs[2].commitment, MakeHash(0xCC));
    EXPECT_EQ(out.outputs[2].tx_index,     3u);
    EXPECT_EQ(out.outputs[2].output_index, 0u);
    EXPECT_EQ(out.outputs[2].leaf_index,   902u);

    EXPECT_EQ(out.next_leaf_index, 903u);
}

// ── Spend nullifiers ────────────────────────────────────────────────

TEST(ShieldedOutputFeed, ShieldedSpendsEmitNullifiersWithoutAdvancingLeafIndex) {
    // vtx[0] is the coinbase (added by MakeBlockWith).
    // vtx[1]: spend-only (1 spend, no outputs)
    ShieldedBundle bundle0{};
    bundle0.spends.push_back(MakeSpend(0xE0));

    // vtx[2]: 2 outputs
    ShieldedBundle bundle1{};
    bundle1.outputs.push_back(MakeOutput(0x11, 0x40));
    bundle1.outputs.push_back(MakeOutput(0x22, 0x41));

    // vtx[3]: 1 spend + 1 output
    ShieldedBundle bundle2{};
    bundle2.spends.push_back(MakeSpend(0xE2));
    bundle2.outputs.push_back(MakeOutput(0x33, 0x42));

    std::vector<Transaction> txs;
    txs.push_back(MakeShieldedTx(Transaction::TX_VERSION_SHIELDED, bundle0));
    txs.push_back(MakeShieldedTx(Transaction::TX_VERSION_SHIELDED, bundle1));
    txs.push_back(MakeShieldedTx(Transaction::TX_VERSION_SHIELDED, bundle2));
    Block block = MakeBlockWith(std::move(txs));

    ShieldedOutputFeedResult out{};
    ASSERT_EQ(ExtractShieldedOutputFeed(block, /*height=*/300,
                                         /*first_leaf_index=*/5000, &out),
              ShieldedOutputFeedError::Ok);

    ASSERT_EQ(out.spent_nullifiers.size(), 2u);
    EXPECT_EQ(out.spent_nullifiers[0].nullifier, MakeHash(0xE0));
    EXPECT_EQ(out.spent_nullifiers[0].tx_index,    1u);
    EXPECT_EQ(out.spent_nullifiers[0].spend_index, 0u);
    EXPECT_EQ(out.spent_nullifiers[1].nullifier, MakeHash(0xE2));
    EXPECT_EQ(out.spent_nullifiers[1].tx_index,    3u);
    EXPECT_EQ(out.spent_nullifiers[1].spend_index, 0u);

    // Outputs: vtx[2] contributes 2 (sorted commitment 0x11, 0x22),
    // vtx[3] contributes 1 (0x33). Spends do NOT advance leaf_index.
    ASSERT_EQ(out.outputs.size(), 3u);
    EXPECT_EQ(out.outputs[0].commitment, MakeHash(0x11));
    EXPECT_EQ(out.outputs[0].leaf_index, 5000u);
    EXPECT_EQ(out.outputs[1].commitment, MakeHash(0x22));
    EXPECT_EQ(out.outputs[1].leaf_index, 5001u);
    EXPECT_EQ(out.outputs[2].commitment, MakeHash(0x33));
    EXPECT_EQ(out.outputs[2].leaf_index, 5002u);

    EXPECT_EQ(out.next_leaf_index, 5003u);
}

// ── Error paths ─────────────────────────────────────────────────────

TEST(ShieldedOutputFeed, MalformedBundleBytesReturnsDecodeFailed) {
    Transaction tx{};
    tx.version = Transaction::TX_VERSION_SHIELDED;
    tx.shielded_bundle_bytes = {0x00, 0x01, 0x02};  // not a valid serialised bundle
    Block block = MakeBlockWith({tx});

    ShieldedOutputFeedResult out{};
    EXPECT_EQ(ExtractShieldedOutputFeed(block, 1, 0, &out),
              ShieldedOutputFeedError::BundleDecodeFailed);
}

TEST(ShieldedOutputFeed, LegacyShortEncryptedNoteStillEmitsForTreeCompleteness) {
    ShieldedBundle bundle{};
    // Older wallet paths emitted 96-byte placeholder notes. They are not
    // decryptable by M2 clients, but their commitments are still real tree
    // leaves, so the feed must return them instead of poisoning the range.
    bundle.outputs.push_back(MakeOutput(0xAA, 0xC1, /*enc_size=*/96));
    Transaction tx = MakeShieldedTx(Transaction::TX_VERSION_SHIELDED, bundle);
    Block block = MakeBlockWith({tx});

    ShieldedOutputFeedResult out{};
    ASSERT_EQ(ExtractShieldedOutputFeed(block, 1, 0, &out),
              ShieldedOutputFeedError::Ok);
    ASSERT_EQ(out.outputs.size(), 1u);
    EXPECT_EQ(out.outputs[0].commitment, MakeHash(0xAA));
    EXPECT_EQ(out.outputs[0].encrypted_note.size(), 96u);
    EXPECT_EQ(out.outputs[0].leaf_index, 0u);
    EXPECT_EQ(out.next_leaf_index, 1u);
}

TEST(ShieldedOutputFeed, NullOutPointerReturnsDecodeFailed) {
    Block block = MakeBlockWith({});
    EXPECT_EQ(ExtractShieldedOutputFeed(block, 0, 0, /*out=*/nullptr),
              ShieldedOutputFeedError::BundleDecodeFailed);
}

// ── Coinbase-attached bundles are not shielded state ───────────────
//
// A miner can attach a shielded bundle to vtx[0]: nothing in block
// acceptance inspects the coinbase's version or bundle. Consensus ignores
// it — ConnectBlockInternal's per-tx loop and ApplyBlockShieldedSection
// both start at index 1 — so the feed must ignore it too. If the feed
// counted those outputs as leaves, every subsequent leaf_index would shift
// relative to the consensus commitment tree and BuildWitnessByIndex would
// derive a root that matches no anchor, breaking every later shielded spend.

TEST(ShieldedOutputFeed, CoinbaseAttachedBundleIsIgnored) {
    // vtx[0] = COINBASE carrying a shielded bundle (the malicious shape).
    ShieldedBundle cb_bundle{};
    cb_bundle.outputs.push_back(MakeOutput(0xF0, 0x01));
    cb_bundle.outputs.push_back(MakeOutput(0xF1, 0x02));
    cb_bundle.spends.push_back(MakeSpend(0xFE));

    // vtx[1] = an ordinary shielded tx that SHOULD contribute.
    ShieldedBundle real_bundle{};
    real_bundle.outputs.push_back(MakeOutput(0x0A, 0x03));

    std::vector<Transaction> txs;
    txs.push_back(MakeShieldedTx(Transaction::TX_VERSION_SHIELDED, cb_bundle));
    txs.push_back(MakeShieldedTx(Transaction::TX_VERSION_SHIELDED, real_bundle));
    Block block = MakeBlockWithRawVtx(std::move(txs));

    ShieldedOutputFeedResult out{};
    ASSERT_EQ(ExtractShieldedOutputFeed(block, /*height=*/400,
                                         /*first_leaf_index=*/700, &out),
              ShieldedOutputFeedError::Ok);

    // Only vtx[1]'s single output is emitted; the coinbase's two are not.
    ASSERT_EQ(out.outputs.size(), 1u);
    EXPECT_EQ(out.outputs[0].commitment, MakeHash(0x0A));
    EXPECT_EQ(out.outputs[0].tx_index, 1u);
    // The leaf basis is unshifted — this is the assertion that fails if the
    // coinbase's outputs are counted.
    EXPECT_EQ(out.outputs[0].leaf_index, 700u);
    EXPECT_EQ(out.next_leaf_index, 701u);

    // The coinbase's nullifier is not reported as spent either.
    EXPECT_TRUE(out.spent_nullifiers.empty());
}

// ── T2: CountShieldedOutputsBeforeHeight ───────────────────────────

using dinero::consensus::shielded::BlockByHeightLookup;
using dinero::consensus::shielded::CountShieldedOutputsBeforeHeight;
using dinero::Status;

namespace {

// Build a synthetic mapping height → Block. Empty entries mean
// "transparent-only block at that height". Used as a `lookup` callable.
struct StaticChain {
    std::map<uint32_t, Block> blocks;

    Block ZeroBundleBlock() const { return MakeBlockWith({MakeTransparentTx()}); }

    Block WithOutputCount(int n) const {
        ShieldedBundle b{};
        for (int i = 0; i < n; ++i) {
            b.outputs.push_back(MakeOutput(static_cast<uint8_t>(0x10 + i),
                                            static_cast<uint8_t>(0xA0 + i)));
        }
        return MakeBlockWith({MakeShieldedTx(Transaction::TX_VERSION_SHIELDED, b)});
    }

    BlockByHeightLookup lookup() {
        return [this](uint32_t h) -> std::optional<Block> {
            auto it = blocks.find(h);
            if (it == blocks.end()) return std::nullopt;
            return it->second;
        };
    }
};

}  // namespace

TEST(ShieldedOutputFeed, CountReturnsZeroBeforeActivation) {
    StaticChain chain;
    auto result = CountShieldedOutputsBeforeHeight(/*from_height=*/100,
                                                    /*activation=*/100,
                                                    chain.lookup());
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), 0u);

    result = CountShieldedOutputsBeforeHeight(/*from=*/50,
                                               /*activation=*/100,
                                               chain.lookup());
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), 0u)
        << "from_height < activation_height yields 0 without calling lookup";
}

TEST(ShieldedOutputFeed, CountAccumulatesAcrossBlocks) {
    StaticChain chain;
    // Activation at 100. Blocks 100-104:
    //   100: 2 outputs
    //   101: 0 outputs (transparent-only)
    //   102: 1 output
    //   103: 3 outputs
    //   104: 0 outputs (empty bundle bytes via transparent-only)
    chain.blocks[100] = chain.WithOutputCount(2);
    chain.blocks[101] = chain.ZeroBundleBlock();
    chain.blocks[102] = chain.WithOutputCount(1);
    chain.blocks[103] = chain.WithOutputCount(3);
    chain.blocks[104] = chain.ZeroBundleBlock();

    // Walk [100, 105) → expect 2 + 0 + 1 + 3 + 0 = 6
    auto result = CountShieldedOutputsBeforeHeight(/*from=*/105,
                                                    /*activation=*/100,
                                                    chain.lookup());
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), 6u);

    // Walk [100, 103) → expect 2 + 0 + 1 = 3
    result = CountShieldedOutputsBeforeHeight(/*from=*/103,
                                               /*activation=*/100,
                                               chain.lookup());
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value(), 3u);
}

TEST(ShieldedOutputFeed, CountReturnsNotFoundWhenChainShortOfRequestedHeight) {
    StaticChain chain;
    chain.blocks[100] = chain.WithOutputCount(1);
    // Height 101 absent — lookup returns nullopt.

    auto result = CountShieldedOutputsBeforeHeight(/*from=*/102,
                                                    /*activation=*/100,
                                                    chain.lookup());
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status(), Status::NotFound);
}

TEST(ShieldedOutputFeed, CountReturnsSerializationOnMalformedHistoricalBundle) {
    StaticChain chain;
    chain.blocks[100] = chain.WithOutputCount(2);
    // Inject a malformed shielded tx at height 101.
    Transaction badTx{};
    badTx.version = Transaction::TX_VERSION_SHIELDED;
    badTx.shielded_bundle_bytes = {0x99, 0x88, 0x77};  // junk
    chain.blocks[101] = MakeBlockWith({badTx});

    auto result = CountShieldedOutputsBeforeHeight(/*from=*/102,
                                                    /*activation=*/100,
                                                    chain.lookup());
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status(), Status::Serialization);
}

TEST(ShieldedOutputFeed, CoinbaseAttachedBundleDoesNotCorruptLeafBasis) {
    // The joint invariant CountShieldedOutputsBeforeHeight and
    // ExtractShieldedOutputFeed must satisfy: the count attributed to
    // heights [activation, from_height) equals the number of leaves the feed
    // would emit over those same heights. A coinbase bundle must be excluded
    // by BOTH or light-client leaf indices desync at the from_height boundary.
    ShieldedBundle cb_bundle{};
    cb_bundle.outputs.push_back(MakeOutput(0xF0, 0x01));
    cb_bundle.outputs.push_back(MakeOutput(0xF1, 0x02));

    ShieldedBundle real_bundle{};
    real_bundle.outputs.push_back(MakeOutput(0x0A, 0x03));

    std::vector<Transaction> txs;
    txs.push_back(MakeShieldedTx(Transaction::TX_VERSION_SHIELDED, cb_bundle));
    txs.push_back(MakeShieldedTx(Transaction::TX_VERSION_SHIELDED, real_bundle));

    StaticChain chain;
    chain.blocks[100] = MakeBlockWithRawVtx(std::move(txs));

    // What the feed actually emits for height 100.
    ShieldedOutputFeedResult out{};
    ASSERT_EQ(ExtractShieldedOutputFeed(chain.blocks[100], /*height=*/100,
                                         /*first_leaf_index=*/0, &out),
              ShieldedOutputFeedError::Ok);

    // What the leaf-basis counter attributes to the same height range.
    auto counted = CountShieldedOutputsBeforeHeight(/*from=*/101,
                                                     /*activation=*/100,
                                                     chain.lookup());
    ASSERT_TRUE(counted.ok());

    EXPECT_EQ(counted.value(), out.outputs.size())
        << "leaf basis and emitted feed disagree — light-client leaf indices "
           "would desync across the from_height boundary";
    EXPECT_EQ(counted.value(), 1u) << "coinbase outputs must not be counted";
}
