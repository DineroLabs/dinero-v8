#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "daemon/block_relay_manager.h"
#include "consensus/block_download_scheduler.h"
#include "p2p/block_download_scheduler.h"
namespace {
// Real service, queue, relay and download schedulers; transport sends are
// captured locally. No sockets, IPC or DaemonApp startup is exercised.
struct CompactNetworkFixture {
    CompactQueuedIngressFixture selected;
    std::shared_ptr<dinero::BlockDownloadScheduler> parallel=std::make_shared<dinero::BlockDownloadScheduler>([](auto,const auto&){return true;});
    std::shared_ptr<BlockRelayManager> relay=std::make_shared<BlockRelayManager>(nullptr,parallel);
    std::shared_ptr<consensus::HeaderChainSelector> headers;
    std::shared_ptr<consensus::BlockDownloadScheduler> downloads;
    explicit CompactNetworkFixture(bool start_queue=true) {
        auto& context=selected.node.context;
        context.parallel_block_download=parallel;context.block_relay=relay;
        relay->SetOrchardBlockIngress(selected.node.service,selected.ingress);
        parallel->registerPeers({"peer"});
        if(start_queue)selected.queue->start();
    }
    ~CompactNetworkFixture() {
        auto& context=selected.node.context;
        context.block_download.reset();context.header_chain.reset();
        context.block_relay.reset();context.parallel_block_download.reset();
    }
    void Request(const CatalogSolvedTemplate& block) {
        parallel->scheduleBlock(block.Header().GetHash(),block.Height(),"peer");parallel->processQueue();
    }
    void Enroll(const CatalogSolvedTemplate& block) {
        headers=CompactBindingSelector(*selected.node.storage);
        OrchardAdmissionFixture::Require(headers->AddHeader(block.Header()));
        selected.node.context.header_chain=headers;
        downloads=std::make_shared<consensus::BlockDownloadScheduler>(headers.get(),selected.node.storage->files.get());
        selected.node.context.block_download=downloads;downloads->SetLocalTipHeight(101);
        downloads->SetSendGetDataCallback([](const uint256&,uint32_t){});
        downloads->SetConnectBlockBytesCallback([this](const auto& bytes,const auto& hash,uint32_t height,const auto&){
            return AcceptDownloadedOrchardBlock(selected.node.service,selected.ingress,parallel,bytes,hash,height)
                ? consensus::ConnectBlockResult::CONNECTED : consensus::ConnectBlockResult::TEMPORARY_FAIL;
        });
        downloads->OnHeadersProcessed();downloads->Tick();
    }
};
}
TEST(OrchardCompactNetwork, ExactCompactModeConnectsAndAcknowledgesOnce) {
    CompactNetworkFixture f;auto& selected=f.selected;const auto& block=*selected.node.storage->first;
    ASSERT_TRUE(selected.node.service->MatchesOrchardNetworkStorageMode(true));
    EXPECT_FALSE(selected.node.service->MatchesOrchardNetworkStorageMode(false));
    f.Request(block);ASSERT_TRUE(f.parallel->isInFlight(block.Header().GetHash()));
    const auto rows=selected.node.storage->Rows();const auto archives=selected.node.storage->ArchiveBytes();
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",block.WireBytes(),false),OrchardNetworkDisposition::Refused);
    EXPECT_EQ(selected.node.storage->Rows(),rows);EXPECT_EQ(selected.node.storage->ArchiveBytes(),archives);
    EXPECT_EQ(selected.queue->getMetrics().blocks_submitted.load(),0u);
    ASSERT_EQ(ReceiveOrchardNetworkBlock("peer",block.WireBytes(),true),OrchardNetworkDisposition::Connected);
    ASSERT_NO_FATAL_FAILURE(selected.CheckCompact(block));
    EXPECT_FALSE(f.parallel->isInFlight(block.Header().GetHash()));EXPECT_EQ(f.parallel->getStats().completed_blocks,1u);
    EXPECT_EQ(f.relay->GetStats().blocks_validated,1u);EXPECT_EQ(f.relay->GetPeerPerformance("peer").blocks_delivered,1u);
    EXPECT_EQ(selected.node.notices->recorded.event_count,1u);
    ASSERT_EQ(ReceiveOrchardNetworkBlock("peer",block.WireBytes(),true),OrchardNetworkDisposition::Connected);
    EXPECT_EQ(f.parallel->getStats().completed_blocks,1u);EXPECT_EQ(f.relay->GetStats().blocks_validated,1u);
    EXPECT_EQ(selected.node.notices->recorded.event_count,1u);
}
TEST(OrchardCompactNetwork, MalformedBodyAndStoppedOwnerNeverAcknowledge) {
    CompactNetworkFixture f;auto& selected=f.selected;const auto& block=*selected.node.storage->first;
    f.Request(block);auto trailing=block.WireBytes();trailing.push_back(0);
    const auto rows=selected.node.storage->Rows();const auto archives=selected.node.storage->ArchiveBytes();
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",trailing,true),OrchardNetworkDisposition::Refused);
    EXPECT_EQ(selected.node.storage->Rows(),rows);EXPECT_EQ(selected.node.storage->ArchiveBytes(),archives);
    EXPECT_TRUE(f.parallel->isInFlight(block.Header().GetHash()));EXPECT_EQ(f.parallel->getStats().completed_blocks,0u);
    EXPECT_FALSE(f.relay->IsBlockSeen(block.Header().GetHash()));EXPECT_EQ(f.relay->GetPeerPerformance("peer").blocks_delivered,0u);
    selected.node.service->Stop();
    EXPECT_FALSE(selected.node.service->MatchesOrchardNetworkStorageMode(true));
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",block.WireBytes(),true),OrchardNetworkDisposition::Refused);
    EXPECT_EQ(f.parallel->getStats().completed_blocks,0u);EXPECT_EQ(selected.node.notices->recorded.event_count,0u);
}
TEST(OrchardCompactNetwork, StoredBeforeQueueStartIsNotConnectedAcknowledgment) {
    CompactNetworkFixture f(false);auto& selected=f.selected;const auto& block=*selected.node.storage->first;
    f.Enroll(block);f.Request(block);ASSERT_TRUE(f.parallel->isInFlight(block.Header().GetHash()));
    ASSERT_EQ(ReceiveOrchardNetworkBlock("peer",block.WireBytes(),true),OrchardNetworkDisposition::Stored);
    EXPECT_TRUE(f.downloads->HasReceivedBlock(block.Header().GetHash()));EXPECT_EQ(f.downloads->GetLocalTipHeight(),101u);
    EXPECT_EQ(f.parallel->getStats().completed_blocks,0u);EXPECT_TRUE(f.parallel->isInFlight(block.Header().GetHash()));
    EXPECT_EQ(selected.node.notices->recorded.event_count,0u);EXPECT_EQ(f.relay->GetPeerPerformance("peer").blocks_delivered,0u);
    selected.queue->start();f.downloads->Tick();
    EXPECT_EQ(f.downloads->GetLocalTipHeight(),102u);EXPECT_EQ(f.parallel->getStats().completed_blocks,1u);
    EXPECT_FALSE(f.parallel->isInFlight(block.Header().GetHash()));
    EXPECT_EQ(selected.node.notices->recorded.event_count,1u);EXPECT_EQ(f.relay->GetPeerPerformance("peer").blocks_delivered,0u);
    ASSERT_NO_FATAL_FAILURE(selected.CheckCompact(block));
}
#endif
