#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
void InstallCompactDrain(CompactNetworkFixture& f, unsigned& calls) {
    f.downloads->SetCompactOrchardConnectCallback([&f,&calls](const auto& bytes,const auto& hash,uint32_t height,const auto&) {
        ++calls;
        const auto result=SubmitDownloadedCompactOrchardBlock(f.selected.node.service,f.selected.ingress,f.parallel,bytes,hash,height);
        return result==OrchardNetworkDisposition::Connected ? consensus::ConnectBlockResult::CONNECTED
            : consensus::ConnectBlockResult::TEMPORARY_FAIL;
    });
}
}
TEST(OrchardCompactDownloadMode, ExplicitTypedPathRetriesStoredWithoutAcknowledgment) {
    CompactNetworkFixture f(false);const auto& block=*f.selected.node.storage->first;
    f.Enroll(block);f.downloads->SetStatelessMode(true);f.Request(block);
    // A full-mode typed callback alone must never enable a stateless drain.
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",block.WireBytes(),true),OrchardNetworkDisposition::Refused);
    EXPECT_FALSE(f.downloads->HasReceivedBlock(block.Header().GetHash()));
    unsigned calls=0;InstallCompactDrain(f,calls);
    auto malformed=block.WireBytes();malformed.push_back(0);
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",malformed,true),OrchardNetworkDisposition::Refused);
    EXPECT_EQ(calls,0u);
    ASSERT_EQ(ReceiveOrchardNetworkBlock("peer",block.WireBytes(),true),OrchardNetworkDisposition::Stored);
    EXPECT_GE(calls,1u);EXPECT_EQ(f.downloads->GetLocalTipHeight(),101u);
    EXPECT_EQ(f.parallel->getStats().completed_blocks,0u);EXPECT_EQ(f.selected.node.notices->recorded.event_count,0u);
    const auto prior=calls;f.selected.queue->start();f.downloads->Tick();
    EXPECT_GT(calls,prior);EXPECT_EQ(f.downloads->GetLocalTipHeight(),102u);
    EXPECT_EQ(f.parallel->getStats().completed_blocks,1u);EXPECT_EQ(f.selected.node.notices->recorded.event_count,1u);
    ASSERT_NO_FATAL_FAILURE(f.selected.CheckCompact(block));
    f.downloads->Tick();EXPECT_EQ(f.parallel->getStats().completed_blocks,1u);
    EXPECT_EQ(f.selected.node.notices->recorded.event_count,1u);
}
TEST(OrchardCompactDownloadMode, StoppedCompactOwnerCannotFinishStoredBody) {
    CompactNetworkFixture f(false);const auto& block=*f.selected.node.storage->first;
    f.Enroll(block);f.downloads->SetStatelessMode(true);f.Request(block);
    unsigned calls=0;InstallCompactDrain(f,calls);
    ASSERT_TRUE(f.downloads->OnOrchardBlockReceived(block.WireBytes()));
    f.selected.node.service->Stop();f.selected.queue->start();f.downloads->Tick();
    EXPECT_EQ(calls,1u);EXPECT_EQ(f.downloads->GetLocalTipHeight(),101u);
    EXPECT_TRUE(f.downloads->HasReceivedBlock(block.Header().GetHash()));
    EXPECT_EQ(f.parallel->getStats().completed_blocks,0u);EXPECT_EQ(f.selected.node.notices->recorded.event_count,0u);
}
TEST(OrchardCompactDownloadMode, HistoricalRawBodyNeverReachesEitherCallback) {
    DownloadDrainFixture f;consensus::HeaderChainSelector headers;
    for(const auto& block:f.f.blocks)ASSERT_TRUE(headers.AddHeader(block.header));
    ASSERT_EQ(f.f.blocks.size(),102u);const auto& historical=f.f.blocks.back();
    consensus::BlockDownloadScheduler scheduler(&headers,f.files.get());
    scheduler.SetStatelessMode(true);scheduler.SetLocalTipHeight(100);
    scheduler.SetSendGetDataCallback([](const uint256&,uint32_t){});
    unsigned old=0,typed=0;
    scheduler.SetConnectBlockCallback([&](const auto&,const auto&){++old;return consensus::ConnectBlockResult::CONNECTED;});
    scheduler.SetCompactOrchardConnectCallback([&](const auto&,const auto&,uint32_t,const auto&){++typed;return consensus::ConnectBlockResult::CONNECTED;});
    scheduler.OnHeadersProcessed();scheduler.Tick();
    ASSERT_TRUE(scheduler.OnBlockReceived(historical));scheduler.Tick();
    EXPECT_EQ(old,0u);EXPECT_EQ(typed,0u);EXPECT_EQ(scheduler.GetLocalTipHeight(),100u);
    EXPECT_TRUE(scheduler.HasReceivedBlock(historical.GetHash()));
}
}
#endif
