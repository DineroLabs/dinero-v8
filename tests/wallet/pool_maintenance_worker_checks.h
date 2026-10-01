#pragma once
#include "daemon/runtime_notification_composition.h"
#include "daemon/runtime_block_reader.h"
namespace {
class PoolMaintenanceWorker : public PoolPaymentAttempt {
protected:
    using Report=dinero::pool::PoolManager::MaintenanceReport;
    void without_funding() {
        auto config=pool->getConfig();config.payment_funding.reset();ASSERT_TRUE(pool->setConfig(config));
        pool->setChainstateSource(chain);
    }
    Report wait_pass(uint64_t previous=0) {
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        auto report=pool->MaintenanceSnapshot();
        while(report.passes<=previous && report.running && std::chrono::steady_clock::now()<end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));report=pool->MaintenanceSnapshot();
        }
        EXPECT_GT(report.passes,previous);return report;
    }
    void TearDown() override {if(pool)pool->stopMaintenanceThread();PoolPaymentAttempt::TearDown();}
};
TEST_F(PoolMaintenanceWorker, InitialPassAndMailboxSurviveOnlyAsStoppedState) {
    without_funding();const auto origin=pool->getDatabase().getPayoutsForBlock(1)[0].allocation_origin;
    auto wake=pool->CaptureMaintenanceWakeHandle();auto adapter=dinero::MakeRuntimePoolNotifications(*pool);
    dinero::RuntimeBlockBody body{canonical->blocks.back()};
    EXPECT_FALSE(wake.Running());EXPECT_FALSE(adapter->Prepare(body,101,dinero::RuntimeBlockDirection::Connect));
    pool->startMaintenanceThread();pool->startMaintenanceThread();const auto report=wait_pass();
    EXPECT_TRUE(report.running);EXPECT_TRUE(report.accounting_pass_returned);EXPECT_TRUE(report.maintenance_pass_returned);
    auto token=adapter->Prepare(body,101,dinero::RuntimeBlockDirection::Connect);ASSERT_TRUE(token);
    pool->stopMaintenanceThread();pool->stopMaintenanceThread();EXPECT_FALSE(wake.Running());
    EXPECT_EQ(pool->getDatabase().getRecordedBlocks()[0].confirmations,101u);
    EXPECT_EQ(pool->getDatabase().getPayoutsForBlock(1)[0].allocation_origin,origin);unpaid();
    EXPECT_TRUE(attempts().empty());EXPECT_TRUE(service->get().getPendingPayments().empty());
    EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
    pool.reset();
    // The manager is already stopped outside selected ownership. These tokens
    // retain only a mailbox, with no source/pool/thread join or callback.
    {auto selected=chain->AcquireBlockIngressActivationLock();token->PublishAfterCommit();token.reset();wake.RequestReplay();}
    EXPECT_FALSE(wake.Running());EXPECT_FALSE(adapter->Prepare(body,101,dinero::RuntimeBlockDirection::Connect));
}
TEST_F(PoolMaintenanceWorker, DeferredSourcePassPreservesRowsThenExplicitWakeRetries) {
    without_funding();const auto before=pool->getDatabase().getRecordedBlocks();const auto tip=canonical->db.getTip();ASSERT_TRUE(tip.ok());
    auto wrong=tip->hash;wrong.begin()[0]^=1;
    {auto selected=chain->AcquireBlockIngressActivationLock();ASSERT_EQ(canonical->db.setTip(canonical->token,wrong,tip->height,tip->work),dinero::Status::Ok);}
    pool->startMaintenanceThread();const auto deferred=wait_pass();pool->stopMaintenanceThread();
    EXPECT_FALSE(deferred.accounting_pass_returned);EXPECT_FALSE(deferred.maintenance_pass_returned);
    EXPECT_EQ(pool->getDatabase().getRecordedBlocks(),before);unpaid();EXPECT_TRUE(attempts().empty());EXPECT_EQ(ingress->submits,0);
    {auto selected=chain->AcquireBlockIngressActivationLock();ASSERT_EQ(canonical->db.setTip(canonical->token,tip->hash,tip->height,tip->work),dinero::Status::Ok);}
    pool->startMaintenanceThread();auto restored=wait_pass();
    auto adapter=dinero::MakeRuntimePoolNotifications(*pool);dinero::RuntimeBlockBody body{canonical->blocks.back()};
    auto token=adapter->Prepare(body,101,dinero::RuntimeBlockDirection::Disconnect);ASSERT_TRUE(token);
    token->PublishAfterCommit();restored=wait_pass(restored.passes);pool->stopMaintenanceThread();
    EXPECT_TRUE(restored.accounting_pass_returned);EXPECT_TRUE(restored.maintenance_pass_returned);
    EXPECT_EQ(pool->getDatabase().getRecordedBlocks()[0].confirmations,101u);unpaid();EXPECT_TRUE(attempts().empty());EXPECT_EQ(ingress->submits,0);
}
TEST_F(PoolMaintenanceWorker, RetainedReorgPlanWakesWithoutClaimingDeliveryAcknowledgement) {
    without_funding();std::shared_ptr<const dinero::RuntimeReorgPlan> plan;
    dinero::CBlockIndex parent(canonical->blocks[100].header,100),tip(canonical->blocks[101].header,101);tip.pprev=&parent;
    const auto metadata=canonical->db.getHeaderMetadata(tip.hash);ASSERT_TRUE(metadata.ok());
    tip.status=metadata->status_flags;tip.file_number=metadata->file_number;tip.data_pos=metadata->data_pos;tip.data_size=metadata->data_size;tip.chainwork=metadata->chainwork.GetHex();
    {auto selected=chain->AcquireBlockIngressActivationLock();const std::array<dinero::CBlockIndex*,1> disconnect{&tip};plan=dinero::ReadRuntimeReorgPlanUnderLock(canonical->db,canonical->files.get(),disconnect,{});}
    ASSERT_TRUE(plan);ASSERT_EQ(plan->disconnect.size(),1u);
    EXPECT_EQ(plan->disconnect[0].body.Historical().Serialize(),canonical->blocks[101].Serialize());
    auto adapter=dinero::MakeRuntimePoolNotifications(*pool);EXPECT_FALSE(adapter->PrepareReorg({}));EXPECT_FALSE(adapter->PrepareReorg(plan));
    pool->startMaintenanceThread();auto report=wait_pass();auto token=adapter->PrepareReorg(plan);ASSERT_TRUE(token);
    token->Finish({1,0,false});report=wait_pass(report.passes);EXPECT_TRUE(report.accounting_pass_returned);
    auto late=adapter->PrepareReorg(plan);ASSERT_TRUE(late);pool->stopMaintenanceThread();auto wake=pool->CaptureMaintenanceWakeHandle();
    EXPECT_TRUE(attempts().empty());unpaid();pool.reset();
    {auto selected=chain->AcquireBlockIngressActivationLock();late->Finish({1,0,true});late.reset();}
    EXPECT_FALSE(wake.Running());EXPECT_FALSE(adapter->PrepareReorg(plan));
    // Actual retained body reader + ordinary mailbox handoff only: no canonical
    // reorg execution, durable consumer cursor, or all-consumer acknowledgement.
}
TEST_F(PoolMaintenanceWorker, BackgroundWalletCallbacksRunAfterSelectedOwnerRelease) {
    pool->setChainstateSource(chain);healthy_preflight();const auto original=ingress->test;
    ingress->test=[&,original](const dinero::Transaction& tx){EXPECT_FALSE(dinero::WalletBatchPaymentTestAccess::SelectedHeld(*chain));return original(tx);};
    ingress->submit=[&](const dinero::Transaction& tx){EXPECT_FALSE(dinero::WalletBatchPaymentTestAccess::SelectedHeld(*chain));submission(tx);return dinero::TxAcceptResult::Accepted(tx.GetTxid().AsUint256());};
    pool->startMaintenanceThread();const auto report=wait_pass();pool->stopMaintenanceThread();
    EXPECT_TRUE(report.accounting_pass_returned);EXPECT_TRUE(report.maintenance_pass_returned);
    EXPECT_EQ(ingress->tests,1);EXPECT_EQ(ingress->submits,1);auto saved=attempts();ASSERT_EQ(saved.size(),1u);EXPECT_TRUE(saved[0].retained);
    ASSERT_EQ(service->get().getPendingPayments().size(),1u);unpaid();
    pool->ClosePayments();EXPECT_THROW(pool->startMaintenanceThread(),std::logic_error);EXPECT_FALSE(pool->CaptureMaintenanceWakeHandle().Running());
    EXPECT_EQ(attempts(),saved);EXPECT_EQ(ingress->submits,1);
}
TEST_F(PoolMaintenanceWorker, HistoricalReaderRefusesUnavailableOrMalformedProfile) {
    // Ordinary read/refusal only; no background worker or ownership removal.
    struct RestoreParams {
        dinero::ChainParams previous=dinero::Params();
        ~RestoreParams(){dinero::MutableParams()=previous;}
    } restore;
    auto selected=chain->AcquireBlockIngressActivationLock();
    const auto& block=canonical->blocks[101];const auto bytes=block.Serialize();
    auto read=[&]{return dinero::ReadRuntimeBlockUnderLock(canonical->db,canonical->files.get(),block.GetHash(),101);};
    const auto before=read();ASSERT_TRUE(before.ok());EXPECT_FALSE(before->IsOrchardProfile());
    EXPECT_EQ(before->Serialize(),std::vector<uint8_t>(bytes.begin(),bytes.end()));
    dinero::MutableParams().orchard_activation_height=101;
    dinero::MutableParams().orchard_branch_id=0;
    EXPECT_EQ(read().status(),dinero::Status::Internal);
#if !DINERO_TEST_RUNTIME_ORCHARD
    dinero::MutableParams().orchard_branch_id=1;
    // A valid selected profile without its backend is unavailable, even if
    // historical bytes can be read successfully at the very same height.
    EXPECT_EQ(read().status(),dinero::Status::Internal);
#endif
    dinero::MutableParams()=restore.previous;
    const auto after=read();ASSERT_TRUE(after.ok());EXPECT_FALSE(after->IsOrchardProfile());
    EXPECT_EQ(after->Serialize(),before->Serialize());
    EXPECT_EQ(canonical->db.getTip()->hash,block.GetHash());
}
} // namespace
