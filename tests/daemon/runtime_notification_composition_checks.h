#pragma once
#include "daemon/runtime_notification_composition.h"
#include "daemon/runtime_block_reader.h"
#include "runtime_delivery_worker_checks.h"
namespace dinero {
namespace {
struct CompositionTrace final:RuntimeBlockNotifications {
    struct State {unsigned prepared=0,published=0,block_dropped=0,plans=0,finished=0;RuntimeReorgProgress progress;};
    std::shared_ptr<State> state=std::make_shared<State>();
    bool refuse=false,throw_prepare=false;
    std::shared_ptr<const RuntimeReorgPlan> received_plan;
    struct BlockToken final:PreparedRuntimeBlockNotifications {
        CompositionTrace& owner;bool published=false;
        explicit BlockToken(CompositionTrace& value):owner(value){}
        ~BlockToken() override {if(!published)++owner.state->block_dropped;}
        void PublishAfterCommit()noexcept override{published=true;++owner.state->published;}
    };
    struct PlanToken final:PreparedRuntimeReorgNotifications {
        CompositionTrace& owner;
        explicit PlanToken(CompositionTrace& value):owner(value){}
        void Finish(RuntimeReorgProgress p)noexcept override{++owner.state->finished;owner.state->progress=p;}
    };
    std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(const RuntimeBlockBody&,uint32_t,RuntimeBlockDirection) override {
        ++state->prepared;if(throw_prepare)throw std::runtime_error("patched consumer preparation refusal");
        if(refuse)return {};return std::make_unique<BlockToken>(*this);
    }
    std::unique_ptr<PreparedRuntimeReorgNotifications> PrepareReorg(std::shared_ptr<const RuntimeReorgPlan> plan) override {
        ++state->plans;received_plan=std::move(plan);
        if(throw_prepare)throw std::runtime_error("patched reorg preparation refusal");
        if(refuse)return {};return std::make_unique<PlanToken>(*this);
    }
};
auto CompositionBindings(std::shared_ptr<RuntimeBlockNotifications> first,std::shared_ptr<RuntimeBlockNotifications> second={}) {
    std::array<RuntimeConsumerBinding,static_cast<size_t>(RuntimeConsumerKind::Count)> out;
    for(size_t i=0;i<out.size();++i)out[i]={static_cast<RuntimeConsumerKind>(i),{}};
    out[0].consumer=std::move(first);out[1].consumer=std::move(second);return out;
}
}
TEST(RuntimeNotificationComposition, EveryFamilyRequiresAnExplicitUniqueDeclaration) {
    auto first=std::make_shared<CompositionTrace>();auto bindings=CompositionBindings(first);
    EXPECT_NO_THROW(RuntimeNotificationComposition{bindings});
    EXPECT_THROW((RuntimeNotificationComposition{std::span(bindings).first(bindings.size()-1)}),std::invalid_argument);
    auto bad=bindings;bad[1].kind=bad[0].kind;EXPECT_THROW((RuntimeNotificationComposition{bad}),std::invalid_argument);
    bad=bindings;bad[1].kind=RuntimeConsumerKind::Count;EXPECT_THROW((RuntimeNotificationComposition{bad}),std::invalid_argument);
    bad=bindings;bad[1].consumer=first;EXPECT_THROW((RuntimeNotificationComposition{bad}),std::invalid_argument);
    EXPECT_THROW((RuntimeNotificationComposition{CompositionBindings({})}),std::invalid_argument);
}
TEST(RuntimeNotificationComposition, CompletePreparationAndOwnedHandoffsPrecedePublication) {
    auto first=std::make_shared<CompositionTrace>(),second=std::make_shared<CompositionTrace>();
    auto a=first->state,b=second->state;RuntimeBlockBody body{Block{}};
    auto provider=std::make_shared<RuntimeNotificationComposition>(CompositionBindings(first,second));
    second->refuse=true;EXPECT_FALSE(provider->Prepare(body,0,RuntimeBlockDirection::Connect));
    EXPECT_EQ(a->prepared,1u);EXPECT_EQ(a->block_dropped,1u);EXPECT_EQ(a->published,0u);EXPECT_EQ(b->published,0u);
    second->refuse=false;second->throw_prepare=true;
    EXPECT_THROW(provider->Prepare(body,0,RuntimeBlockDirection::Disconnect),std::runtime_error);
    EXPECT_EQ(a->block_dropped,2u);EXPECT_EQ(a->published,0u);
    second->throw_prepare=false;auto handoff=provider->Prepare(body,0,RuntimeBlockDirection::Connect);ASSERT_TRUE(handoff);
    EXPECT_EQ(a->published,0u);EXPECT_EQ(b->published,0u);
    std::weak_ptr<CompositionTrace> weak_first=first,weak_second=second;
    first.reset();second.reset();provider.reset();EXPECT_FALSE(weak_first.expired());EXPECT_FALSE(weak_second.expired());
    handoff->PublishAfterCommit();EXPECT_EQ(a->published,1u);EXPECT_EQ(b->published,1u);
    handoff.reset();EXPECT_TRUE(weak_first.expired());EXPECT_TRUE(weak_second.expired());
}
TEST(RuntimeNotificationComposition, MailboxHandoffOutlivesStoppedWorkerWithoutOwningItsThread) {
    auto source=std::make_shared<ChainstateService>();
    auto worker=std::make_unique<DeliveryWorker>(source,nullptr,DeliveryWorkerFixtureLimits());
    auto adapter=MakeRuntimeWalletNotifications(*worker);RuntimeBlockBody body{Block{}};
    EXPECT_FALSE(adapter->Prepare(body,0,RuntimeBlockDirection::Connect));
    auto mailbox=worker->CaptureWakeHandle();EXPECT_FALSE(mailbox.Running());worker->Start();
    (void)WaitDeliveryWorker(*worker,[](const auto& r){return r.slices>0;});
    auto token=adapter->Prepare(body,0,RuntimeBlockDirection::Connect);ASSERT_TRUE(token);
    worker->Stop();worker.reset();EXPECT_FALSE(mailbox.Running());
    // The worker was stopped outside ownership. Handoff publication/destruction
    // now owns only a mailbox; the actual selected owner is safe to retain.
    {auto selected=source->AcquireBlockIngressActivationLock();token->PublishAfterCommit();token.reset();mailbox.RequestReplay();}
    EXPECT_FALSE(adapter->Prepare(body,0,RuntimeBlockDirection::Connect));
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(RuntimeNotificationComposition, ReorgPreparationUnwindsAndPreservesSharedPlanAndProgress) {
    ReorgReadmissionFixture f;const auto block=f.Build();ASSERT_TRUE(block);ASSERT_TRUE(f.Submit(block->WireBytes()).accepted());
    std::shared_ptr<const RuntimeReorgPlan> plan;
    {auto selected=f.f.service->AcquireBlockIngressActivationLock();const std::array<CBlockIndex*,1> disconnect{f.f.service->GetActiveTip()};plan=ReadRuntimeReorgPlanUnderLock(f.f.db,f.files.get(),disconnect,{});}
    ASSERT_TRUE(plan);auto first=std::make_shared<CompositionTrace>(),second=std::make_shared<CompositionTrace>();
    auto a=first->state,b=second->state;RuntimeNotificationComposition provider(CompositionBindings(first,second));
    EXPECT_FALSE(provider.PrepareReorg({}));EXPECT_EQ(a->plans,0u);
    second->refuse=true;EXPECT_FALSE(provider.PrepareReorg(plan));EXPECT_EQ(a->finished,1u);
    EXPECT_EQ(a->progress.disconnected,0u);EXPECT_EQ(a->progress.connected,0u);EXPECT_FALSE(a->progress.complete);
    EXPECT_EQ(first->received_plan,plan);EXPECT_EQ(second->received_plan,plan);EXPECT_EQ(b->finished,0u);
    second->refuse=false;second->throw_prepare=true;EXPECT_THROW(provider.PrepareReorg(plan),std::runtime_error);
    EXPECT_EQ(a->finished,2u);EXPECT_FALSE(a->progress.complete);
    second->throw_prepare=false;{auto prepared=provider.PrepareReorg(plan);ASSERT_TRUE(prepared);prepared->Finish({1,0,false});}
    EXPECT_EQ(a->finished,3u);EXPECT_EQ(b->finished,1u);EXPECT_EQ(a->progress.disconnected,1u);EXPECT_FALSE(a->progress.complete);
    {auto prepared=provider.PrepareReorg(plan);ASSERT_TRUE(prepared);prepared->Finish({1,0,true});}
    EXPECT_EQ(a->finished,4u);EXPECT_EQ(b->finished,2u);EXPECT_TRUE(a->progress.complete);EXPECT_TRUE(b->progress.complete);
}
TEST(RuntimeNotificationComposition, ActualTypedCommitRequiresAllPreparationsBeforePublication) {
    CanonicalPoolFixture f;const auto block=f.Build();ASSERT_TRUE(block);
    auto first=std::make_shared<CompositionTrace>(),second=std::make_shared<CompositionTrace>();
    auto provider=std::make_shared<RuntimeNotificationComposition>(CompositionBindings(first,second));
    f.f.service->setRuntimeBlockNotifications(provider);second->refuse=true;
    const auto refused=f.Submit(block->WireBytes());EXPECT_FALSE(refused.accepted());EXPECT_FALSE(refused.connected);
    EXPECT_EQ(first->state->published,0u);EXPECT_EQ(second->state->published,0u);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.f.ingress->mempool().size(),1u);f.f.CheckUnpublished();
    second->refuse=false;std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto accepted=f.Submit(block->WireBytes());ASSERT_TRUE(accepted.accepted())<<accepted.reason;
    EXPECT_EQ(first->state->published,1u);EXPECT_EQ(second->state->published,1u);
    EXPECT_EQ(f.f.service->GetActiveTip()->height,102u);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
}
TEST(RuntimeNotificationComposition, ActualCommittedWalletPrefixReplaysThroughMailboxNotification) {
    CanonicalRecoveryFixture f(true);f.MineAndAdopt();
    DeliveryWorker worker(f.f.service,f.wallet,DeliveryWorkerFixtureLimits());worker.Start();
    auto report=WaitDeliveryWorker(worker,[](const auto& r){return r.slices>0;});
    ASSERT_EQ(report.wallet,DeliveryWorker::WalletOutcome::AppliedPrefix);ASSERT_TRUE(report.wallet_head);
    ASSERT_EQ(report.wallet_head->sequence,1u);
    auto notices=MakeRuntimeWalletNotifications(worker);
    f.f.service->setRuntimeBlockNotifications(std::make_shared<RuntimeNotificationComposition>(CompositionBindings(notices)));
    f.MineEmpty();
    report=WaitDeliveryWorker(worker,[](const auto& r){return r.wallet_head&&r.wallet_head->sequence==2;});worker.Stop();
    EXPECT_EQ(report.wallet,DeliveryWorker::WalletOutcome::AppliedPrefix);
    EXPECT_EQ(f.ReadAccount().account.Delivery().sequence,2u);EXPECT_EQ(f.ReadAccount().account.Scan().BalanceUna(),500000u);
}
#endif
} // namespace dinero
