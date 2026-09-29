#pragma once
#include "daemon/block_relay_manager.h"
namespace dinero {
namespace {
struct ServedBlockMessages {
    struct Message {std::string peer,command;std::vector<uint8_t> bytes;};
    std::vector<Message> messages;
    void Bind(BlockRelayManager& relay) {
        relay.SetSendMessageCallback([this](const auto& peer,const auto& command,const auto& bytes) {
            messages.push_back({peer,command,bytes});
        });
    }
};
}
TEST(OrchardBlockServing, MissingAndExpiredOwnerNeverUseHistoricalFallback) {
    BlockRelayManager relay(nullptr);ServedBlockMessages sent;sent.Bind(relay);
    unsigned legacy_reads=0;
    relay.SetRetrieveBlockCallback([&](const uint256&,Block&){++legacy_reads;return false;});
    relay.SetFullBlockSource({});relay.HandleGetData("peer",uint256{});
    ASSERT_EQ(sent.messages.size(),1u);EXPECT_EQ(sent.messages[0].command,"notfound");
    auto source=std::make_shared<ChainstateService>();relay.SetFullBlockSource(source);source.reset();
    relay.HandleGetData("peer",uint256{});
    ASSERT_EQ(sent.messages.size(),2u);EXPECT_EQ(sent.messages[1].command,"notfound");
    EXPECT_EQ(legacy_reads,0u);EXPECT_EQ(relay.BlocksServed24h(),0u);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardBlockServing, ExactTypedWireReopenAndUnavailableStorage) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());
    BlockRelayManager relay(nullptr);relay.SetFullBlockSource(f.f.service);
    unsigned legacy_reads=0;relay.SetRetrieveBlockCallback([&](const uint256&,Block&){++legacy_reads;return false;});
    ServedBlockMessages sent;sent.Bind(relay);const auto hash=block->Header().GetHash();
    relay.HandleGetData("peer-a",hash);ASSERT_EQ(sent.messages.size(),1u);
    EXPECT_EQ(sent.messages.back().peer,"peer-a");EXPECT_EQ(sent.messages.back().command,"block");
    EXPECT_EQ(sent.messages.back().bytes,block->WireBytes());EXPECT_EQ(relay.BlocksServed24h(),1u);
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    relay.HandleGetData("peer-b",hash);ASSERT_EQ(sent.messages.size(),2u);
    EXPECT_EQ(sent.messages.back().bytes,block->WireBytes());EXPECT_EQ(relay.BlocksServed24h(),2u);
    // Network callbacks run after capture; replacing storage availability here
    // cannot change the already owned response or its captured height.
    relay.SetSendMessageCallback([&](const auto& peer,const auto& command,const auto& bytes) {
        f.f.service->setBlockStorage(nullptr);sent.messages.push_back({peer,command,bytes});
    });
    relay.HandleGetData("peer-c",hash);ASSERT_EQ(sent.messages.size(),3u);
    EXPECT_EQ(sent.messages.back().command,"block");EXPECT_EQ(sent.messages.back().bytes,block->WireBytes());
    sent.Bind(relay);relay.HandleGetData("peer-d",hash);ASSERT_EQ(sent.messages.size(),4u);
    EXPECT_EQ(sent.messages.back().command,"notfound");EXPECT_EQ(relay.BlocksServed24h(),3u);
    f.f.service->setBlockStorage(f.files);
    const auto metadata=f.f.db.getHeaderMetadata(hash);ASSERT_TRUE(metadata.ok());auto wrong=*metadata;wrong.parent_hash=uint256{};
    ASSERT_EQ(f.f.db.putHeaderMetadata(f.f.token,hash,wrong),Status::Ok);
    relay.HandleGetData("peer-e",hash);EXPECT_EQ(sent.messages.back().command,"notfound");
    EXPECT_EQ(relay.BlocksServed24h(),3u);EXPECT_EQ(legacy_reads,0u);
    EXPECT_EQ(f.f.service->GetActiveTip()->hash,hash);EXPECT_EQ(f.notices->published,1u);
}
TEST(OrchardBlockServing, HistoricalFlatfileAndRetainedDisconnectedBody) {
    CanonicalPoolFixture f;BlockRelayManager relay(nullptr);relay.SetFullBlockSource(f.f.service);
    ServedBlockMessages sent;sent.Bind(relay);const auto& historical=f.f.blocks[2];const auto hash=historical.GetHash();
    relay.HandleGetData("peer",hash);ASSERT_EQ(sent.messages.size(),1u);EXPECT_EQ(sent.messages.back().command,"notfound");
    const auto location=f.files->writeBlock(hash,historical);ASSERT_TRUE(location.ok());const auto work=f.f.db.getBlockWork(hash);ASSERT_TRUE(work.ok());
    ChainDB::PersistedHeaderMetadata metadata;metadata.height=2;metadata.parent_hash=historical.header.prev_block_hash;metadata.chainwork=*work;
    metadata.status_flags=BLOCK_HAVE_DATA|BLOCK_VALID_CHAIN|BLOCK_VALID_SCRIPTS;
    metadata.file_number=location->file_number;metadata.data_pos=location->offset;metadata.data_size=location->size;
    ASSERT_EQ(f.f.db.putHeaderMetadata(f.f.token,hash,metadata),Status::Ok);
    relay.HandleGetData("peer",hash);EXPECT_EQ(sent.messages.back().command,"block");
    const auto encoded=historical.Serialize();EXPECT_EQ(sent.messages.back().bytes,std::vector<uint8_t>(encoded.begin(),encoded.end()));
    const auto block=f.Build();ASSERT_TRUE(block);ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    relay.HandleGetData("peer",block->Header().GetHash());EXPECT_EQ(sent.messages.back().command,"block");
    EXPECT_EQ(sent.messages.back().bytes,block->WireBytes());EXPECT_EQ(relay.BlocksServed24h(),2u);
}
#endif
} // namespace dinero
