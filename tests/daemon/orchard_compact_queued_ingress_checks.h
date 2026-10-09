#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "daemon/services/block_ingress_service.h"
#include "daemon/orchard_network_block.h"
#include "consensus/validation_queue.h"
#include "daemon/services/chainstate_restart_import.h"
namespace {
// Actual service Start, compact reconstruction, selected catalog/stump and
// canonical writes through the production typed task. No network sockets,
// DaemonApp composition or claim that the stateless receive gate is open.
struct CompactQueuedIngressFixture {
    CompactServiceStartFixture node{true};
    std::shared_ptr<consensus::ValidationQueue> queue=consensus::ValidationQueue::CreateCanonicalOnly();
    std::shared_ptr<BlockIngressService> ingress=std::make_shared<BlockIngressService>();
    explicit CompactQueuedIngressFixture(bool retained_branch=false) {
        node.context.logger_interface=node.storage->f.logger.get();
        OrchardAdmissionFixture::Require(node.service->Start());
        // The shared archive fixture contains two previously connected bodies.
        // Model stored-but-unvalidated incoming bodies for the incremental cases:
        // preserve every body/undo/catalog byte and locator, changing only the
        // unselected headers' validation flags before any queue thread starts.
        // The retained-branch case below uses the original flags unchanged.
        if(!retained_branch) {
            auto selected=node.service->AcquireBlockIngressActivationLock();
            std::lock_guard<std::recursive_mutex> graph(dinero::g_block_index_mutex);
            for(const auto& block:{node.storage->first,node.storage->second}) {
                const auto hash=block->Header().GetHash();
                auto metadata=node.storage->reopened.getHeaderMetadata(hash);
                OrchardAdmissionFixture::Require(metadata.ok());
                OrchardAdmissionFixture::Require((metadata->status_flags&BLOCK_HAVE_DATA)!=0);
                OrchardAdmissionFixture::Require((metadata->status_flags&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD))==0);
                auto staged=*metadata;
                staged.status_flags=(staged.status_flags&~uint32_t(BLOCK_VALID_MASK))|BLOCK_VALID_HEADER;
                OrchardAdmissionFixture::Require(node.storage->reopened.putHeaderMetadata(
                    node.storage->f.token,hash,staged)==Status::Ok);
                auto* index=dinero::FindBlockIndex(hash);
                OrchardAdmissionFixture::Require(index!=nullptr);
                node.service->RemoveCandidate(index);
                OrchardAdmissionFixture::Require(RestorePersistedBlockIndexMetadata(node.storage->reopened,hash,index));
                OrchardAdmissionFixture::Require(index->status==staged.status_flags);
            }
        }
        node.context.validation_queue=queue;
        OrchardAdmissionFixture::Require(ingress->Init(node.context));
        OrchardAdmissionFixture::Require(ingress->Start());
        node.context.block_ingress=ingress.get();
    }
    ~CompactQueuedIngressFixture() {
        queue->stop();node.context.validation_queue.reset();
        node.context.block_ingress=nullptr;ingress->Stop();
    }
    BlockAcceptResult Receive(const std::vector<uint8_t>& wire) {
        return ingress->SubmitHex(util::hex(wire),BlockOrigin::P2P);
    }
    void CheckCompact(const CatalogSolvedTemplate& expected) {
        ASSERT_TRUE(node.service->IsStarted());EXPECT_EQ(node.service->GetConsensusUTXOSet(),nullptr);
        ASSERT_NE(node.service->GetActiveTip(),nullptr);
        EXPECT_EQ(node.service->GetActiveTip()->hash,expected.Header().GetHash());
        auto& db=node.storage->reopened;const auto tip=db.getTip(),validated=db.getValidatedTip();
        ASSERT_TRUE(tip.ok());ASSERT_TRUE(validated.ok());
        EXPECT_EQ(tip->hash,expected.Header().GetHash());EXPECT_EQ(validated->hash,tip->hash);
        EXPECT_EQ(tip->height,int32_t(expected.Height()));EXPECT_EQ(validated->height,tip->height);
        const auto body=node.service->getBlockRpcSnapshot(tip->hash);ASSERT_TRUE(body.ok());
        EXPECT_EQ(body->bytes,expected.WireBytes());
        const auto catalog=db.getOrchardCatalogState(tip->hash);ASSERT_TRUE(catalog.ok());
        EXPECT_EQ(*catalog,node.storage->expected_catalog.at(tip->hash));
        size_t coins=0;ASSERT_EQ(db.forEachUTXO([&](const uint256&,uint32_t,const Coin&){++coins;return true;}),Status::Ok);
        EXPECT_EQ(coins,0u);
    }
};
}
TEST(OrchardCompactQueuedIngress, ActualTypedQueueConnectsBoundaryAndChild) {
    CompactQueuedIngressFixture f;f.queue->start();
    for(const auto& block:{f.node.storage->first,f.node.storage->second}) {
        const auto result=f.Receive(block->WireBytes());ASSERT_TRUE(result.accepted())<<result.reason;
        EXPECT_TRUE(result.connected);EXPECT_FALSE(result.relayed);
        EXPECT_EQ(result.block_hash,block->Header().GetHash());EXPECT_EQ(result.height,block->Height());
        ASSERT_NO_FATAL_FAILURE(f.CheckCompact(*block));
    }
    EXPECT_EQ(f.queue->getTotalProcessed(),2u);EXPECT_EQ(f.node.notices->recorded.event_count,2u);
    const auto rows=f.node.storage->Rows();const auto archives=f.node.storage->ArchiveBytes();
    const auto repeated=f.Receive(f.node.storage->second->WireBytes());ASSERT_TRUE(repeated.accepted());
    EXPECT_TRUE(repeated.connected);EXPECT_FALSE(repeated.relayed);
    EXPECT_EQ(f.node.notices->recorded.event_count,2u);
    EXPECT_EQ(f.node.storage->Rows(),rows);EXPECT_EQ(f.node.storage->ArchiveBytes(),archives);
    EXPECT_EQ(f.queue->getQueuedCount(),0u);EXPECT_EQ(f.queue->getInFlightCount(),0u);
}
TEST(OrchardCompactQueuedIngress, FramingAndMissingQueueRefuseWithoutMutation) {
    CompactQueuedIngressFixture f;auto& block=*f.node.storage->first;
    const auto rows=f.node.storage->Rows();const auto archives=f.node.storage->ArchiveBytes();
    EXPECT_FALSE(f.Receive(block.WireBytes()).accepted());EXPECT_EQ(f.queue->getTotalProcessed(),0u);
    f.queue->start();auto trailing=block.WireBytes();trailing.push_back(0);
    EXPECT_FALSE(f.Receive(trailing).accepted());EXPECT_EQ(f.queue->getTotalProcessed(),0u);
    EXPECT_EQ(f.node.notices->recorded.event_count,0u);
    EXPECT_EQ(f.node.storage->Rows(),rows);EXPECT_EQ(f.node.storage->ArchiveBytes(),archives);
    ASSERT_TRUE(f.Receive(block.WireBytes()).accepted());ASSERT_NO_FATAL_FAILURE(f.CheckCompact(block));
    f.queue->stop();const auto accepted_rows=f.node.storage->Rows();
    EXPECT_FALSE(f.Receive(f.node.storage->second->WireBytes()).accepted());
    EXPECT_EQ(f.node.storage->Rows(),accepted_rows);EXPECT_EQ(f.queue->getTotalProcessed(),1u);
}
TEST(OrchardCompactQueuedIngress, DownloadCompletionRequiresExactConnectedResult) {
    CompactQueuedIngressFixture f;f.queue->start();const auto& block=*f.node.storage->first;
    const auto rows=f.node.storage->Rows();const auto archives=f.node.storage->ArchiveBytes();
    EXPECT_EQ(SubmitDownloadedOrchardBlock(f.node.service,f.ingress,{},block.WireBytes(),uint256{},block.Height()),OrchardNetworkDisposition::Refused);
    EXPECT_EQ(SubmitDownloadedOrchardBlock(f.node.service,f.ingress,{},block.WireBytes(),block.Header().GetHash(),block.Height()+1),OrchardNetworkDisposition::Refused);
    EXPECT_EQ(f.queue->getMetrics().blocks_submitted.load(),0u);
    EXPECT_EQ(f.node.storage->Rows(),rows);EXPECT_EQ(f.node.storage->ArchiveBytes(),archives);
    ASSERT_EQ(SubmitDownloadedOrchardBlock(f.node.service,f.ingress,{},block.WireBytes(),block.Header().GetHash(),block.Height()),OrchardNetworkDisposition::Connected);
    ASSERT_NO_FATAL_FAILURE(f.CheckCompact(block));EXPECT_EQ(f.queue->getTotalProcessed(),1u);
    f.ingress->Stop();const auto after=f.node.storage->Rows();
    EXPECT_EQ(SubmitDownloadedOrchardBlock(f.node.service,f.ingress,{},f.node.storage->second->WireBytes(),f.node.storage->second->Header().GetHash(),f.node.storage->second->Height()),OrchardNetworkDisposition::Refused);
    EXPECT_EQ(f.node.storage->Rows(),after);EXPECT_EQ(f.queue->getTotalProcessed(),1u);
}
TEST(OrchardCompactQueuedIngress, RetainedBranchRequiresDetachedActivationBeforeAcknowledgment) {
    CompactQueuedIngressFixture f(true);f.queue->start();
    const auto& first=*f.node.storage->first;
    const auto result=f.Receive(first.WireBytes());
    EXPECT_EQ(result.code,BlockRejectCode::STORED_NOT_VALIDATED);
    EXPECT_FALSE(result.accepted());EXPECT_FALSE(result.connected);EXPECT_FALSE(result.relayed);
    EXPECT_EQ(result.block_hash,first.Header().GetHash());EXPECT_EQ(result.height,first.Height());
    ASSERT_NE(f.node.service->GetActiveTip(),nullptr);
    EXPECT_EQ(f.node.service->GetActiveTip()->hash,f.node.storage->parent->hash);
    EXPECT_EQ(f.node.notices->recorded.event_count,0u);
    EXPECT_EQ(f.queue->getMetrics().blocks_validated.load(),0u);
    // Actual root activation performs detached whole-branch validation; the
    // queued request must not invent a connected acknowledgment beforehand.
    f.node.service->ActivateBestChain();
    ASSERT_NO_FATAL_FAILURE(f.CheckCompact(*f.node.storage->second));
    EXPECT_EQ(f.node.notices->recorded.event_count,2u);
    const auto rows=f.node.storage->Rows();const auto archives=f.node.storage->ArchiveBytes();
    const auto retry=f.Receive(first.WireBytes());ASSERT_TRUE(retry.accepted())<<retry.reason;
    EXPECT_TRUE(retry.connected);EXPECT_EQ(retry.block_hash,first.Header().GetHash());
    EXPECT_EQ(f.node.notices->recorded.event_count,2u);
    EXPECT_EQ(f.node.storage->Rows(),rows);EXPECT_EQ(f.node.storage->ArchiveBytes(),archives);
}
#endif
