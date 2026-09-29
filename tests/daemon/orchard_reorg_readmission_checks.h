#pragma once
#include "daemon/runtime_reorg_readmission.h"
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "daemon/runtime_reorg_store.h"
#endif
namespace dinero {
TEST(OrchardReorgReadmission, MissingOwnerAndUnavailableBackendRefuse) {
    ChainstateService service;
    EXPECT_FALSE(service.readmitRuntimeReorg({}).ok());
    auto guard=service.AcquireBlockIngressActivationLock();
    EXPECT_EQ(service.readmitRuntimeReorg({}).status(),Status::Invalid);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct ReorgReadmissionNotices final:RuntimeBlockNotifications {
    struct Block final:PreparedRuntimeBlockNotifications {void PublishAfterCommit()noexcept override{}};
    struct Plan final:PreparedRuntimeReorgNotifications {void Finish(RuntimeReorgProgress)noexcept override{}};
    std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(const RuntimeBlockBody& body,uint32_t height,RuntimeBlockDirection)override {
        OrchardAdmissionFixture::Require(body.IsOrchardProfile() && height>=102);
        return std::make_unique<Block>();
    }
    std::unique_ptr<PreparedRuntimeReorgNotifications> PrepareReorg(std::shared_ptr<const RuntimeReorgPlan> plan)override {
        OrchardAdmissionFixture::Require(bool(plan));return std::make_unique<Plan>();
    }
};
struct ReorgReadmissionFixture:CanonicalPoolFixture {
    std::shared_ptr<ReorgReadmissionNotices> observer=std::make_shared<ReorgReadmissionNotices>();
    ReorgReadmissionFixture(){f.service->setRuntimeBlockNotifications(observer);}
    RuntimeOutboxCursor Prepare(const std::vector<CBlockIndex*>& disconnect) {
        auto selected=f.service->AcquireBlockIngressActivationLock();
        const auto plan=ReadRuntimeReorgPlanUnderLock(f.db,files.get(),disconnect,{});
        OrchardAdmissionFixture::Require(bool(plan));
        // Use the real retained-plan writer on exact indexed bodies. No
        // callback progress is provided to the recovery operation.
        const auto cursor=PersistRuntimeReorgIntentUnderLock(f.db,ChainWriteToken::CreateForTesting(),*plan);
        OrchardAdmissionFixture::Require(cursor.sequence!=0);return cursor;
    }
    std::string IntentHead(){std::string out;OrchardAdmissionFixture::Require(f.db.getRaw("runtime_reorg_intent:v1:head",out)==Status::Ok);return out;}
};
}
TEST(OrchardReorgReadmission, UncommittedIntentAndCheckedEndDoNotAdmit) {
    ReorgReadmissionFixture f;const auto block=f.Build();ASSERT_TRUE(block);ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());
    const auto cursor=f.Prepare({f.f.service->GetActiveTip()});const auto saved=f.IntentHead();
    const auto captured=f.f.service->readmitRuntimeReorg({});ASSERT_TRUE(captured.ok());
    ASSERT_TRUE((*captured)->intent);EXPECT_EQ(*(*captured)->intent,cursor);
    EXPECT_EQ((*captured)->planned_disconnects,1u);EXPECT_EQ((*captured)->matched_disconnects,0u);
    EXPECT_TRUE((*captured)->entries.empty());EXPECT_EQ(f.f.ingress->mempool().size(),0u);EXPECT_EQ(f.IntentHead(),saved);
    const auto end=f.f.service->readmitRuntimeReorg(cursor);ASSERT_TRUE(end.ok());EXPECT_FALSE((*end)->intent);EXPECT_TRUE((*end)->entries.empty());
    auto wrong=cursor;wrong.digest.data[0]^=1;EXPECT_FALSE(f.f.service->readmitRuntimeReorg(wrong).ok());
    EXPECT_EQ(f.IntentHead(),saved);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
}
TEST(OrchardReorgReadmission, CommittedPrefixReopenAndExactDuplicate) {
    ReorgReadmissionFixture f;const auto block=f.Build();ASSERT_TRUE(block);const auto body=block->Transactions()[1];
    ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());auto* lower=f.f.service->GetActiveTip();
    // A real signed historical child spends the typed parent's transparent
    // output. Reverse block traversal must restore this parent before its child.
    const MempoolTransaction child(SelectionSpend(f.f,OutPoint(body.GetTxid(),0),body.OutputCoin(0,102),100000));
    ASSERT_TRUE(f.f.ingress->SubmitBody(child,TxOrigin::INTERNAL).accepted());
    BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    const auto next=assembler.CreateOrchardBlock(OrchardMiningPayout);ASSERT_TRUE(next);ASSERT_EQ(next->Transactions().size(),2u);
    ASSERT_TRUE(f.Submit(next->WireBytes()).accepted());auto* upper=f.f.service->GetActiveTip();
    const auto cursor=f.Prepare({upper,lower});const auto saved=f.IntentHead();
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,upper));
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,lower));
    f.f.db.close();ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);unsigned relayed=0;
    auto& pool=f.f.ingress->mempool();pool.setTxBroadcastCallback([&](const uint256&){++relayed;});
    const auto recovered=f.f.service->readmitRuntimeReorg({});ASSERT_TRUE(recovered.ok());
    EXPECT_EQ((*recovered)->matched_disconnects,2u);ASSERT_EQ((*recovered)->entries.size(),2u);
    EXPECT_EQ((*recovered)->entries[0].body.Serialize(),body.Serialize());
    EXPECT_EQ((*recovered)->entries[1].body.Serialize(),child.Serialize());
    for(const auto& entry:(*recovered)->entries){EXPECT_TRUE(entry.result.accepted())<<entry.result.message;EXPECT_TRUE(entry.present_after_attempt);}
    EXPECT_EQ(pool.size(),2u);EXPECT_EQ(relayed,0u);
    const auto again=f.f.service->readmitRuntimeReorg({});ASSERT_TRUE(again.ok());ASSERT_EQ((*again)->entries.size(),2u);
    for(const auto& entry:(*again)->entries){EXPECT_EQ(entry.result.code,TxRejectCode::ALREADY_IN_MEMPOOL);EXPECT_TRUE(entry.present_after_attempt);}
    EXPECT_EQ(pool.size(),2u);EXPECT_EQ(f.IntentHead(),saved);EXPECT_EQ(*(*again)->intent,cursor);
}
TEST(OrchardReorgReadmission, LaterCanonicalRefusalRetainsOriginalIntent) {
    ReorgReadmissionFixture f;const auto block=f.Build();ASSERT_TRUE(block);ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());
    f.Prepare({f.f.service->GetActiveTip()});const auto saved=f.IntentHead();auto* child=f.f.service->GetActiveTip();
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,child));
    ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());
    const auto refused=f.f.service->readmitRuntimeReorg({});ASSERT_TRUE(refused.ok());ASSERT_EQ((*refused)->entries.size(),1u);
    EXPECT_EQ((*refused)->matched_disconnects,1u);EXPECT_FALSE((*refused)->entries[0].present_after_attempt);
    EXPECT_FALSE((*refused)->entries[0].result.accepted());EXPECT_EQ(f.f.ingress->mempool().size(),0u);EXPECT_EQ(f.IntentHead(),saved);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,child));
    const auto retry=f.f.service->readmitRuntimeReorg({});ASSERT_TRUE(retry.ok());ASSERT_EQ((*retry)->entries.size(),1u);
    EXPECT_TRUE((*retry)->entries[0].present_after_attempt);EXPECT_EQ(f.f.ingress->mempool().size(),1u);EXPECT_EQ(f.IntentHead(),saved);
}
TEST(OrchardReorgReadmission, InterruptedTwoBlockPlanUsesOnlyCommittedPrefix) {
    ReorgReadmissionFixture f;const auto first=f.Build();ASSERT_TRUE(first);ASSERT_TRUE(f.Submit(first->WireBytes()).accepted());
    auto* lower=f.f.service->GetActiveTip();BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    const auto second=assembler.CreateOrchardBlock(OrchardMiningPayout);ASSERT_TRUE(second);ASSERT_EQ(second->Transactions().size(),1u);
    ASSERT_TRUE(f.Submit(second->WireBytes()).accepted());auto* upper=f.f.service->GetActiveTip();f.Prepare({upper,lower});
    const auto saved=f.IntentHead();ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,upper));
    const auto prefix=f.f.service->readmitRuntimeReorg({});ASSERT_TRUE(prefix.ok());EXPECT_EQ((*prefix)->planned_disconnects,2u);
    EXPECT_EQ((*prefix)->matched_disconnects,1u);EXPECT_TRUE((*prefix)->entries.empty());EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,lower));
    const auto all=f.f.service->readmitRuntimeReorg({});ASSERT_TRUE(all.ok());EXPECT_EQ((*all)->matched_disconnects,2u);
    ASSERT_EQ((*all)->entries.size(),1u);EXPECT_EQ((*all)->entries[0].body.Serialize(),first->Transactions()[1].Serialize());
    EXPECT_TRUE((*all)->entries[0].present_after_attempt);EXPECT_EQ(f.IntentHead(),saved);
}
TEST(OrchardReorgReadmission, WrongOwnerBoundsAndCorruptIntentRefuseBeforeAdmission) {
    ReorgReadmissionFixture f;const auto block=f.Build();ASSERT_TRUE(block);ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());
    f.Prepare({f.f.service->GetActiveTip()});ASSERT_TRUE(ShieldedStateStartupTestAccess::DisconnectBoundary(*f.f.service,f.f.service->GetActiveTip()));
    EXPECT_FALSE(f.f.service->readmitRuntimeReorg({},0).ok());EXPECT_FALSE(f.f.service->readmitRuntimeReorg({},2048,1).ok());
    {auto selected=f.f.service->AcquireBlockIngressActivationLock();EXPECT_EQ(f.f.service->readmitRuntimeReorg({}).status(),Status::Invalid);}
    f.context.mempool.reset();EXPECT_FALSE(f.f.service->readmitRuntimeReorg({}).ok());f.context.mempool=f.f.ingress;
    auto foreign=std::make_shared<ChainstateService>();f.context.chainstate=foreign;
    EXPECT_FALSE(f.f.service->readmitRuntimeReorg({}).ok());f.context.chainstate=f.f.service;
    const auto saved=f.IntentHead();auto damaged=saved;damaged.back()^=1;
    {auto selected=f.f.service->AcquireBlockIngressActivationLock();const auto token=ChainWriteToken::CreateForTesting();rocksdb::WriteBatch batch;batch.Put("runtime_reorg_intent:v1:head",damaged);ASSERT_EQ(f.f.db.writeBatch(token,std::move(batch),true),Status::Ok);}
    EXPECT_FALSE(f.f.service->readmitRuntimeReorg({}).ok());EXPECT_EQ(f.f.ingress->mempool().size(),0u);
    {auto selected=f.f.service->AcquireBlockIngressActivationLock();const auto token=ChainWriteToken::CreateForTesting();rocksdb::WriteBatch batch;batch.Put("runtime_reorg_intent:v1:head",saved);ASSERT_EQ(f.f.db.writeBatch(token,std::move(batch),true),Status::Ok);}
    const auto repaired=f.f.service->readmitRuntimeReorg({});ASSERT_TRUE(repaired.ok());ASSERT_EQ((*repaired)->entries.size(),1u);
    EXPECT_TRUE((*repaired)->entries[0].present_after_attempt);EXPECT_EQ(f.IntentHead(),saved);
}
#endif
} // namespace dinero
