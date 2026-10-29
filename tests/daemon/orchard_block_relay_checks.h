#pragma once
#include "daemon/block_relay_manager.h"
namespace dinero {
TEST(OrchardBlockRelay, MissingAndExpiredOwnersNeverAcknowledge) {
    BlockRelayManager relay(nullptr);unsigned historical=0;
    relay.SetValidateBlockCallback([&](const Block&,const std::string&){++historical;return BlockRelayManager::BlockValidationOutcome::Accepted;});
    EXPECT_FALSE(relay.HandleOrchardBlock("peer",{}));
    auto source=std::make_shared<ChainstateService>();auto ingress=std::make_shared<BlockIngressService>();
    relay.SetOrchardBlockIngress(source,ingress);source.reset();ingress.reset();
    EXPECT_FALSE(relay.HandleOrchardBlock("peer",std::vector<uint8_t>(128,0)));
    EXPECT_EQ(relay.GetSeenBlockCount(),0u);EXPECT_EQ(relay.GetStats().blocks_validated,0u);
    EXPECT_EQ(relay.GetStats().blocks_rejected,0u);EXPECT_EQ(historical,0u);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct OrchardRelayFixture : QueuedIngressFixture {
    std::shared_ptr<BlockIngressService> owner=std::make_shared<BlockIngressService>();
    IBlockIngress* prior_ingress=nullptr;
    OrchardRelayFixture() {
        OrchardAdmissionFixture::Require(owner->Init(context));OrchardAdmissionFixture::Require(owner->Start());
        prior_ingress=context.block_ingress;context.block_ingress=owner.get();
    }
    ~OrchardRelayFixture(){context.block_ingress=prior_ingress;if(owner)owner->Stop();}
};
}
TEST(OrchardBlockRelay, CanonicalResultCompletesRealDownload) {
    OrchardRelayFixture f;const auto block=f.Build();ASSERT_TRUE(block);const auto hash=block->Header().GetHash();
    unsigned requests=0;auto downloads=std::make_shared<BlockDownloadScheduler>([&](auto,const auto& requested){EXPECT_EQ(requested,hash);++requests;return true;});
    downloads->registerPeers({"peer"});BlockRelayManager relay(nullptr,downloads);
    relay.SetOrchardBlockIngress(f.f.service,f.owner);downloads->scheduleBlock(hash,102,"peer");downloads->processQueue();
    ASSERT_EQ(requests,1u);ASSERT_TRUE(downloads->isInFlight(hash));
    f.f.service->setRuntimeBlockNotifications(nullptr);
    EXPECT_FALSE(relay.HandleOrchardBlock("peer",block->WireBytes()));
    EXPECT_TRUE(downloads->isInFlight(hash));EXPECT_EQ(downloads->getStats().completed_blocks,0u);
    EXPECT_FALSE(relay.IsBlockSeen(hash));EXPECT_EQ(relay.GetStats().blocks_validated,0u);
    EXPECT_EQ(relay.GetPeerPerformance("peer").blocks_delivered,0u);EXPECT_EQ(relay.GetPeerPerformance("peer").blocks_failed,0u);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);
    f.f.service->setRuntimeBlockNotifications(f.notices);
    ASSERT_TRUE(relay.HandleOrchardBlock("peer",block->WireBytes()));
    EXPECT_FALSE(downloads->isInFlight(hash));EXPECT_EQ(downloads->getStats().completed_blocks,1u);
    EXPECT_TRUE(relay.IsBlockSeen(hash));EXPECT_EQ(relay.GetStats().blocks_validated,1u);
    EXPECT_EQ(relay.GetPeerPerformance("peer").blocks_delivered,1u);EXPECT_EQ(f.notices->published,1u);
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    const auto stored=f.f.service->getBlockRpcSnapshot(hash);ASSERT_TRUE(stored.ok());EXPECT_EQ(stored->bytes,block->WireBytes());
    ASSERT_TRUE(relay.HandleOrchardBlock("peer",block->WireBytes()));
    EXPECT_EQ(relay.GetStats().blocks_validated,1u);EXPECT_EQ(relay.GetPeerPerformance("peer").blocks_delivered,1u);
    EXPECT_EQ(downloads->getStats().completed_blocks,1u);EXPECT_EQ(f.notices->published,1u);
}
TEST(OrchardBlockRelay, SeenHashNeverBypassesCurrentOwner) {
    OrchardRelayFixture f;const auto block=f.Build();ASSERT_TRUE(block);BlockRelayManager relay(nullptr);
    relay.SetOrchardBlockIngress(f.f.service,f.owner);ASSERT_TRUE(relay.HandleOrchardBlock("peer",block->WireBytes()));
    ASSERT_TRUE(relay.IsBlockSeen(block->Header().GetHash()));f.queue->stop();
    EXPECT_FALSE(relay.HandleOrchardBlock("peer",block->WireBytes()));
    EXPECT_EQ(relay.GetStats().blocks_validated,1u);EXPECT_EQ(f.notices->published,1u);
    f.StartQueue(consensus::ValidationQueue::Config::forNormalOperation());f.owner->Stop();
    EXPECT_FALSE(relay.HandleOrchardBlock("peer",block->WireBytes()));
    ASSERT_TRUE(f.owner->Start());ASSERT_TRUE(relay.HandleOrchardBlock("peer",block->WireBytes()));
    EXPECT_EQ(relay.GetStats().blocks_validated,1u);EXPECT_EQ(f.notices->published,1u);
    f.context.block_ingress=nullptr;f.owner->Stop();f.owner.reset();
    EXPECT_FALSE(relay.HandleOrchardBlock("peer",block->WireBytes()));
    EXPECT_EQ(relay.GetPeerPerformance("peer").blocks_delivered,1u);EXPECT_EQ(relay.GetPeerPerformance("peer").blocks_failed,0u);
}
TEST(OrchardBlockRelay, HistoricalAndMalformedBodiesHaveNoCompletion) {
    OrchardRelayFixture f;const auto block=f.Build();ASSERT_TRUE(block);BlockRelayManager relay(nullptr);
    relay.SetOrchardBlockIngress(f.f.service,f.owner);unsigned historical=0;
    relay.SetValidateBlockCallback([&](const Block&,const auto&){++historical;return BlockRelayManager::BlockValidationOutcome::Accepted;});
    const auto old=f.f.blocks.back().Serialize();
    EXPECT_FALSE(relay.HandleOrchardBlock("peer",std::vector<uint8_t>(old.begin(),old.end())));
    EXPECT_EQ(f.queue->getMetrics().blocks_submitted.load(),0u);
    auto trailing=block->WireBytes();trailing.push_back(0);EXPECT_FALSE(relay.HandleOrchardBlock("peer",trailing));
    EXPECT_EQ(relay.GetSeenBlockCount(),0u);EXPECT_EQ(relay.GetStats().blocks_validated,0u);
    EXPECT_EQ(relay.GetPeerPerformance("peer").blocks_failed,0u);EXPECT_EQ(historical,0u);
    EXPECT_EQ(f.notices->published,0u);EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);
    EXPECT_EQ(f.f.db.getHeader(block->Header().GetHash()).status(),Status::NotFound);
    ASSERT_TRUE(relay.HandleOrchardBlock("peer",block->WireBytes()));EXPECT_EQ(relay.GetStats().blocks_validated,1u);
}
#endif
} // namespace dinero
