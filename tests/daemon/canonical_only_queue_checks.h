#pragma once
#include "consensus/validation_queue.h"
namespace dinero {
namespace {
// Scheduling/result-contract fixture only: these synthetic tasks do not claim
// to validate a block or prove compact chainstate/network composition.
struct CanonicalOnlyFixtureTask final : consensus::CanonicalBlockTask {
    enum class Reply { Connected, WrongHash, WrongHeight, Disconnected, Refused, Exception };
    const uint256 hash=uint256::FromHexUnsafe(std::string(64,'a'));
    const uint64_t height=42;
    const size_t bytes;
    const Reply reply;
    unsigned& calls;
    CanonicalOnlyFixtureTask(unsigned& count,Reply value=Reply::Connected,size_t size=128)
        :bytes(size),reply(value),calls(count){}
    const uint256& Hash()const noexcept override{return hash;}
    uint64_t Height()const noexcept override{return height;}
    size_t WireBytes()const noexcept override{return bytes;}
    BlockAcceptResult ValidateAndApply()const override {
        ++calls;
        if(reply==Reply::Exception)throw std::runtime_error("fixture task refusal");
        if(reply==Reply::Refused)return BlockAcceptResult::Rejected(
            BlockRejectCode::CONNECT_FAILED,"fixture refusal",hash,height);
        return BlockAcceptResult{BlockRejectCode::OK,"fixture result",
            reply==Reply::WrongHash?uint256{}:hash,
            reply==Reply::WrongHeight?height+1:height,
            reply!=Reply::Disconnected,false};
    }
};
}
TEST(CanonicalOnlyQueue, TypedDispatchAndLegacyRefusal) {
    auto queue=consensus::ValidationQueue::CreateCanonicalOnly();ASSERT_TRUE(queue);
    EXPECT_THROW((void)consensus::ValidationQueue(nullptr,nullptr),std::runtime_error);
    queue->start();ASSERT_TRUE(queue->isRunning());
    unsigned calls=0;const auto task=std::make_shared<CanonicalOnlyFixtureTask>(calls);
    const auto result=queue->submitAndWait(task);ASSERT_TRUE(result.accepted());EXPECT_TRUE(result.connected);
    EXPECT_EQ(result.block_hash,task->hash);EXPECT_EQ(result.height,task->height);EXPECT_EQ(calls,1u);
    Block historical;EXPECT_FALSE(queue->submit(historical,1,uint256{}));
    EXPECT_FALSE(queue->submitAndWait(historical,1,uint256{}).accepted());
    EXPECT_EQ(queue->getMetrics().blocks_submitted.load(),1u);
    EXPECT_EQ(queue->getMetrics().blocks_connected.load(),1u);
    EXPECT_EQ(queue->getQueuedCount(),0u);EXPECT_EQ(queue->getInFlightCount(),0u);
}
TEST(CanonicalOnlyQueue, CountAndByteLimitsRemainRequired) {
    unsigned calls=0;const auto task=std::make_shared<CanonicalOnlyFixtureTask>(calls);
    auto config=consensus::ValidationQueue::Config::forNormalOperation();config.max_queued_blocks=0;
    {auto queue=consensus::ValidationQueue::CreateCanonicalOnly(config);queue->start();
     EXPECT_FALSE(queue->submitAndWait(task).accepted());EXPECT_EQ(calls,0u);
     EXPECT_EQ(queue->getMetrics().blocks_submitted.load(),0u);}
    config.max_queued_blocks=1;config.max_canonical_wire_bytes=128;
    auto queue=consensus::ValidationQueue::CreateCanonicalOnly(config);queue->start();
    EXPECT_FALSE(queue->submitAndWait(std::make_shared<CanonicalOnlyFixtureTask>(calls,CanonicalOnlyFixtureTask::Reply::Connected,129)).accepted());
    EXPECT_FALSE(queue->submitAndWait(std::make_shared<CanonicalOnlyFixtureTask>(calls,CanonicalOnlyFixtureTask::Reply::Connected,0)).accepted());
    EXPECT_FALSE(queue->submitAndWait(std::shared_ptr<const consensus::CanonicalBlockTask>{}).accepted());
    EXPECT_EQ(calls,0u);ASSERT_TRUE(queue->submitAndWait(task).accepted());EXPECT_EQ(calls,1u);
    EXPECT_EQ(queue->getQueuedCount(),0u);EXPECT_EQ(queue->getInFlightCount(),0u);
}
TEST(CanonicalOnlyQueue, IdentityAndExceptionRefuseWithoutAcknowledgment) {
    auto queue=consensus::ValidationQueue::CreateCanonicalOnly();queue->start();unsigned calls=0;
    for(const auto reply:{CanonicalOnlyFixtureTask::Reply::WrongHash,CanonicalOnlyFixtureTask::Reply::WrongHeight,
                         CanonicalOnlyFixtureTask::Reply::Disconnected,CanonicalOnlyFixtureTask::Reply::Refused,
                         CanonicalOnlyFixtureTask::Reply::Exception}) {
        const auto task=std::make_shared<CanonicalOnlyFixtureTask>(calls,reply);
        const auto result=queue->submitAndWait(task);EXPECT_FALSE(result.accepted());EXPECT_FALSE(result.connected);
        EXPECT_EQ(result.block_hash,task->hash);EXPECT_EQ(result.height,task->height);
        EXPECT_EQ(queue->getMetrics().blocks_connected.load(),0u);EXPECT_EQ(queue->getTotalProcessed(),0u);
    }
    EXPECT_EQ(calls,5u);EXPECT_EQ(queue->getMetrics().blocks_failed.load(),5u);
    EXPECT_TRUE(queue->submitAndWait(std::make_shared<CanonicalOnlyFixtureTask>(calls)).connected);
    EXPECT_EQ(calls,6u);EXPECT_EQ(queue->getTotalProcessed(),1u);
}
TEST(CanonicalOnlyQueue, BeforeStartAndAfterStopRefuse) {
    auto queue=consensus::ValidationQueue::CreateCanonicalOnly();unsigned calls=0;
    auto task=std::make_shared<CanonicalOnlyFixtureTask>(calls);
    EXPECT_FALSE(queue->submitAndWait(task).accepted());EXPECT_EQ(calls,0u);
    queue->start();ASSERT_TRUE(queue->submitAndWait(task).connected);queue->stop();
    EXPECT_FALSE(queue->isRunning());EXPECT_FALSE(queue->submitAndWait(task).accepted());EXPECT_EQ(calls,1u);
    EXPECT_EQ(queue->getQueuedCount(),0u);EXPECT_EQ(queue->getInFlightCount(),0u);
}
} // namespace dinero
