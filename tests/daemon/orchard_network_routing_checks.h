#pragma once
#include "daemon/orchard_network_block.h"
namespace dinero {
TEST(OrchardNetworkRouting, UnsetProfileAndMissingOwnerAreDistinct) {
    const auto params=Params();auto* context=DaemonContext::instance();
    struct Restore {ChainParams params;DaemonContext* context;~Restore(){MutableParams()=params;DaemonContext::setInstance(context);}} restore{params,context};
    MutableParams().orchard_activation_height=UINT32_MAX;MutableParams().orchard_branch_id=0;
    EXPECT_EQ(ClassifyNetworkBlock({}).family,NetworkBlockFamily::Historical);
    MutableParams().orchard_activation_height=102;MutableParams().orchard_branch_id=1;
    DaemonContext::setInstance(nullptr);
    EXPECT_EQ(ClassifyNetworkBlock({}).family,NetworkBlockFamily::Unavailable);
    const auto raw=SelectedGenesis().header.SerializeForHash();const std::vector<uint8_t> bytes(raw.begin(),raw.end());
    EXPECT_EQ(ClassifyNetworkBlock(bytes).family,NetworkBlockFamily::Unavailable);
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",bytes,false),OrchardNetworkDisposition::Refused);
    EXPECT_FALSE(AcceptDownloadedOrchardBlock(nullptr,nullptr,nullptr,bytes,uint256{},0));
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct NetworkRoutingFixture : OrchardRelayFixture {
    std::shared_ptr<consensus::HeaderChainSelector> headers=std::make_shared<consensus::HeaderChainSelector>();
    std::shared_ptr<BlockDownloadScheduler> parallel=std::make_shared<BlockDownloadScheduler>([](auto,const auto&){return true;});
    std::shared_ptr<BlockRelayManager> relay=std::make_shared<BlockRelayManager>(nullptr,parallel);
    std::shared_ptr<consensus::BlockDownloadScheduler> downloads;
    NetworkRoutingFixture() {
        relay->SetOrchardBlockIngress(f.service,owner);context.block_relay=relay;
        context.parallel_block_download=parallel;
        parallel->registerPeers({"peer"});
    }
    void Enroll(const BlockHeader& child,bool request=true) {
        for(const auto& block:f.blocks)OrchardAdmissionFixture::Require(headers->AddHeader(block.header));
        OrchardAdmissionFixture::Require(headers->AddHeader(child));context.header_chain=headers;
        downloads=std::make_shared<consensus::BlockDownloadScheduler>(headers.get(),files.get());
        context.block_download=downloads;downloads->SetLocalTipHeight(101);
        downloads->SetSendGetDataCallback([](const uint256&,uint32_t){});
        downloads->SetConnectBlockBytesCallback([this](const auto& bytes,const auto& hash,uint32_t height,const auto&){
            return AcceptDownloadedOrchardBlock(f.service,owner,parallel,bytes,hash,height)
                ? consensus::ConnectBlockResult::CONNECTED : consensus::ConnectBlockResult::TEMPORARY_FAIL;
        });
        if(request){downloads->OnHeadersProcessed();downloads->Tick();}
    }
    void RequestParallel(const uint256& hash) {parallel->scheduleBlock(hash,102,"peer");parallel->processQueue();}
    ~NetworkRoutingFixture(){context.block_download.reset();context.header_chain.reset();context.block_relay.reset();context.parallel_block_download.reset();}
};
}
TEST(OrchardNetworkRouting, DirectTypedRouteUsesActualCanonicalQueue) {
    NetworkRoutingFixture f;const auto block=f.Build();ASSERT_TRUE(block);const auto hash=block->Header().GetHash();
    const auto classification=ClassifyNetworkBlock(block->WireBytes());
    ASSERT_EQ(classification.family,NetworkBlockFamily::Orchard);EXPECT_EQ(classification.height,102u);
    f.RequestParallel(hash);ASSERT_TRUE(f.parallel->isInFlight(hash));
    f.f.service->setRuntimeBlockNotifications(nullptr);
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",block->WireBytes(),false),OrchardNetworkDisposition::Refused);
    EXPECT_TRUE(f.parallel->isInFlight(hash));EXPECT_EQ(f.notices->published,0u);EXPECT_EQ(f.relay->GetStats().blocks_validated,0u);
    f.f.service->setRuntimeBlockNotifications(f.notices);
    ASSERT_EQ(ReceiveOrchardNetworkBlock("peer",block->WireBytes(),false),OrchardNetworkDisposition::Connected);
    EXPECT_EQ(f.parallel->getStats().completed_blocks,1u);EXPECT_EQ(f.notices->published,1u);
    EXPECT_EQ(f.relay->GetPeerPerformance("peer").blocks_delivered,1u);
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    const auto stored=f.f.service->getBlockRpcSnapshot(hash);ASSERT_TRUE(stored.ok());EXPECT_EQ(stored->bytes,block->WireBytes());
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",block->WireBytes(),false),OrchardNetworkDisposition::Connected);
    EXPECT_EQ(f.parallel->getStats().completed_blocks,1u);EXPECT_EQ(f.notices->published,1u);
}
TEST(OrchardNetworkRouting, StoredBodyDoesNotCompleteParallelDownload) {
    NetworkRoutingFixture f;const auto block=f.Build();ASSERT_TRUE(block);const auto hash=block->Header().GetHash();
    f.Enroll(block->Header());f.RequestParallel(hash);ASSERT_TRUE(f.parallel->isInFlight(hash));
    f.f.service->setRuntimeBlockNotifications(nullptr);
    ASSERT_EQ(ReceiveOrchardNetworkBlock("peer",block->WireBytes(),false),OrchardNetworkDisposition::Stored);
    EXPECT_TRUE(f.downloads->HasReceivedBlock(hash));EXPECT_EQ(f.downloads->GetLocalTipHeight(),101u);
    EXPECT_TRUE(f.parallel->isInFlight(hash));EXPECT_EQ(f.parallel->getStats().completed_blocks,0u);
    EXPECT_EQ(f.notices->published,0u);EXPECT_EQ(f.relay->GetPeerPerformance("peer").blocks_delivered,0u);
    f.f.service->setRuntimeBlockNotifications(f.notices);f.downloads->Tick();
    EXPECT_EQ(f.downloads->GetLocalTipHeight(),102u);EXPECT_EQ(f.parallel->getStats().completed_blocks,1u);
    EXPECT_FALSE(f.parallel->isInFlight(hash));EXPECT_EQ(f.notices->published,1u);
    // No fabricated peer attribution for the later scheduler-owned drain.
    EXPECT_EQ(f.relay->GetPeerPerformance("peer").blocks_delivered,0u);
}
TEST(OrchardNetworkRouting, FamilyFramingAndStatelessRefuseBeforeCompletion) {
    NetworkRoutingFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    const auto old=f.f.blocks.back().Serialize();const std::vector<uint8_t> historical(old.begin(),old.end());
    ASSERT_EQ(ClassifyNetworkBlock(historical).family,NetworkBlockFamily::Historical);
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",historical,false),OrchardNetworkDisposition::Refused);
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",block->WireBytes(),true),OrchardNetworkDisposition::Refused);
    auto unknown=block->Header();unknown.prev_block_hash=uint256::FromHexUnsafe(std::string(64,'7'));
    const auto head=unknown.SerializeForHash();auto bytes=block->WireBytes();std::copy(head.begin(),head.end(),bytes.begin());
    EXPECT_EQ(ClassifyNetworkBlock(bytes).family,NetworkBlockFamily::Unavailable);
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",bytes,false),OrchardNetworkDisposition::Refused);
    EXPECT_EQ(f.queue->getMetrics().blocks_submitted.load(),0u);
    auto trailing=block->WireBytes();trailing.push_back(0);
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",trailing,false),OrchardNetworkDisposition::Refused);
    EXPECT_EQ(f.notices->published,0u);EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);
    ASSERT_EQ(ReceiveOrchardNetworkBlock("peer",block->WireBytes(),false),OrchardNetworkDisposition::Connected);
}
TEST(OrchardNetworkRouting, SyncPolicyAndExactDownloadOwnerRemainRequired) {
    NetworkRoutingFixture f;const auto block=f.Build();ASSERT_TRUE(block);const auto hash=block->Header().GetHash();
    f.Enroll(block->Header(),false);
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",block->WireBytes(),false),OrchardNetworkDisposition::Refused);
    EXPECT_EQ(f.queue->getMetrics().blocks_submitted.load(),0u);
    EXPECT_FALSE(AcceptDownloadedOrchardBlock(f.f.service,f.owner,f.parallel,block->WireBytes(),hash,103));
    EXPECT_FALSE(AcceptDownloadedOrchardBlock(f.f.service,f.owner,nullptr,block->WireBytes(),hash,102));
    f.owner->Stop();EXPECT_FALSE(AcceptDownloadedOrchardBlock(f.f.service,f.owner,f.parallel,block->WireBytes(),hash,102));
    ASSERT_TRUE(f.owner->Start());f.downloads->OnHeadersProcessed();f.downloads->Tick();
    ASSERT_EQ(ReceiveOrchardNetworkBlock("peer",block->WireBytes(),false),OrchardNetworkDisposition::Stored);
    EXPECT_EQ(f.downloads->GetLocalTipHeight(),102u);EXPECT_EQ(f.notices->published,1u);
}
#endif
} // namespace dinero
