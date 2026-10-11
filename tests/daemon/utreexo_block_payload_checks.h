#pragma once
#include "daemon/utreexo_block_payload.h"
namespace dinero {
namespace {
std::vector<uint8_t> PayloadEnvelope(uint8_t version,const Block& block,
    const std::vector<uint8_t>& batch,const std::vector<uint8_t>& transition={}) {
    std::vector<uint8_t> wire{version};const auto hash=block.GetHash();
    wire.insert(wire.end(),hash.data,hash.data+32);
    const auto number=[&](uint32_t n){for(unsigned i=0;i<4;++i)wire.push_back(uint8_t(n>>(8*i)));};
    number(0x01020304);wire.insert(wire.end(),32,0x5a);
    const auto body=block.Serialize();number(uint32_t(body.size()));wire.insert(wire.end(),body.begin(),body.end());
    number(uint32_t(batch.size()));wire.insert(wire.end(),batch.begin(),batch.end());
    if(version==3){number(uint32_t(transition.size()));wire.insert(wire.end(),transition.begin(),transition.end());}
    return wire;
}
}
TEST(UtreexoBlockPayload, SupportedEnvelopesOwnExactFields) {
    const auto block=SelectedGenesis();const auto body=block.Serialize();
    // Deliberately opaque bytes: this exercises framing, not proof validation.
    for(const auto version:{uint8_t(2),uint8_t(3)}) {
        auto wire=PayloadEnvelope(version,block,{1,2,3},version==3?std::vector<uint8_t>{4,5}:std::vector<uint8_t>{});
        auto decoded=DecodeUtreexoBlockPayload(wire);ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->height,0x01020304u);EXPECT_EQ(decoded->hash,block.GetHash());
        EXPECT_EQ(decoded->block_bytes,std::vector<uint8_t>(body.begin(),body.end()));EXPECT_EQ(decoded->root_after,std::vector<uint8_t>(32,0x5a));
        EXPECT_EQ(decoded->batch_proof,(std::vector<uint8_t>{1,2,3}));
        EXPECT_EQ(decoded->transition_proof,version==3?(std::vector<uint8_t>{4,5}):std::vector<uint8_t>{});
        std::fill(wire.begin(),wire.end(),0);EXPECT_EQ(decoded->block_bytes,std::vector<uint8_t>(body.begin(),body.end()));EXPECT_EQ(decoded->batch_proof,(std::vector<uint8_t>{1,2,3}));
    }
    EXPECT_TRUE(DecodeUtreexoBlockPayload(PayloadEnvelope(3,block,{}, {7})));
    EXPECT_FALSE(DecodeUtreexoBlockPayload(PayloadEnvelope(2,block,{})));
    EXPECT_FALSE(DecodeUtreexoBlockPayload(PayloadEnvelope(3,block,{},{})));
}
TEST(UtreexoBlockPayload, TruncationTrailingAndUnknownVersionsRefuse) {
    const auto block=SelectedGenesis();const auto good=PayloadEnvelope(3,block,{1,2},{3,4});
    for(size_t size=0;size<good.size();++size)EXPECT_FALSE(DecodeUtreexoBlockPayload(std::span<const uint8_t>(good).first(size)))<<size;
    auto extra=good;extra.push_back(0);EXPECT_FALSE(DecodeUtreexoBlockPayload(extra));
    for(const auto version:{uint8_t(0),uint8_t(1),uint8_t(4),uint8_t(255)}) {
        auto wrong=good;wrong[0]=version;EXPECT_FALSE(DecodeUtreexoBlockPayload(wrong));
    }
    auto old=PayloadEnvelope(2,block,{1});old.push_back(0);EXPECT_FALSE(DecodeUtreexoBlockPayload(old));
}
TEST(UtreexoBlockPayload, LengthLimitsAndWrongBodyIdentityRefuse) {
    const auto block=SelectedGenesis();const auto good=PayloadEnvelope(3,block,{1},{2});
    const auto set=[&](size_t offset,uint32_t n) {
        auto wire=good;for(unsigned i=0;i<4;++i)wire[offset+i]=uint8_t(n>>(8*i));return wire;
    };
    const auto body_size=block.Serialize().size();
    EXPECT_FALSE(DecodeUtreexoBlockPayload(set(69,4*1024*1024+1)));
    EXPECT_FALSE(DecodeUtreexoBlockPayload(set(73+body_size,1024*1024+1)));
    EXPECT_FALSE(DecodeUtreexoBlockPayload(set(78+body_size,512*1024+1)));
    EXPECT_FALSE(DecodeUtreexoBlockPayload(set(69,0)));
    auto wrong=good;wrong[1]^=1;EXPECT_FALSE(DecodeUtreexoBlockPayload(wrong));
    EXPECT_FALSE(DecodeUtreexoBlockPayload(std::vector<uint8_t>(5*1024*1024+1)));
    EXPECT_TRUE(DecodeUtreexoBlockPayload(good));
}

TEST(UtreexoBlockPayload, BlockCountEncodingsPreserveExistingRoundTrip) {
    const auto block=SelectedGenesis();ASSERT_EQ(block.vtx.size(),1u);
    const auto encoded=block.Serialize();const std::vector<uint8_t> canonical(encoded.begin(),encoded.end());
    ASSERT_GT(canonical.size(),129u);ASSERT_EQ(canonical[128],1u);
    // Preserve the existing decoder's accepted CompactSize encodings. This
    // allocation change must not introduce a new canonical-encoding rule.
    const std::vector<std::vector<uint8_t>> counts={{1},{0xfd,1,0},{0xfe,1,0,0,0},{0xff,1,0,0,0,0,0,0,0}};
    for(const auto& count:counts) {
        std::vector<uint8_t> wire(canonical.begin(),canonical.begin()+128);
        wire.insert(wire.end(),count.begin(),count.end());
        wire.insert(wire.end(),canonical.begin()+129,canonical.end());
        const auto decoded=Block::Deserialize(wire);ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->Serialize(),encoded);
        // The optional absent-Utreexo flag may still be omitted by old data.
        ASSERT_EQ(wire.back(),0u);wire.pop_back();
        const auto legacy=Block::Deserialize(wire);ASSERT_TRUE(legacy);
        EXPECT_EQ(legacy->Serialize(),encoded);
    }
    auto two=block;two.vtx.push_back(block.vtx.front());
    const auto two_encoded=two.Serialize();
    const auto decoded_two=Block::Deserialize(std::vector<uint8_t>(two_encoded.begin(),two_encoded.end()));
    ASSERT_TRUE(decoded_two);EXPECT_EQ(decoded_two->Serialize(),two_encoded);
}
TEST(UtreexoBlockPayload, SmallIncompleteBlockCountsRefuse) {
    const auto block=SelectedGenesis();const auto encoded=block.Serialize();
    const std::vector<uint8_t> good(encoded.begin(),encoded.end());
    ASSERT_GT(good.size(),130u);ASSERT_EQ(good[128],1u);
    // Small benign inputs only: no large allocations or exhaustion probes.
    for(const auto& count:std::vector<std::vector<uint8_t>>{{1},{2},{0xfd},{0xfd,1},{0xfe,1,0},{0xff,1,0,0}}) {
        std::vector<uint8_t> wire(good.begin(),good.begin()+128);
        wire.insert(wire.end(),count.begin(),count.end());
        EXPECT_FALSE(Block::Deserialize(wire));
    }
    auto missing_second=good;missing_second[128]=2;
    EXPECT_FALSE(Block::Deserialize(missing_second));
    std::vector<uint8_t> no_body(good.begin(),good.begin()+130);
    EXPECT_FALSE(Block::Deserialize(no_body));
    EXPECT_TRUE(Block::Deserialize(good));
}
} // namespace dinero
