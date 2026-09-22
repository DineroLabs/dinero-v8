#include "consensus/csn_replay_data.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using dinero::consensus::CsnReplayData;
using dinero::consensus::DecodeCsnReplayData;
using dinero::consensus::SerializeCsnReplayData;
using dinero::consensus::SpentOutputData;
using dinero::consensus::UtreexoHash;

namespace {

void AppendU32(std::string& blob, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        blob.push_back(static_cast<char>(value >> shift));
    }
}

void SetU32(std::string& blob, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        blob.at(offset + i) = static_cast<char>(value >> (8 * i));
    }
}

UtreexoHash Hash(uint8_t seed) {
    UtreexoHash result(32);
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<uint8_t>(seed + i);
    }
    return result;
}

std::vector<SpentOutputData> Metadata() {
    return {
        {0x8877665544332211ULL, {0x51, 0x00, 0xac}, 0xfedcba98u, true,
         true, {0x02, 0x00, 0xff}},
        {0, {}, 42, false, false, {}},
    };
}

std::string LegacyRecord(const std::vector<UtreexoHash>& targets) {
    std::string blob;
    AppendU32(blob, static_cast<uint32_t>(targets.size()));
    for (const auto& target : targets) {
        blob.append(reinterpret_cast<const char*>(target.data()), target.size());
    }
    return blob;
}

// Frozen daemon_app.cpp encoder, predating the shared codec. This is an
// independent framing oracle; the literal fixture below also pins metadata.
std::string OriginalCsn2Encoder(const std::vector<UtreexoHash>& targets,
                               const std::vector<SpentOutputData>& spent,
                               uint8_t version) {
    std::string blob("CSN2", 4);
    AppendU32(blob, static_cast<uint32_t>(targets.size()));
    for (const auto& target : targets) {
        blob.append(reinterpret_cast<const char*>(target.data()), target.size());
    }
    AppendU32(blob, static_cast<uint32_t>(spent.size()));
    blob.push_back(static_cast<char>(version));
    for (const auto& output : spent) {
        const auto bytes = output.serialize(version);
        blob.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return blob;
}

std::string FromHex(const std::string& hex) {
    std::string bytes;
    for (size_t i = 0; i < hex.size(); i += 2) {
        bytes.push_back(static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

void ExpectEmpty(const CsnReplayData& data) {
    EXPECT_TRUE(data.spend_targets.empty());
    EXPECT_TRUE(data.spent_outputs.empty());
    EXPECT_FALSE(data.has_spent_outputs);
}

CsnReplayData StaleData() {
    CsnReplayData data;
    data.spend_targets = {Hash(0xaa)};
    data.spent_outputs = Metadata();
    data.has_spent_outputs = true;
    return data;
}

void ExpectRejected(const std::string& blob) {
    auto data = StaleData();
    bool accepted = true;
    EXPECT_NO_THROW(accepted = DecodeCsnReplayData(blob, data));
    EXPECT_FALSE(accepted);
    ExpectEmpty(data);
}

void ExpectMetadata(const std::vector<SpentOutputData>& actual,
                    const std::vector<SpentOutputData>& expected,
                    uint8_t version) {
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(actual[i].value, expected[i].value);
        EXPECT_EQ(actual[i].scriptPubKey, expected[i].scriptPubKey);
        EXPECT_EQ(actual[i].created_height, version >= 6 ? expected[i].created_height : 0u);
        EXPECT_EQ(actual[i].is_coinbase, version >= 6 && expected[i].is_coinbase);
        EXPECT_EQ(actual[i].is_confidential, version >= 5 && expected[i].is_confidential);
        EXPECT_EQ(actual[i].commitment, version >= 5 ? expected[i].commitment : std::vector<uint8_t>{});
    }
}

}  // namespace

TEST(CsnReplayData, LegacyHashOnlyPreservesOrderAndEmbeddedZeros) {
    const std::vector<UtreexoHash> targets{Hash(0), Hash(0xf0)};
    CsnReplayData data;
    ASSERT_TRUE(DecodeCsnReplayData(LegacyRecord(targets), data));
    EXPECT_EQ(data.spend_targets, targets);
    EXPECT_TRUE(data.spent_outputs.empty());
    EXPECT_FALSE(data.has_spent_outputs);
}

TEST(CsnReplayData, EmptyLegacyAndCsn2MetadataRemainDistinct) {
    CsnReplayData data;
    ASSERT_TRUE(DecodeCsnReplayData(std::string(4, '\0'), data));
    ExpectEmpty(data);
    for (uint8_t version : {4, 5, 6}) {
        ASSERT_TRUE(DecodeCsnReplayData(OriginalCsn2Encoder({}, {}, version), data));
        EXPECT_TRUE(data.spend_targets.empty());
        EXPECT_TRUE(data.spent_outputs.empty());
        EXPECT_TRUE(data.has_spent_outputs);
    }
}

TEST(CsnReplayData, ExistingCsn2EncoderBytesAndEveryMetadataFieldSurvive) {
    const std::vector<UtreexoHash> targets{Hash(0), Hash(0xf0)};
    const auto spent = Metadata();
    for (uint8_t version : {4, 5, 6}) {
        SCOPED_TRACE(static_cast<int>(version));
        const auto blob = SerializeCsnReplayData(targets, spent, version);
        EXPECT_EQ(blob, OriginalCsn2Encoder(targets, spent, version));
        CsnReplayData data;
        ASSERT_TRUE(DecodeCsnReplayData(blob, data));
        EXPECT_EQ(data.spend_targets, targets);
        EXPECT_TRUE(data.has_spent_outputs);
        ExpectMetadata(data.spent_outputs, spent, version);
        EXPECT_EQ(SerializeCsnReplayData(data.spend_targets, data.spent_outputs, version), blob);
    }
}

TEST(CsnReplayData, LiteralFixturesPinVersionedWireLayout) {
    const auto spent = Metadata().front();
    // No forest targets, one input. Intra-block spends need not be targets.
    const std::string prefix = "43534e320000000001000000";
    const std::string transparent = "1122334455667788030000005100ac";
    const std::string confidential = "01030000000200ff";
    const std::string maturity = "98badcfe01";
    const std::vector<std::pair<uint8_t, std::string>> fixtures{
        {4, prefix + "04" + transparent},
        {5, prefix + "05" + transparent + confidential},
        {6, prefix + "06" + transparent + maturity + confidential},
    };
    for (const auto& [version, hex] : fixtures) {
        SCOPED_TRACE(static_cast<int>(version));
        const auto blob = FromHex(hex);
        EXPECT_EQ(SerializeCsnReplayData({}, {spent}, version), blob);
        CsnReplayData data;
        ASSERT_TRUE(DecodeCsnReplayData(blob, data));
        EXPECT_TRUE(data.spend_targets.empty());
        EXPECT_TRUE(data.has_spent_outputs);
        ExpectMetadata(data.spent_outputs, {spent}, version);
    }
}

TEST(CsnReplayData, ReusedOutputDropsMetadataOnLegacySuccess) {
    CsnReplayData data;
    ASSERT_TRUE(DecodeCsnReplayData(OriginalCsn2Encoder({Hash(1)}, Metadata(), 6), data));
    ASSERT_TRUE(DecodeCsnReplayData(LegacyRecord({Hash(2)}), data));
    EXPECT_EQ(data.spend_targets, std::vector<UtreexoHash>{Hash(2)});
    EXPECT_TRUE(data.spent_outputs.empty());
    EXPECT_FALSE(data.has_spent_outputs);
    ASSERT_TRUE(DecodeCsnReplayData(std::string(4, '\0'), data));
    ExpectEmpty(data);
}

TEST(CsnReplayData, ReusedOutputDropsMetadataAfterEveryFailureStage) {
    ExpectRejected("");
    ExpectRejected("CSN2");
    auto blob = OriginalCsn2Encoder({Hash(1)}, Metadata(), 6);
    ExpectRejected(blob.substr(0, 40));
    ExpectRejected(blob.substr(0, blob.size() - 1));
    ExpectRejected(blob + '\0');
    ExpectRejected(LegacyRecord({Hash(1)}) + '\0');
}

TEST(CsnReplayData, EveryTruncationIsRejectedWithoutPublishingPartialData) {
    std::vector<std::string> fixtures{LegacyRecord({Hash(0), Hash(1)})};
    for (uint8_t version : {4, 5, 6}) {
        fixtures.push_back(OriginalCsn2Encoder({Hash(0), Hash(1)}, Metadata(), version));
    }
    for (const auto& blob : fixtures) {
        for (size_t length = 0; length < blob.size(); ++length) {
            SCOPED_TRACE(length);
            ExpectRejected(blob.substr(0, length));
        }
    }
}

TEST(CsnReplayData, UnknownMetadataVersionsAreRejectedEvenWithoutOutputs) {
    for (unsigned version = 0; version <= 255; ++version) {
        if (version == 4 || version == 5 || version == 6) continue;
        SCOPED_TRACE(version);
        ExpectRejected(OriginalCsn2Encoder({}, {}, static_cast<uint8_t>(version)));
        ExpectRejected(OriginalCsn2Encoder({}, Metadata(), static_cast<uint8_t>(version)));
    }
}

TEST(CsnReplayData, UnknownEnvelopeVersionsAreRejected) {
    for (const auto& magic : {"CSN1", "CSN3", "CSN0"}) {
        auto blob = OriginalCsn2Encoder({}, {}, 6);
        blob.replace(0, 4, magic);
        ExpectRejected(blob);
    }
}

TEST(CsnReplayData, CountsCannotExceedCapOrAvailableBytes) {
    for (uint32_t count : {1u, 1'000'000u, 1'000'001u,
                           std::numeric_limits<uint32_t>::max()}) {
        std::string legacy;
        AppendU32(legacy, count);
        ExpectRejected(legacy);
        auto blob = OriginalCsn2Encoder({}, {}, 6);
        SetU32(blob, 4, count);
        ExpectRejected(blob);
        blob = OriginalCsn2Encoder({}, {}, 6);
        SetU32(blob, 8, count);
        ExpectRejected(blob);
    }
}

TEST(CsnReplayData, MalformedScriptAndCommitmentLengthsAreRejected) {
    for (uint8_t version : {4, 5, 6}) {
        auto blob = OriginalCsn2Encoder({}, {Metadata().front()}, version);
        SetU32(blob, 13 + 8, std::numeric_limits<uint32_t>::max());
        ExpectRejected(blob);
        if (version >= 5) {
            blob = OriginalCsn2Encoder({}, {Metadata().front()}, version);
            const size_t commitment_length = 13 + 12 + 3 + (version >= 6 ? 5 : 0) + 1;
            SetU32(blob, commitment_length, std::numeric_limits<uint32_t>::max());
            ExpectRejected(blob);
        }
    }
}

TEST(CsnReplayData, EncoderRejectsInvalidTargetsAndUnknownVersions) {
    for (size_t size : {0u, 1u, 31u, 33u, 64u}) {
        EXPECT_THROW(SerializeCsnReplayData({UtreexoHash(size)}, {}, 6), std::invalid_argument);
    }
    for (uint8_t version : {0, 1, 2, 3, 7, 255}) {
        EXPECT_THROW(SerializeCsnReplayData({}, {}, version), std::invalid_argument);
    }
}

TEST(CsnReplayData, EncoderRejectsCountsAboveTheHistoricalCap) {
    const std::vector<UtreexoHash> targets(1'000'001);
    EXPECT_THROW(SerializeCsnReplayData(targets, {}, 6), std::length_error);
    const std::vector<SpentOutputData> spent(1'000'001);
    EXPECT_THROW(SerializeCsnReplayData({}, spent, 6), std::length_error);
}

TEST(CsnReplayData, VariableFieldsAreNotSubjectToUnrelatedProofLimits) {
    auto spent = Metadata();
    spent[0].scriptPubKey.assign(100'001, 0x51);
    spent[0].commitment.assign(100'003, 0x02);
    const auto blob = OriginalCsn2Encoder({Hash(1)}, spent, 6);
    CsnReplayData data;
    ASSERT_TRUE(DecodeCsnReplayData(blob, data));
    ExpectMetadata(data.spent_outputs, spent, 6);
    EXPECT_EQ(SerializeCsnReplayData(data.spend_targets, data.spent_outputs, 6), blob);
}
