// Forest checkpoint delta campaign — phase 2
// (docs/design/forest-checkpoint-deltas.md).
//
// The per-block Utreexo delta record (the UD:<blockhash> sidecar) becomes
// restore-critical in phase 2: restart replays it to rebuild the forest
// between full checkpoints. Per the design doc, delta records get the #397
// husk treatment — truncation at ANY byte offset must fail loudly and never
// partially apply. These tests pin the production codec (hoisted from
// chainstate_service.cpp into consensus/utreexo_delta_codec).

#include "consensus/utreexo_delta_codec.h"
#include "consensus/chainparams.h"
#include "primitives/block.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

using dinero::DeserializeUtreexoDelta;
using dinero::BuildStatelessUtreexoDelta;
using dinero::MakeUtreexoDeltaUndoKey;
using dinero::SerializeUtreexoDelta;
using dinero::consensus::UtreexoDelta;
using dinero::consensus::UtreexoHash;

namespace {

UtreexoHash MakeLeaf(uint8_t seed) {
    UtreexoHash h(32);
    for (size_t i = 0; i < 32; ++i) {
        h[i] = static_cast<uint8_t>(seed + i);
    }
    return h;
}

UtreexoDelta MakeSampleDelta() {
    UtreexoDelta delta;
    delta.numLeavesBefore = 191000;
    delta.recordDelete(17, MakeLeaf(1));
    delta.recordDelete(4093, MakeLeaf(2));
    delta.recordAdd(MakeLeaf(3), 191000);
    delta.recordAdd(MakeLeaf(4), 191001);
    delta.recordAdd(MakeLeaf(5), 191002);
    return delta;
}

dinero::Block MakeCoinbaseBlock(uint32_t height) {
    dinero::Block block{};
    dinero::Transaction coinbase;
    coinbase.version = 2;

    dinero::TxInput input;
    input.prevout.vout = std::numeric_limits<uint32_t>::max();
    input.scriptSig.push_back(static_cast<uint8_t>(height));
    coinbase.vin.push_back(std::move(input));

    dinero::TxOutput output;
    output.value = dinero::AmountUna::Una(50'000'000);
    output.scriptPubKey = {0x51};
    coinbase.vout.push_back(std::move(output));
    block.vtx.push_back(std::move(coinbase));
    return block;
}

void ExpectEqualDeltas(const UtreexoDelta& a, const UtreexoDelta& b) {
    EXPECT_EQ(a.numLeavesBefore, b.numLeavesBefore);
    ASSERT_EQ(a.deletedLeaves.size(), b.deletedLeaves.size());
    for (size_t i = 0; i < a.deletedLeaves.size(); ++i) {
        EXPECT_EQ(a.deletedLeaves[i].leafHash, b.deletedLeaves[i].leafHash);
        EXPECT_EQ(a.deletedLeaves[i].position, b.deletedLeaves[i].position);
    }
    ASSERT_EQ(a.addedLeaves.size(), b.addedLeaves.size());
    for (size_t i = 0; i < a.addedLeaves.size(); ++i) {
        EXPECT_EQ(a.addedLeaves[i].hash, b.addedLeaves[i].hash);
        EXPECT_EQ(a.addedLeaves[i].position, b.addedLeaves[i].position);
    }
}

TEST(UtreexoDeltaCodec, RoundTripPreservesEveryField) {
    const UtreexoDelta original = MakeSampleDelta();

    std::string blob, error;
    ASSERT_TRUE(SerializeUtreexoDelta(original, blob, error)) << error;
    ASSERT_FALSE(blob.empty());

    UtreexoDelta decoded;
    ASSERT_TRUE(DeserializeUtreexoDelta(blob, decoded, error)) << error;
    ExpectEqualDeltas(original, decoded);
}

TEST(UtreexoDeltaCodec, EmptyDeltaRoundTrips) {
    UtreexoDelta original;
    original.numLeavesBefore = 42;

    std::string blob, error;
    ASSERT_TRUE(SerializeUtreexoDelta(original, blob, error)) << error;

    UtreexoDelta decoded;
    ASSERT_TRUE(DeserializeUtreexoDelta(blob, decoded, error)) << error;
    ExpectEqualDeltas(original, decoded);
}

// THE husk test (#397 semantics): no truncation of the payload may decode.
TEST(UtreexoDeltaCodec, TruncationAtEveryByteOffsetFailsLoudly) {
    const UtreexoDelta original = MakeSampleDelta();

    std::string blob, error;
    ASSERT_TRUE(SerializeUtreexoDelta(original, blob, error)) << error;

    for (size_t len = 0; len < blob.size(); ++len) {
        UtreexoDelta decoded;
        decoded.numLeavesBefore = 0xDEADBEEF;  // sentinel: must stay untouched
        std::string trunc_error;
        EXPECT_FALSE(DeserializeUtreexoDelta(blob.substr(0, len), decoded,
                                             trunc_error))
            << "truncation at offset " << len << " decoded successfully";
        EXPECT_FALSE(trunc_error.empty())
            << "truncation at offset " << len << " gave no error";
        EXPECT_EQ(decoded.numLeavesBefore, 0xDEADBEEF)
            << "truncation at offset " << len << " partially applied";
    }
}

TEST(UtreexoDeltaCodec, TrailingGarbageFails) {
    const UtreexoDelta original = MakeSampleDelta();

    std::string blob, error;
    ASSERT_TRUE(SerializeUtreexoDelta(original, blob, error)) << error;

    blob.push_back('\x00');
    UtreexoDelta decoded;
    EXPECT_FALSE(DeserializeUtreexoDelta(blob, decoded, error));
    EXPECT_FALSE(error.empty());
}

TEST(UtreexoDeltaCodec, UnsupportedSchemaFails) {
    const UtreexoDelta original = MakeSampleDelta();

    std::string blob, error;
    ASSERT_TRUE(SerializeUtreexoDelta(original, blob, error)) << error;

    blob[0] = static_cast<char>(0x7F);  // bogus schema byte
    UtreexoDelta decoded;
    EXPECT_FALSE(DeserializeUtreexoDelta(blob, decoded, error));
    EXPECT_FALSE(error.empty());
}

TEST(UtreexoDeltaCodec, MalformedHashSizeRefusesToSerialize) {
    UtreexoDelta bad;
    bad.numLeavesBefore = 1;
    bad.recordDelete(0, UtreexoHash(31));  // wrong hash size

    std::string blob, error;
    EXPECT_FALSE(SerializeUtreexoDelta(bad, blob, error));
    EXPECT_FALSE(error.empty());
}

TEST(UtreexoDeltaCodec, UndoKeyMatchesLegacyFormat) {
    // The on-disk key format predates this codec unit (written by every
    // v8 node since the April P1 sidecar fix) and must never drift:
    // "UD:" + block hash hex.
    const dinero::uint256 hash = dinero::uint256::FromHexUnsafe(
        "00000010fb37bbab00000000000000000000000000000000000000000000abcd");
    EXPECT_EQ(MakeUtreexoDeltaUndoKey(hash), "UD:" + hash.GetHex());
}

TEST(UtreexoDeltaCodec, BuildsReplayableStatelessTransition) {
    dinero::SelectParams(dinero::Chain::REGTEST);

    dinero::consensus::UtreexoForest forest;
    const UtreexoHash spent = MakeLeaf(90);
    ASSERT_EQ(forest.add(spent), 0u);

    const dinero::Block block = MakeCoinbaseBlock(7);
    UtreexoDelta delta;
    std::string error;
    ASSERT_TRUE(BuildStatelessUtreexoDelta(
        forest, block, 7, {spent}, delta, error)) << error;

    ASSERT_EQ(delta.numLeavesBefore, 1u);
    ASSERT_EQ(delta.deletedLeaves.size(), 1u);
    EXPECT_EQ(delta.deletedLeaves[0].leafHash, spent);
    EXPECT_EQ(delta.deletedLeaves[0].position, 0u);
    ASSERT_EQ(delta.addedLeaves.size(), 1u);
    EXPECT_EQ(delta.addedLeaves[0].position, 1u);

    auto replayed = forest.cloneForHeight(7);
    ASSERT_TRUE(dinero::ApplyUtreexoDeltaForward(replayed, delta, error)) << error;
    EXPECT_EQ(replayed.getNumLeaves(), 2u);
    EXPECT_EQ(replayed.findLeafPosition(spent), std::nullopt);
    EXPECT_EQ(replayed.findLeafPosition(delta.addedLeaves[0].hash),
              std::optional<uint64_t>(1));
}

TEST(UtreexoDeltaCodec, MissingStatelessTargetFailsWithoutPartialOutput) {
    dinero::consensus::UtreexoForest forest;
    UtreexoDelta delta = MakeSampleDelta();
    const UtreexoDelta original = delta;
    std::string error;

    EXPECT_FALSE(BuildStatelessUtreexoDelta(
        forest, MakeCoinbaseBlock(8), 8, {MakeLeaf(200)}, delta, error));
    EXPECT_FALSE(error.empty());
    ExpectEqualDeltas(delta, original);
}

}  // namespace
