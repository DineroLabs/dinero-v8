#pragma once
#include "daemon/services/block_ingress_service.h"
#include "consensus/validation_queue.h"
#include "consensus/chainstate_guard.h"
#include "common/test_logger.h"
namespace dinero {
TEST(OrchardQueuedIngress, MissingSelectedOwnerRefuses) {
    const auto before=Params();struct Restore {ChainParams value;~Restore(){MutableParams()=value;}} restore{before};
    MutableParams().orchard_activation_height=102;MutableParams().orchard_branch_id=1;
    BlockIngressService ingress;TestLogger logger;DaemonContext missing;missing.logger_interface=&logger;
    ASSERT_TRUE(ingress.Init(missing));ASSERT_TRUE(ingress.Start());
    EXPECT_FALSE(ingress.SubmitHex("",BlockOrigin::P2P).accepted());
    const auto header=SelectedGenesis().header.SerializeForHash();
    EXPECT_EQ(ingress.SubmitHex(util::hex(std::vector<uint8_t>(header.begin(),header.end()))+"00",BlockOrigin::P2P).code,BlockRejectCode::CONNECT_FAILED);
    ingress.Stop();
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct QueuedIngressFixture : CanonicalPoolFixture {
    consensus::ChainstateGuard guard;
    std::shared_ptr<consensus::ValidationQueue> queue;
    BlockIngressService ingress;
    QueuedIngressFixture() {
        OrchardAdmissionFixture::Require(ingress.Init(context));
        OrchardAdmissionFixture::Require(ingress.Start());
        StartQueue(consensus::ValidationQueue::Config::forNormalOperation());
    }
    void StartQueue(consensus::ValidationQueue::Config config) {
        if(queue)queue->stop();context.validation_queue.reset();
        config.worker_pool_threads=1;
        queue=std::make_shared<consensus::ValidationQueue>(f.service->GetConsensusUTXOSet(),&guard,config);
        context.validation_queue=queue;queue->start();
    }
    ~QueuedIngressFixture(){queue->stop();context.validation_queue.reset();ingress.Stop();}
    BlockAcceptResult Receive(const std::vector<uint8_t>& wire) {
        return ingress.SubmitHex(util::hex(wire),BlockOrigin::P2P);
    }
};
}
TEST(OrchardQueuedIngress, ActualQueueCommitsExactBodyAndReopens) {
    QueuedIngressFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    const auto result=f.Receive(block->WireBytes());ASSERT_TRUE(result.accepted())<<result.reason;
    EXPECT_TRUE(result.connected);EXPECT_FALSE(result.relayed);EXPECT_EQ(result.block_hash,block->Header().GetHash());
    EXPECT_EQ(result.height,102u);EXPECT_EQ(f.queue->getTotalProcessed(),1u);
    EXPECT_EQ(f.queue->getMetrics().blocks_validated.load(),1u);EXPECT_EQ(f.queue->getQueuedCount(),0u);
    EXPECT_EQ(f.queue->getInFlightCount(),0u);EXPECT_EQ(f.notices->published,1u);
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);
    const auto stored=f.f.service->getBlockRpcSnapshot(result.block_hash);ASSERT_TRUE(stored.ok());
    EXPECT_EQ(stored->bytes,block->WireBytes());EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    EXPECT_TRUE(f.Receive(block->WireBytes()).accepted());EXPECT_EQ(f.notices->published,1u);
    f.queue->stop();EXPECT_FALSE(f.Receive(block->WireBytes()).accepted());
}
TEST(OrchardQueuedIngress, FramingAndProviderRefusalKeepRetryAvailable) {
    QueuedIngressFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    auto trailing=block->WireBytes();trailing.push_back(0);
    EXPECT_FALSE(f.Receive(trailing).accepted());EXPECT_EQ(f.queue->getTotalProcessed(),0u);
    f.f.service->setRuntimeBlockNotifications(nullptr);
    EXPECT_FALSE(f.Receive(block->WireBytes()).accepted());EXPECT_EQ(f.queue->getTotalProcessed(),0u);
    EXPECT_EQ(f.queue->getMetrics().blocks_validated.load(),0u);
    EXPECT_EQ(f.f.db.getHeader(block->Header().GetHash()).status(),Status::NotFound);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    f.f.service->setRuntimeBlockNotifications(f.notices);
    ASSERT_TRUE(f.Receive(block->WireBytes()).accepted());EXPECT_EQ(f.queue->getTotalProcessed(),1u);
    EXPECT_EQ(f.queue->getMetrics().blocks_failed.load(),2u);EXPECT_EQ(f.notices->published,1u);
}
TEST(OrchardQueuedIngress, CountAndByteLimitsRefuseBeforeCanonicalApply) {
    QueuedIngressFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    auto config=consensus::ValidationQueue::Config::forNormalOperation();config.max_queued_blocks=0;f.StartQueue(config);
    EXPECT_FALSE(f.Receive(block->WireBytes()).accepted());EXPECT_EQ(f.queue->getMetrics().blocks_submitted.load(),0u);
    config.max_queued_blocks=1;config.max_canonical_wire_bytes=block->WireBytes().size()-1;f.StartQueue(config);
    EXPECT_FALSE(f.Receive(block->WireBytes()).accepted());EXPECT_EQ(f.queue->getMetrics().blocks_submitted.load(),0u);
    EXPECT_EQ(f.f.db.getHeader(block->Header().GetHash()).status(),Status::NotFound);EXPECT_EQ(f.notices->published,0u);
    config.max_canonical_wire_bytes=block->WireBytes().size();f.StartQueue(config);
    ASSERT_TRUE(f.Receive(block->WireBytes()).accepted());EXPECT_EQ(f.queue->getTotalProcessed(),1u);
    EXPECT_EQ(f.notices->published,1u);EXPECT_EQ(f.queue->getInFlightCount(),0u);
}
#endif
} // namespace dinero
