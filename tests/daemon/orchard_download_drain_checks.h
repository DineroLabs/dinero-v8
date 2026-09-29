#pragma once
#include "consensus/block_download_scheduler.h"
#include "consensus/header_chain.h"
namespace dinero {
TEST(OrchardDownloadDrain, MissingOwnerNeverAcknowledgesStorage) {
    consensus::BlockDownloadScheduler scheduler(nullptr,nullptr);
    const std::vector<uint8_t> incomplete(128,0);
    EXPECT_FALSE(scheduler.OnOrchardBlockReceived(incomplete));
    EXPECT_EQ(scheduler.GetLocalTipHeight(),0u);
    EXPECT_EQ(scheduler.GetInFlightCount(),0u);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct DownloadDrainFixture : QueuedIngressFixture {
    consensus::HeaderChainSelector headers;
    std::unique_ptr<consensus::BlockDownloadScheduler> scheduler;
    unsigned requests=0;
    void Enroll(const BlockHeader& child) {
        for(const auto& block:f.blocks)OrchardAdmissionFixture::Require(headers.AddHeader(block.header));
        OrchardAdmissionFixture::Require(headers.AddHeader(child));
        scheduler=std::make_unique<consensus::BlockDownloadScheduler>(&headers,files.get());
        scheduler->SetLocalTipHeight(101);
        scheduler->SetSendGetDataCallback([&](const uint256&,uint32_t){++requests;});
        scheduler->OnHeadersProcessed();scheduler->Tick();
    }
    consensus::ConnectBlockResult Apply(const std::vector<uint8_t>& wire,const uint256& hash,uint32_t height) {
        const auto result=Receive(wire);
        return result.accepted() && result.connected && result.block_hash==hash && result.height==height
            ? consensus::ConnectBlockResult::CONNECTED : consensus::ConnectBlockResult::TEMPORARY_FAIL;
    }
};
}
TEST(OrchardDownloadDrain, ActualStoredBodyQueueRetryAndReopen) {
    DownloadDrainFixture f;const auto block=f.Build();ASSERT_TRUE(block);f.Enroll(block->Header());
    ASSERT_EQ(f.requests,1u);unsigned offered=0;
    f.scheduler->SetConnectBlockBytesCallback([&](const auto& wire,const auto& hash,uint32_t height,const auto&){
        ++offered;EXPECT_EQ(wire,block->WireBytes());EXPECT_EQ(height,102u);
        // These public scheduler operations are valid while the callback runs.
        EXPECT_EQ(f.scheduler->GetInFlightCount(),0u);f.scheduler->Tick();
        return f.Apply(wire,hash,height);
    });
    ASSERT_TRUE(f.scheduler->OnOrchardBlockReceived(block->WireBytes()));
    EXPECT_TRUE(f.scheduler->HasReceivedBlock(block->Header().GetHash()));
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    f.f.service->setRuntimeBlockNotifications(nullptr);f.scheduler->Tick();
    EXPECT_EQ(offered,1u);EXPECT_EQ(f.scheduler->GetLocalTipHeight(),101u);
    EXPECT_TRUE(f.scheduler->HasReceivedBlock(block->Header().GetHash()));
    f.f.service->setRuntimeBlockNotifications(f.notices);f.scheduler->Tick();
    EXPECT_EQ(offered,2u);EXPECT_EQ(f.scheduler->GetLocalTipHeight(),102u);
    EXPECT_EQ(f.notices->published,1u);EXPECT_EQ(f.queue->getTotalProcessed(),1u);
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    const auto stored=f.f.service->getBlockRpcSnapshot(block->Header().GetHash());ASSERT_TRUE(stored.ok());
    EXPECT_EQ(stored->bytes,block->WireBytes());EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
TEST(OrchardDownloadDrain, ChangedInventoryCannotAcknowledgeCapturedOffer) {
    DownloadDrainFixture f;const auto block=f.Build();ASSERT_TRUE(block);f.Enroll(block->Header());
    ASSERT_TRUE(f.scheduler->OnOrchardBlockReceived(block->WireBytes()));unsigned calls=0;
    f.scheduler->SetConnectBlockBytesCallback([&](const auto&,const auto& hash,uint32_t,const auto&){
        ++calls;EXPECT_TRUE(f.scheduler->ReRequestBlock(hash));
        return consensus::ConnectBlockResult::CONNECTED; // Deliberately stale result, not actual chain acceptance.
    });
    f.scheduler->Tick();EXPECT_EQ(calls,1u);EXPECT_EQ(f.scheduler->GetLocalTipHeight(),101u);
    EXPECT_FALSE(f.scheduler->HasReceivedBlock(block->Header().GetHash()));
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    f.scheduler->Tick();EXPECT_EQ(f.requests,2u);EXPECT_EQ(calls,1u);
    f.scheduler->SetConnectBlockBytesCallback([&](const auto& wire,const auto& hash,uint32_t height,const auto&){return f.Apply(wire,hash,height);});
    ASSERT_TRUE(f.scheduler->OnOrchardBlockReceived(block->WireBytes()));f.scheduler->Tick();
    EXPECT_EQ(f.scheduler->GetLocalTipHeight(),102u);EXPECT_EQ(f.notices->published,1u);
}
TEST(OrchardDownloadDrain, ExactFramingAndTypedCallbackRequired) {
    DownloadDrainFixture f;const auto block=f.Build();ASSERT_TRUE(block);f.Enroll(block->Header());
    auto trailing=block->WireBytes();trailing.push_back(0);
    EXPECT_FALSE(f.scheduler->OnOrchardBlockReceived(trailing));
    EXPECT_FALSE(f.scheduler->HasReceivedBlock(block->Header().GetHash()));
    unsigned historical=0;
    f.scheduler->SetConnectBlockCallback([&](const Block&,const auto&){++historical;return consensus::ConnectBlockResult::CONNECTED;});
    ASSERT_TRUE(f.scheduler->OnOrchardBlockReceived(block->WireBytes()));f.scheduler->Tick();
    EXPECT_EQ(historical,0u);EXPECT_EQ(f.scheduler->GetLocalTipHeight(),101u);
    EXPECT_TRUE(f.scheduler->HasReceivedBlock(block->Header().GetHash()));
    f.scheduler->SetConnectBlockBytesCallback([&](const auto& wire,const auto& hash,uint32_t height,const auto&){return f.Apply(wire,hash,height);});
    f.scheduler->Tick();EXPECT_EQ(f.scheduler->GetLocalTipHeight(),102u);EXPECT_EQ(historical,0u);
}
#endif
} // namespace dinero
