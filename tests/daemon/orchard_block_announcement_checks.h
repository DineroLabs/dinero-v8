#pragma once
#include "daemon/orchard_network_block.h"
namespace dinero {
TEST(OrchardBlockAnnouncement, MissingOwnerHasNoTransportHandoff) {
    BlockRelayManager relay(nullptr);unsigned sent=0;
    relay.SetSendMessageCallback([&](const auto&,const auto&,const auto&){++sent;});
    EXPECT_FALSE(relay.AnnounceOrchardBlock(uint256{},102));
    const auto result=BlockAcceptResult::Accepted(uint256{},102);
    EXPECT_FALSE(AnnounceAcceptedOrchardBlock(nullptr,result));
    ChainstateService source;EXPECT_FALSE(source.getOrchardAnnouncementSnapshot(uint256{},102).ok());
    EXPECT_EQ(sent,0u);EXPECT_EQ(relay.GetStats().blocks_relayed,0u);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct AnnouncementMessages {
    struct Message{std::string peer,command;std::vector<uint8_t> bytes;};
    std::vector<Message> values;
    void Bind(BlockRelayManager& relay){relay.SetSendMessageCallback([this](const auto& peer,const auto& command,const auto& bytes){values.push_back({peer,command,bytes});});}
    void CheckInventory(size_t index,const uint256& hash)const {
        ASSERT_LT(index,values.size());const auto& value=values[index];
        EXPECT_TRUE(value.peer.empty());EXPECT_EQ(value.command,"inv_all");ASSERT_EQ(value.bytes.size(),37u);
        EXPECT_EQ(std::vector<uint8_t>(value.bytes.begin(),value.bytes.begin()+5),(std::vector<uint8_t>{1,2,0,0,0}));
        EXPECT_EQ(std::vector<uint8_t>(value.bytes.begin()+5,value.bytes.end()),std::vector<uint8_t>(hash.begin(),hash.end()));
    }
};
}
TEST(OrchardBlockAnnouncement, QueueHandoffAndExactGetData) {
    NetworkRoutingFixture f;AnnouncementMessages sent;sent.Bind(*f.relay);f.relay->SetFullBlockSource(f.f.service);
    const auto block=f.Build();ASSERT_TRUE(block);const auto hash=block->Header().GetHash();
    f.f.service->setRuntimeBlockNotifications(nullptr);
    EXPECT_FALSE(f.Receive(block->WireBytes()).accepted());EXPECT_TRUE(sent.values.empty());
    f.f.service->setRuntimeBlockNotifications(f.notices);
    const auto accepted=f.Receive(block->WireBytes());ASSERT_TRUE(accepted.accepted())<<accepted.reason;
    EXPECT_TRUE(accepted.connected);EXPECT_TRUE(accepted.relayed);ASSERT_EQ(sent.values.size(),1u);sent.CheckInventory(0,hash);
    EXPECT_EQ(f.relay->GetStats().blocks_relayed,1u);EXPECT_EQ(f.notices->published,1u);
    const auto duplicate=f.Receive(block->WireBytes());EXPECT_TRUE(duplicate.accepted());EXPECT_TRUE(duplicate.relayed);
    EXPECT_EQ(sent.values.size(),1u);EXPECT_EQ(f.relay->GetStats().blocks_relayed,1u);
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    f.relay->HandleGetData("peer",hash);ASSERT_EQ(sent.values.size(),2u);
    EXPECT_EQ(sent.values.back().command,"block");EXPECT_EQ(sent.values.back().bytes,block->WireBytes());
}
TEST(OrchardBlockAnnouncement, TransportExceptionKeepsAcceptanceAndRetry) {
    NetworkRoutingFixture f;const auto block=f.Build();ASSERT_TRUE(block);const auto hash=block->Header().GetHash();unsigned attempts=0;
    f.relay->SetSendMessageCallback([&](const auto&,const auto&,const auto&){++attempts;throw std::runtime_error("isolated transport refusal");});
    const auto accepted=f.Receive(block->WireBytes());ASSERT_TRUE(accepted.accepted())<<accepted.reason;
    EXPECT_TRUE(accepted.connected);EXPECT_FALSE(accepted.relayed);EXPECT_EQ(attempts,1u);
    EXPECT_EQ(f.f.service->GetActiveTip()->hash,hash);EXPECT_EQ(f.notices->published,1u);EXPECT_EQ(f.relay->GetStats().blocks_relayed,0u);
    // Positive same-thread callback checks: no selected lock and recursive
    // announce is refused while this handoff is pending, without another send.
    f.relay->SetSendMessageCallback([&](const auto&,const auto& command,const auto&){
        ++attempts;EXPECT_EQ(command,"inv_all");EXPECT_TRUE(f.f.service->getOrchardAnnouncementSnapshot(hash,102).ok());
        EXPECT_FALSE(f.relay->AnnounceOrchardBlock(hash,102));
    });
    const auto retry=f.Submit(block->WireBytes());ASSERT_TRUE(retry.accepted())<<retry.reason;
    EXPECT_TRUE(retry.relayed);EXPECT_EQ(attempts,2u);EXPECT_EQ(f.notices->published,1u);
    EXPECT_TRUE(f.relay->AnnounceOrchardBlock(hash,102));EXPECT_EQ(attempts,2u);EXPECT_EQ(f.relay->GetStats().blocks_relayed,1u);
}
TEST(OrchardBlockAnnouncement, CaptureRequiresUnlockedCurrentCanonicalOwner) {
    NetworkRoutingFixture f;const auto block=f.Build();ASSERT_TRUE(block);const auto hash=block->Header().GetHash();
    ASSERT_TRUE(f.Receive(block->WireBytes()).accepted());AnnouncementMessages sent;sent.Bind(*f.relay);
    EXPECT_FALSE(f.relay->AnnounceOrchardBlock(hash,103));
    EXPECT_FALSE(f.relay->AnnounceOrchardBlock(f.parent->hash,101));
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        EXPECT_EQ(f.f.service->getOrchardAnnouncementSnapshot(hash,102).status(),Status::Invalid);
        EXPECT_FALSE(f.relay->AnnounceOrchardBlock(hash,102));
    }
    EXPECT_TRUE(sent.values.empty());
    f.context.block_relay.reset();EXPECT_FALSE(f.relay->AnnounceOrchardBlock(hash,102));f.context.block_relay=f.relay;
    f.relay->SetOrchardBlockIngress({},{});EXPECT_FALSE(f.relay->AnnounceOrchardBlock(hash,102));
    f.relay->SetOrchardBlockIngress(f.f.service,f.owner);
    ASSERT_TRUE(f.relay->AnnounceOrchardBlock(hash,102));ASSERT_EQ(sent.values.size(),1u);sent.CheckInventory(0,hash);
}
TEST(OrchardBlockAnnouncement, ReconnectSameHashHasNewCanonicalCursor) {
    NetworkRoutingFixture f;AnnouncementMessages sent;sent.Bind(*f.relay);const auto block=f.Build();ASSERT_TRUE(block);const auto hash=block->Header().GetHash();
    ASSERT_TRUE(f.Receive(block->WireBytes()).accepted());const auto before=f.f.service->getOrchardAnnouncementSnapshot(hash,102);ASSERT_TRUE(before.ok());
    ASSERT_EQ(sent.values.size(),1u);auto* child=f.f.service->GetActiveTip();
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,child));
    EXPECT_FALSE(f.relay->AnnounceOrchardBlock(hash,102));EXPECT_EQ(sent.values.size(),1u);
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    const auto again=f.Receive(block->WireBytes());ASSERT_TRUE(again.accepted())<<again.reason;EXPECT_TRUE(again.relayed);
    const auto after=f.f.service->getOrchardAnnouncementSnapshot(hash,102);ASSERT_TRUE(after.ok());
    EXPECT_GT(after->event_sequence,before->event_sequence);EXPECT_NE(after->event_digest,before->event_digest);
    ASSERT_EQ(sent.values.size(),2u);sent.CheckInventory(1,hash);EXPECT_EQ(f.relay->GetStats().blocks_relayed,2u);
}
TEST(OrchardBlockAnnouncement, MiningSubmitReleasesOuterParentGuard) {
    OrchardRpcMiningFixture f;auto relay=std::make_shared<BlockRelayManager>(nullptr);
    relay->SetOrchardBlockIngress(f.raw.f.service,{});f.raw.context.block_relay=relay;
    const auto job=f.Job();ASSERT_FALSE(job.isMember("error"))<<job["error"].asString();
    auto header=f.Header(job);header.nonce=f.Nonce(job);const auto hash=header.GetHash();unsigned sent=0;
    relay->SetSendMessageCallback([&](const auto&,const auto& command,const auto&){
        ++sent;EXPECT_EQ(command,"inv_all");EXPECT_TRUE(f.raw.f.service->getOrchardAnnouncementSnapshot(hash,102).ok());
    });
    ASSERT_TRUE(f.Submit(job,header.nonce).isNull());EXPECT_EQ(sent,1u);EXPECT_EQ(relay->GetStats().blocks_relayed,1u);
    EXPECT_EQ(f.Submit(job,header.nonce)["code"].asString(),"stale-job");EXPECT_EQ(sent,1u);
    EXPECT_EQ(f.raw.f.service->GetActiveTip()->hash,hash);EXPECT_EQ(f.raw.notices->published,1u);
}
#endif
} // namespace dinero
