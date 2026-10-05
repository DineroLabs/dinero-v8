#pragma once
namespace dinero {
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
auto ForkSibling(const auto& block) {
    auto header=block.Header();++header.nonce;
    auto bytes=block.WireBytes();const auto prefix=header.SerializeForHash();
    std::copy(prefix.begin(),prefix.end(),bytes.begin());
    return std::pair{header,bytes};
}
void CheckStoredFork(NetworkRoutingFixture& f,const BlockHeader& header,
                     const std::vector<uint8_t>& bytes,const uint256& selected) {
    EXPECT_EQ(f.f.service->GetActiveTip()->hash,selected);
    EXPECT_EQ(f.notices->published,1u);
    const auto metadata=f.f.db.getHeaderMetadata(header.GetHash());ASSERT_TRUE(metadata.ok());
    EXPECT_NE(metadata->status_flags&BLOCK_HAVE_DATA,0u);
    EXPECT_EQ(metadata->status_flags&BLOCK_VALID_MASK,uint32_t(BLOCK_VALID_HEADER));
    EXPECT_EQ(metadata->status_flags&(BLOCK_HAVE_UNDO|BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    const auto stored=ReadRuntimeBlockUnderLock(f.f.db,f.files.get(),header.GetHash(),102);
    ASSERT_TRUE(stored.ok());ASSERT_TRUE(stored->IsOrchardProfile());EXPECT_EQ(stored->Orchard().WireBytes(),bytes);
    EXPECT_EQ(f.f.db.getOrchardState()->pool_balance,5000u);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
}
TEST(OrchardForkIntake, ActualRouterRetainsSiblingWithoutSelectedCoinValidation) {
    NetworkRoutingFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    const auto [header,bytes]=ForkSibling(*block);
    ASSERT_TRUE(f.Submit(block->WireBytes()).connected);
    const auto selected=f.f.service->GetActiveTip()->hash;
    // The shared shield input is now spent on the selected branch. This sibling
    // must be stored with header validity only, never checked against child coins.
    const auto received=f.Submit(bytes);EXPECT_TRUE(received.retained())<<received.reason;
    EXPECT_FALSE(received.accepted());EXPECT_FALSE(received.rejected());
    EXPECT_FALSE(received.connected);EXPECT_FALSE(received.relayed);
    EXPECT_STREQ(BlockRejectCodeToString(received.code),"stored-not-validated");
    CheckStoredFork(f,header,bytes,selected);
    const auto before=f.f.db.getHeaderMetadata(header.GetHash());ASSERT_TRUE(before.ok());
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    EXPECT_TRUE(f.Submit(bytes).retained());
    const auto after=f.f.db.getHeaderMetadata(header.GetHash());ASSERT_TRUE(after.ok());
    EXPECT_EQ(before->file_number,after->file_number);EXPECT_EQ(before->data_pos,after->data_pos);
    CheckStoredFork(f,header,bytes,selected);
}
TEST(OrchardForkIntake, QueueAndTransportPreserveStoredVersusConnected) {
    NetworkRoutingFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    const auto [header,bytes]=ForkSibling(*block);ASSERT_TRUE(f.Submit(block->WireBytes()).connected);
    f.RequestParallel(header.GetHash());ASSERT_TRUE(f.parallel->isInFlight(header.GetHash()));
    const auto connected=f.queue->getMetrics().blocks_connected.load();
    const auto failed=f.queue->getMetrics().blocks_failed.load();
    EXPECT_EQ(SubmitDownloadedOrchardBlock(f.f.service,f.owner,f.parallel,bytes,header.GetHash(),102),
              OrchardNetworkDisposition::Stored);
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",bytes,false),OrchardNetworkDisposition::Stored);
    EXPECT_FALSE(AcceptDownloadedOrchardBlock(f.f.service,f.owner,f.parallel,bytes,header.GetHash(),102));
    EXPECT_FALSE(f.relay->HandleOrchardBlock("peer",bytes));
    EXPECT_TRUE(f.parallel->isInFlight(header.GetHash()));
    EXPECT_EQ(f.parallel->getStats().completed_blocks,0u);
    EXPECT_FALSE(f.relay->IsBlockSeen(header.GetHash()));
    EXPECT_EQ(f.relay->GetStats().blocks_validated,0u);
    EXPECT_EQ(f.relay->GetPeerPerformance("peer").blocks_delivered,0u);
    EXPECT_EQ(f.queue->getMetrics().blocks_connected.load(),connected);
    EXPECT_EQ(f.queue->getMetrics().blocks_failed.load(),failed);
    EXPECT_EQ(f.queue->getMetrics().blocks_retained.load(),4u);
    EXPECT_EQ(f.queue->getTotalProcessed(),0u);EXPECT_EQ(f.queue->getQueuedCount(),0u);
    EXPECT_EQ(f.queue->getInFlightCount(),0u);
    CheckStoredFork(f,header,bytes,block->Header().GetHash());
}
TEST(OrchardForkIntake, BadHeaderFramingAndOperatorInvalidityNeverBecomeRetention) {
    NetworkRoutingFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    const auto [header,bytes]=ForkSibling(*block);ASSERT_TRUE(f.Submit(block->WireBytes()).connected);
    auto bad=bytes;bad.push_back(0);EXPECT_FALSE(f.Submit(bad).retained());
    auto old=header;old.timestamp=1;const auto prefix=old.SerializeForHash();bad=bytes;
    std::copy(prefix.begin(),prefix.end(),bad.begin());
    const auto refused=f.Submit(bad);EXPECT_TRUE(refused.rejected());EXPECT_FALSE(refused.retained());
    EXPECT_EQ(f.f.db.getHeader(old.GetHash()).status(),Status::NotFound);
    EXPECT_EQ(f.f.db.getHeader(header.GetHash()).status(),Status::NotFound);
    ASSERT_TRUE(f.Submit(bytes).retained());std::string error;
    ASSERT_TRUE(f.f.service->InvalidateBlock(header.GetHash(),error))<<error;
    const auto invalid=f.f.db.getHeaderMetadata(header.GetHash());ASSERT_TRUE(invalid.ok());
    EXPECT_NE(invalid->status_flags&BLOCK_FAILED_VALID,0u);
    EXPECT_TRUE(f.Submit(bytes).rejected());
    const auto after=f.f.db.getHeaderMetadata(header.GetHash());ASSERT_TRUE(after.ok());
    EXPECT_EQ(after->status_flags,invalid->status_flags);
    EXPECT_EQ(after->data_pos,invalid->data_pos);
    EXPECT_EQ(f.f.service->GetActiveTip()->hash,block->Header().GetHash());EXPECT_EQ(f.notices->published,1u);
}
#endif
} // namespace dinero
