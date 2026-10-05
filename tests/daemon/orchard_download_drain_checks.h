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

namespace {
struct DownloadPeerCreditFixture : DownloadDrainFixture {
    static constexpr const char* slow = "192.0.2.9:20999";
    std::vector<std::pair<uint256,bool>> offers;
    uint256 probe_hash;
    void Start(const BlockHeader& header, unsigned misses) {
        Enroll(header);
        scheduler->SetTipRetryTimeout(std::chrono::hours(1));
        scheduler->SetStaleRequestTimeoutSeconds(3600);
        scheduler->SetSlowPeerCooldown(std::chrono::hours(1));
        scheduler->SetSendGetDataCallback([&](const uint256& hash,uint32_t) {
            offers.emplace_back(hash,scheduler->CurrentRequestSkipPeers().count(slow)>0);
            scheduler->NotifyGetDataDispatched(hash,1,slow);
            return true;
        });
        OrchardAdmissionFixture::Require(scheduler->ReRequestBlock(header.GetHash()));
        scheduler->Tick();
        for(unsigned i=0;i<misses;++i) {
            scheduler->SetStaleRequestTimeoutSeconds(0);
            scheduler->Tick();
            scheduler->SetStaleRequestTimeoutSeconds(3600);
            scheduler->SetTipRetryTimeout(std::chrono::hours(1));
            scheduler->Tick();
        }
        // This second header is only a request probe. No body for it is
        // supplied, connected, or claimed independently consensus-valid.
        auto next=header;next.prev_block_hash=header.GetHash();
        next.timestamp+=60;++next.nonce;probe_hash=next.GetHash();
        OrchardAdmissionFixture::Require(headers.AddHeader(next));
        scheduler->OnHeadersProcessed();scheduler->Tick();
    }
    bool ProbeSkipsSlowPeer() {
        offers.clear();OrchardAdmissionFixture::Require(scheduler->ReRequestBlock(probe_hash));
        scheduler->Tick();
        const auto found=std::find_if(offers.begin(),offers.end(),[&](const auto& p){return p.first==probe_hash;});
        OrchardAdmissionFixture::Require(found!=offers.end());
        return found->second;
    }
};
}
TEST(OrchardDownloadPeerCredit, ValidStoredReceiptCreditsButRejectedWireDoesNot) {
    DownloadPeerCreditFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    f.Start(block->Header(),2);ASSERT_TRUE(f.ProbeSkipsSlowPeer());
    auto trailing=block->WireBytes();trailing.push_back(0);
    EXPECT_FALSE(f.scheduler->OnOrchardBlockReceived(trailing));
    EXPECT_FALSE(f.scheduler->HasReceivedBlock(block->Header().GetHash()));
    EXPECT_TRUE(f.ProbeSkipsSlowPeer());
    ASSERT_TRUE(f.scheduler->OnOrchardBlockReceived(block->WireBytes()));
    EXPECT_FALSE(f.ProbeSkipsSlowPeer());
    EXPECT_EQ(f.scheduler->GetLocalTipHeight(),101u);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
}
TEST(OrchardDownloadPeerCredit, DuplicateReceiptCannotEarnRepeatedCredit) {
    DownloadPeerCreditFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    f.Start(block->Header(),3);ASSERT_TRUE(f.ProbeSkipsSlowPeer());
    ASSERT_TRUE(f.scheduler->OnOrchardBlockReceived(block->WireBytes()));
    EXPECT_TRUE(f.ProbeSkipsSlowPeer());
    for(unsigned i=0;i<4;++i)ASSERT_TRUE(f.scheduler->OnOrchardBlockReceived(block->WireBytes()));
    EXPECT_TRUE(f.ProbeSkipsSlowPeer());
    EXPECT_EQ(f.scheduler->GetLocalTipHeight(),101u);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
}
#endif
} // namespace dinero
