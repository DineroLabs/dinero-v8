#pragma once
#include "daemon/orchard_network_block.h"
#include "daemon/p2p_message.h"
namespace dinero {
TEST(OrchardNetworkRequest, LegacyAndBackfillFormatsRemainDistinct) {
    struct Restore {ChainParams params=Params();DaemonContext* context=DaemonContext::instance();
        ~Restore(){MutableParams()=params;DaemonContext::setInstance(context);}} restore;
    MutableParams().name="regtest";MutableParams().orchard_activation_height=UINT32_MAX;
    MutableParams().orchard_branch_id=0;DaemonContext::setInstance(nullptr);
    EXPECT_EQ(SelectBlockRequestInventory(uint256{},false),InventoryType::MSG_BLOCK);
    EXPECT_EQ(SelectBlockRequestInventory(uint256{},true),InventoryType::MSG_UTREEXO_BLOCK);
    EXPECT_EQ(SelectBlockRequestInventory(uint256{},true,true),InventoryType::MSG_BLOCK);
    MutableParams().orchard_activation_height=102;MutableParams().orchard_branch_id=1;
    EXPECT_FALSE(SelectBlockRequestInventory(uint256{},true));
    EXPECT_EQ(SelectBlockRequestInventory(uint256{},true,true),InventoryType::MSG_BLOCK);
    MutableParams().orchard_branch_id=0;
    EXPECT_FALSE(SelectBlockRequestInventory(uint256{},false));
    EXPECT_FALSE(SelectBlockRequestInventory(uint256{},true,true));
}
// Android, mainnet 165,229 (2026-11-23): a phone that had connected four
// blocks of a losing branch could never switch. Its drain was told
// TEMPORARY_FAIL for the winning branch's first block, which the chainstate
// had stored and indexed but which did not outweigh the tip alone, so it
// offered that one block again on every tick ("Drain has failed 50
// consecutive ticks") and never reached the blocks that would have made the
// branch win. Stored is "accepted, not active" for phone storage too.
TEST(OrchardNetworkRequest, StoredBranchBlockLetsTheDrainMoveOnInEveryStorageMode) {
    using consensus::ConnectBlockResult;
    EXPECT_EQ(DrainResultForOrchardDisposition(OrchardNetworkDisposition::Stored),ConnectBlockResult::ACCEPTED_NOT_ACTIVE);
    EXPECT_EQ(DrainResultForOrchardDisposition(OrchardNetworkDisposition::Connected),ConnectBlockResult::CONNECTED);
    EXPECT_EQ(DrainResultForOrchardDisposition(OrchardNetworkDisposition::Refused),ConnectBlockResult::TEMPORARY_FAIL);
}
// The "[Phase N] Sent getdata(...)" log named the request from the node's
// mode alone, so a stateless node logged MSG_UTREEXO_BLOCK for the plain
// block requests it sends at Orchard-active heights. The log now prints the
// name of the type that was selected and sent.
TEST(OrchardNetworkRequest, LoggedRequestNameIsTheTypeThatWasSent) {
    EXPECT_STREQ(BlockRequestInventoryName(InventoryType::MSG_BLOCK),"MSG_BLOCK");
    EXPECT_STREQ(BlockRequestInventoryName(InventoryType::MSG_UTREEXO_BLOCK),"MSG_UTREEXO_BLOCK");
    struct Restore {ChainParams params=Params();DaemonContext* context=DaemonContext::instance();
        ~Restore(){MutableParams()=params;DaemonContext::setInstance(context);}} restore;
    MutableParams().name="regtest";MutableParams().orchard_activation_height=UINT32_MAX;
    MutableParams().orchard_branch_id=0;DaemonContext::setInstance(nullptr);
    // A stateless node's store-only backfill request is a plain block request.
    const auto backfill=SelectBlockRequestInventory(uint256{},true,true);ASSERT_TRUE(backfill);
    EXPECT_STREQ(BlockRequestInventoryName(*backfill),"MSG_BLOCK");
    const auto tip=SelectBlockRequestInventory(uint256{},true);ASSERT_TRUE(tip);
    EXPECT_STREQ(BlockRequestInventoryName(*tip),"MSG_UTREEXO_BLOCK");
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardNetworkRequest, ExactBoundaryAndActiveHeadersChooseTypedBytes) {
    CompactNetworkFixture f(false);auto& storage=*f.selected.node.storage;
    auto headers=CompactBindingSelector(storage);
    ASSERT_TRUE(headers->AddHeader(storage.first->Header()));
    ASSERT_TRUE(headers->AddHeader(storage.second->Header()));
    f.selected.node.context.header_chain=headers;
    const auto rows=storage.Rows();const auto archives=storage.ArchiveBytes();
    EXPECT_EQ(SelectBlockRequestInventory(storage.first->Header().prev_block_hash,true,false,101),InventoryType::MSG_UTREEXO_BLOCK);
    for(const auto& block:{storage.first,storage.second}) {
        EXPECT_EQ(SelectBlockRequestInventory(block->Header().GetHash(),true),InventoryType::MSG_BLOCK);
        EXPECT_EQ(SelectBlockRequestInventory(block->Header().GetHash(),true,false,block->Height()),InventoryType::MSG_BLOCK);
    }
    EXPECT_EQ(storage.Rows(),rows);EXPECT_EQ(storage.ArchiveBytes(),archives);
    EXPECT_EQ(f.selected.queue->getMetrics().blocks_submitted.load(),0u);
    EXPECT_EQ(f.parallel->getStats().completed_blocks,0u);
}
TEST(OrchardNetworkRequest, UnknownMismatchedAndMissingContextsRefuse) {
    CompactNetworkFixture f(false);auto& node=f.selected.node;const auto& block=*node.storage->first;
    auto headers=CompactBindingSelector(*node.storage);ASSERT_TRUE(headers->AddHeader(block.Header()));
    node.context.header_chain=headers;
    EXPECT_FALSE(SelectBlockRequestInventory(uint256{},true));
    EXPECT_FALSE(SelectBlockRequestInventory(block.Header().GetHash(),true,false,block.Height()+1));
    node.context.header_chain.reset();EXPECT_FALSE(SelectBlockRequestInventory(block.Header().GetHash(),true));
    node.context.header_chain=headers;
    EXPECT_EQ(SelectBlockRequestInventory(block.Header().GetHash(),true),InventoryType::MSG_BLOCK);
    node.service->Stop();
    // A format choice is not service readiness. The existing receiving owner
    // still refuses a stopped service, with no download completion.
    EXPECT_EQ(SelectBlockRequestInventory(block.Header().GetHash(),true),InventoryType::MSG_BLOCK);
    EXPECT_EQ(ReceiveOrchardNetworkBlock("peer",block.WireBytes(),true),OrchardNetworkDisposition::Refused);
    EXPECT_EQ(f.parallel->getStats().completed_blocks,0u);
}
#endif
}
