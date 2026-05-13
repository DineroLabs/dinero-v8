// Copyright (c) 2026 Dinero Labs.
//
// Phase 0 wave 1 — wire-format tests for ShieldedBundle. The
// canonical-serialization invariant is consensus-critical: two nodes
// MUST produce byte-identical serializations of the same logical
// bundle. These tests pin round-trips, canonical ordering, and
// deserialization rejection of non-canonical inputs.

#include <gtest/gtest.h>

#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_tx.h"

#include <array>
#include <cstdint>
#include <vector>

namespace dinero::consensus::shielded::testing {
namespace {

using shielded::BundleDecodeError;
using shielded::DeserializeShieldedBundle;
using shielded::Hash;
using shielded::SerializeShieldedBundle;
using shielded::ShieldedBundle;
using shielded::ShieldedOutput;
using shielded::ShieldedSpend;

Hash MakeHash(uint8_t seed) {
    Hash h{};
    h[0] = seed;
    h[31] = 0x55;
    return h;
}

// BindingSignature grew to 64 bytes; MakeHash (32 bytes) can no longer
// stand in for it. Helper that returns the 64-byte sig with the same
// seed pattern so existing test bodies keep their property checks.
shielded::BindingSignature MakeSig(uint8_t seed) {
    shielded::BindingSignature s{};
    s[0]  = seed;
    s[31] = 0x55;
    s[63] = seed;  // pin the tail too so round-trip tests catch byte-tail bugs
    return s;
}

ShieldedSpend MakeSpend(uint8_t nullifier_seed, uint8_t anchor_seed) {
    ShieldedSpend s;
    s.nullifier = MakeHash(nullifier_seed);
    s.anchor    = MakeHash(anchor_seed);
    s.zk_proof  = std::vector<uint8_t>(64, nullifier_seed);
    return s;
}

ShieldedOutput MakeOutput(uint8_t commitment_seed) {
    ShieldedOutput o;
    o.commitment      = MakeHash(commitment_seed);
    o.encrypted_note  = std::vector<uint8_t>(96, commitment_seed);
    o.zk_proof        = std::vector<uint8_t>(64, commitment_seed);
    return o;
}

TEST(ShieldedSerializationTest, EmptyBundleRoundTrips) {
    ShieldedBundle src;
    src.value_balance = 0;
    src.binding_sig   = MakeSig(0xFF);
    const auto bytes = SerializeShieldedBundle(src);
    ASSERT_FALSE(bytes.empty());

    ShieldedBundle decoded;
    EXPECT_EQ(DeserializeShieldedBundle(bytes, &decoded), BundleDecodeError::Ok);
    EXPECT_EQ(decoded.value_balance, 0);
    EXPECT_TRUE(decoded.spends.empty());
    EXPECT_TRUE(decoded.outputs.empty());
    EXPECT_EQ(decoded.binding_sig, src.binding_sig);
}

TEST(ShieldedSerializationTest, SingleOutputRoundTrip) {
    ShieldedBundle src;
    src.value_balance = 100'000'000;  // 1 DIN positive (shield)
    src.outputs.push_back(MakeOutput(0x10));
    src.binding_sig = MakeSig(0xEE);

    const auto bytes = SerializeShieldedBundle(src);
    ASSERT_FALSE(bytes.empty());

    ShieldedBundle decoded;
    ASSERT_EQ(DeserializeShieldedBundle(bytes, &decoded), BundleDecodeError::Ok);
    EXPECT_EQ(decoded.value_balance, src.value_balance);
    ASSERT_EQ(decoded.outputs.size(), 1u);
    EXPECT_EQ(decoded.outputs[0].commitment, src.outputs[0].commitment);
    EXPECT_EQ(decoded.outputs[0].encrypted_note, src.outputs[0].encrypted_note);
    EXPECT_EQ(decoded.outputs[0].zk_proof, src.outputs[0].zk_proof);
}

TEST(ShieldedSerializationTest, SingleSpendRoundTrip) {
    ShieldedBundle src;
    src.value_balance = -50'000'000;  // unshield
    src.spends.push_back(MakeSpend(0x21, 0x30));
    src.binding_sig = MakeSig(0xDD);

    const auto bytes = SerializeShieldedBundle(src);
    ASSERT_FALSE(bytes.empty());

    ShieldedBundle decoded;
    ASSERT_EQ(DeserializeShieldedBundle(bytes, &decoded), BundleDecodeError::Ok);
    EXPECT_EQ(decoded.value_balance, src.value_balance);
    ASSERT_EQ(decoded.spends.size(), 1u);
    EXPECT_EQ(decoded.spends[0].nullifier, src.spends[0].nullifier);
    EXPECT_EQ(decoded.spends[0].anchor,    src.spends[0].anchor);
}

TEST(ShieldedSerializationTest, MixedBundleRoundTrip) {
    ShieldedBundle src;
    src.value_balance = 0;  // pure transfer
    src.spends.push_back(MakeSpend(0x40, 0x50));
    src.spends.push_back(MakeSpend(0x41, 0x50));
    src.outputs.push_back(MakeOutput(0x60));
    src.outputs.push_back(MakeOutput(0x61));
    src.binding_sig = MakeSig(0xCC);

    const auto bytes = SerializeShieldedBundle(src);
    ASSERT_FALSE(bytes.empty());

    ShieldedBundle decoded;
    ASSERT_EQ(DeserializeShieldedBundle(bytes, &decoded), BundleDecodeError::Ok);
    EXPECT_EQ(decoded.spends.size(), 2u);
    EXPECT_EQ(decoded.outputs.size(), 2u);
}

TEST(ShieldedSerializationTest, CanonicalOrderingIsEnforcedByEncoder) {
    // Build a bundle with intentionally unsorted entries; the encoder
    // MUST emit them in canonical order.
    ShieldedBundle src;
    src.outputs.push_back(MakeOutput(0x33));
    src.outputs.push_back(MakeOutput(0x11));
    src.outputs.push_back(MakeOutput(0x22));
    src.binding_sig = MakeSig(0x00);

    const auto bytes = SerializeShieldedBundle(src);
    ASSERT_FALSE(bytes.empty());

    ShieldedBundle decoded;
    ASSERT_EQ(DeserializeShieldedBundle(bytes, &decoded), BundleDecodeError::Ok);
    ASSERT_EQ(decoded.outputs.size(), 3u);
    // After canonical encode, decoded outputs are sorted by commitment ascending.
    EXPECT_LE(decoded.outputs[0].commitment, decoded.outputs[1].commitment);
    EXPECT_LE(decoded.outputs[1].commitment, decoded.outputs[2].commitment);
}

TEST(ShieldedSerializationTest, ReencodeIsByteIdentical) {
    ShieldedBundle src;
    src.value_balance = 12345;
    src.spends.push_back(MakeSpend(0x70, 0x71));
    src.outputs.push_back(MakeOutput(0x80));
    src.outputs.push_back(MakeOutput(0x81));
    src.binding_sig = MakeSig(0xAA);

    const auto bytes = SerializeShieldedBundle(src);
    ShieldedBundle decoded;
    ASSERT_EQ(DeserializeShieldedBundle(bytes, &decoded), BundleDecodeError::Ok);
    const auto reencoded = SerializeShieldedBundle(decoded);
    EXPECT_EQ(bytes, reencoded);
}

TEST(ShieldedSerializationTest, TrailingBytesRejected) {
    ShieldedBundle src;
    src.outputs.push_back(MakeOutput(0x90));
    src.binding_sig = MakeSig(0x90);

    auto bytes = SerializeShieldedBundle(src);
    bytes.push_back(0x00);  // extra byte after binding_sig

    ShieldedBundle decoded;
    EXPECT_EQ(DeserializeShieldedBundle(bytes, &decoded), BundleDecodeError::TrailingBytes);
}

TEST(ShieldedSerializationTest, TruncatedRejected) {
    ShieldedBundle src;
    src.outputs.push_back(MakeOutput(0xA0));
    src.binding_sig = MakeSig(0xA0);

    auto bytes = SerializeShieldedBundle(src);
    bytes.resize(bytes.size() / 2);  // chop in half

    ShieldedBundle decoded;
    EXPECT_EQ(DeserializeShieldedBundle(bytes, &decoded), BundleDecodeError::Truncated);
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
