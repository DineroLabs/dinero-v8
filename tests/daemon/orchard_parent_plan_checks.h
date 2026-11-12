#pragma once
#include <condition_variable>
#include <future>
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct ForwardBacklogNotices final:RuntimeBlockNotifications {
    unsigned published=0,reorgs=0;
    struct Event final:PreparedRuntimeBlockNotifications {
        ForwardBacklogNotices& owner;
        explicit Event(ForwardBacklogNotices& value):owner(value){}
        void PublishAfterCommit()noexcept override{++owner.published;}
    };
    struct Plan final:PreparedRuntimeReorgNotifications {
        ForwardBacklogNotices& owner;
        explicit Plan(ForwardBacklogNotices& value):owner(value){}
        void Finish(RuntimeReorgProgress progress)noexcept override {
            EXPECT_TRUE(progress.complete);++owner.reorgs;
        }
    };
    std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(
        const RuntimeBlockBody& body,uint32_t height,RuntimeBlockDirection)override {
        OrchardAdmissionFixture::Require(body.IsOrchardProfile() && height>=102);
        return std::make_unique<Event>(*this);
    }
    std::unique_ptr<PreparedRuntimeReorgNotifications> PrepareReorg(
        std::shared_ptr<const RuntimeReorgPlan> plan)override {
        OrchardAdmissionFixture::Require(bool(plan));return std::make_unique<Plan>(*this);
    }
};
auto PrepareFullForwardBacklog(CanonicalPoolFixture& f) {
    auto notices=std::make_shared<ForwardBacklogNotices>();
    f.f.service->setRuntimeBlockNotifications(notices);
    const auto connect=[&](const OrchardMiningTemplate& body) {
        auto header=body.Header();arith_uint256 compact;compact.SetCompact(header.difficulty);
        const auto target=uint256::FromHexUnsafe(compact.GetHex());
        for(header.nonce=0;header.nonce<100000 && target<header.GetHash();++header.nonce){}
        OrchardAdmissionFixture::Require(!(target<header.GetHash()));
        auto wire=body.WireBytes();const auto prefix=header.SerializeForHash();
        std::copy(prefix.begin(),prefix.end(),wire.begin());
        OrchardAdmissionFixture::Require(f.Submit(wire).connected);
        return f.f.service->GetActiveTip();
    };
    const auto first=f.Build();OrchardAdmissionFixture::Require(bool(first));
    std::array<CBlockIndex*,3> nodes{connect(*first),nullptr,nullptr};
    BlockAssembler assembler(&f.f.db);WireOrchardAssembler(assembler,f.f);
    for(size_t i=1;i<nodes.size();++i) {
        const auto next=assembler.CreateOrchardBlock(OrchardMiningPayout);
        OrchardAdmissionFixture::Require(bool(next));nodes[i]=connect(*next);
    }
    std::string error;
    OrchardAdmissionFixture::Require(f.f.service->InvalidateBlock(nodes[1]->hash,error));
    OrchardAdmissionFixture::Require(f.f.service->GetActiveTip()==nodes[0]);
    OrchardAdmissionFixture::Require(notices->reorgs==1);
    OrchardAdmissionFixture::Require(f.f.service->ReconsiderBlock(nodes[1]->hash,error));
    OrchardAdmissionFixture::Require(f.f.service->GetActiveTip()==nodes[0]);
    return std::pair{nodes,notices};
}
}
TEST(OrchardParentPlan, FullForwardBacklogUsesSelectedExtensionsWithoutHistoricalReplay) {
    CanonicalPoolFixture f(true);const auto [nodes,notices]=PrepareFullForwardBacklog(f);
    const auto genesis=f.f.blocks.front();
    ASSERT_EQ(f.f.db.deleteBlock(f.f.token,genesis.GetHash()),Status::Ok);
    // The selected full owner and catalog were genuinely validated above.
    // A queued straight continuation must use that owner, not replay genesis
    // merely because more than one retained successor is available.
    const auto published=notices->published;
    f.f.service->ActivateBestChain();
    EXPECT_EQ(f.f.service->GetActiveTip(),nodes[1]);EXPECT_EQ(notices->published,published+1);
    f.f.service->ActivateBestChain();
    EXPECT_EQ(f.f.service->GetActiveTip(),nodes[2]);EXPECT_EQ(notices->published,published+2);
    EXPECT_EQ(nodes[1]->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    EXPECT_EQ(nodes[2]->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    ASSERT_EQ(f.f.db.putBlock(f.f.token,genesis.GetHash(),genesis),Status::Ok);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
TEST(OrchardParentPlan, ForwardPrefixStillRefusesAStaleOperatorGeneration) {
    CanonicalPoolFixture f(true);const auto [nodes,notices]=PrepareFullForwardBacklog(f);
    auto plan=ShieldedStateStartupTestAccess::CaptureActivationPlan(*f.f.service);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::CompleteActivationPlan(*f.f.service,plan));
    std::string error;
    ASSERT_TRUE(f.f.service->InvalidateBlock(nodes[1]->hash,error))<<error;
    ASSERT_TRUE(f.f.service->ReconsiderBlock(nodes[1]->hash,error))<<error;
    const auto published=notices->published;
    ShieldedStateStartupTestAccess::ApplyActivationPlan(*f.f.service,plan);
    EXPECT_EQ(f.f.service->GetActiveTip(),nodes[0]);EXPECT_EQ(notices->published,published);
    f.f.service->ActivateBestChain();
    EXPECT_EQ(f.f.service->GetActiveTip(),nodes[1]);EXPECT_EQ(notices->published,published+1);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
TEST(OrchardParentPlan, ConcurrentRootCallersDeferAndOneStoredActivationCommits) {
    CanonicalPoolFixture f(true);const auto built=f.Build();ASSERT_TRUE(built);
    auto header=built->Header();arith_uint256 work_target;work_target.SetCompact(header.difficulty);
    const auto target=uint256::FromHexUnsafe(work_target.GetHex());
    for(header.nonce=0;header.nonce<100000 && target<header.GetHash();++header.nonce){}
    ASSERT_FALSE(target<header.GetHash());
    auto* next=RetainActivationCandidate(f,*built,header.nonce);
    // Hold a genuine selected-state publication interval while concurrent
    // network/tick callers arrive. One root may wait for the selected view;
    // the others must defer before making another detached activation owner.
    auto selected=f.f.service->AcquireBlockIngressActivationLock();
    constexpr size_t callers=6;
    std::promise<void> start;auto started=start.get_future().share();
    std::mutex completed_mutex;std::condition_variable completed_cv;
    size_t completed=0;
    std::vector<std::future<void>> pending;
    for(size_t i=0;i<callers;++i)pending.push_back(std::async(std::launch::async,[&] {
        started.wait();f.f.service->ActivateBestChain();
        {std::lock_guard lock(completed_mutex);++completed;}
        completed_cv.notify_one();
    }));
    start.set_value();
    bool deferred=false;
    {
        std::unique_lock lock(completed_mutex);
        deferred=completed_cv.wait_for(lock,std::chrono::seconds(5),[&]{return completed>=callers-1;});
        EXPECT_EQ(completed,callers-1);
    }
    EXPECT_TRUE(deferred);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    EXPECT_EQ(next->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    f.f.CheckUnpublished();
    selected.unlock();
    for(auto& call:pending)EXPECT_NO_THROW(call.get());
    EXPECT_EQ(f.f.service->GetActiveTip(),next);EXPECT_EQ(f.notices->published,1u);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    f.f.service->ActivateBestChain();
    EXPECT_EQ(f.f.service->GetActiveTip(),next);EXPECT_EQ(f.notices->published,1u);
}
TEST(OrchardParentPlan, UnchangedCapturedPlanCommitsThroughActualSecondPass) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    auto* next=RetainActivationCandidate(f,*built);
    auto plan=ShieldedStateStartupTestAccess::CaptureActivationPlan(*f.f.service);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    f.f.CheckUnpublished();
    ASSERT_TRUE(ShieldedStateStartupTestAccess::CompleteActivationPlan(*f.f.service,plan));
    ShieldedStateStartupTestAccess::ApplyActivationPlan(*f.f.service,plan);
    EXPECT_EQ(f.f.service->GetActiveTip(),next);EXPECT_EQ(f.notices->published,1u);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
TEST(OrchardParentPlan, StoredActivationSharesBoundaryProofAndDefersWhilePreparationBusy) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    auto* next=RetainActivationCandidate(f,*built);
    auto shared=f.f.service->PrepareSelectedOrchardParent();ASSERT_TRUE(shared);
    const auto* identity=&shared->Record();
    auto plan=ShieldedStateStartupTestAccess::CaptureActivationPlan(*f.f.service);
    std::promise<void> locked,release;
    auto released=release.get_future();
    std::thread preparing([&] {
        auto busy=ShieldedStateStartupTestAccess::LockBoundaryPreparation(*f.f.service);
        locked.set_value();released.wait();
    });
    locked.get_future().wait();
    const bool completed=ShieldedStateStartupTestAccess::CompleteActivationPlan(*f.f.service,plan);
    release.set_value();preparing.join();
    EXPECT_FALSE(completed);EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);f.f.CheckUnpublished();
    ASSERT_TRUE(ShieldedStateStartupTestAccess::CompleteActivationPlan(*f.f.service,plan));
    EXPECT_EQ(ShieldedStateStartupTestAccess::ActivationParentRecord(plan),identity);
    ShieldedStateStartupTestAccess::ApplyActivationPlan(*f.f.service,plan);
    EXPECT_EQ(f.f.service->GetActiveTip(),next);EXPECT_EQ(f.notices->published,1u);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
TEST(OrchardParentPlan, SharedBoundaryPlanStillRejectsChangedDurableCoins) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    auto* next=RetainActivationCandidate(f,*built);
    auto shared=f.f.service->PrepareSelectedOrchardParent();ASSERT_TRUE(shared);
    auto plan=ShieldedStateStartupTestAccess::CaptureActivationPlan(*f.f.service);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::CompleteActivationPlan(*f.f.service,plan));
    const auto point=f.f.replay->ProvenUtxos().begin()->first;
    const auto original=f.f.db.getCoin(point.txid.AsUint256(),point.vout);ASSERT_TRUE(original.ok());
    auto wrong=*original;++wrong.amount;
    ASSERT_EQ(f.f.db.putCoin(f.f.token,point.txid.AsUint256(),point.vout,wrong),Status::Ok);
    ShieldedStateStartupTestAccess::ApplyActivationPlan(*f.f.service,plan);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    f.f.CheckUnpublished();EXPECT_EQ(next->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    ASSERT_EQ(f.f.db.putCoin(f.f.token,point.txid.AsUint256(),point.vout,*original),Status::Ok);
    f.f.service->ActivateBestChain();
    EXPECT_EQ(f.f.service->GetActiveTip(),next);EXPECT_EQ(f.notices->published,1u);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
TEST(OrchardParentPlan, SerializedOperatorDecisionsInvalidateCapturedPlan) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    auto* next=RetainActivationCandidate(f,*built);const auto original_status=next->status;
    const auto before=ShieldedStateStartupTestAccess::OperatorGeneration(*f.f.service);
    ASSERT_TRUE(before.usable());
    auto plan=ShieldedStateStartupTestAccess::CaptureActivationPlan(*f.f.service);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::CompleteActivationPlan(*f.f.service,plan));
    // Real serialized operator decisions on the retained noncanonical child.
    // No races, barriers, fabricated counters or changes to locking.
    std::string error;
    ASSERT_TRUE(f.f.service->InvalidateBlock(next->hash,error))<<error;
    EXPECT_NE(next->status&BLOCK_FAILED_VALID,0u);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    ASSERT_TRUE(f.f.service->ReconsiderBlock(next->hash,error))<<error;
    EXPECT_EQ(next->status,original_status);
    const auto after=ShieldedStateStartupTestAccess::OperatorGeneration(*f.f.service);
    ASSERT_TRUE(after.usable());EXPECT_EQ(after.value,before.value+2);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    ShieldedStateStartupTestAccess::ApplyActivationPlan(*f.f.service,plan);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    EXPECT_EQ(next->status,original_status);EXPECT_EQ(f.f.ingress->mempool().size(),1u);
    f.f.CheckUnpublished();
    f.f.service->PumpReplayMetadataRecovery();
    EXPECT_EQ(f.f.service->GetActiveTip(),next);EXPECT_EQ(f.notices->published,1u);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
TEST(OrchardParentPlan, RelocatedExactBodyInvalidatesCapturedPlan) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    auto* next=RetainActivationCandidate(f,*built);
    auto plan=ShieldedStateStartupTestAccess::CaptureActivationPlan(*f.f.service);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::CompleteActivationPlan(*f.f.service,plan));
    const auto before=ShieldedStateStartupTestAccess::OperatorGeneration(*f.f.service);
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        std::lock_guard<std::recursive_mutex> graph(dinero::g_block_index_mutex);
        auto metadata=f.f.db.getHeaderMetadata(next->hash);ASSERT_TRUE(metadata.ok());
        const auto position=f.files->writeBlockBytes(next->hash,
            {built->WireBytes().begin(),built->WireBytes().end()});ASSERT_TRUE(position.ok());
        ASSERT_TRUE(position->file_number!=next->file_number || position->offset!=next->data_pos);
        metadata->file_number=position->file_number;metadata->data_pos=position->offset;
        metadata->data_size=position->size;
        ASSERT_EQ(f.f.db.putHeaderMetadata(f.f.token,next->hash,*metadata),Status::Ok);
        next->file_number=metadata->file_number;next->data_pos=metadata->data_pos;
        next->data_size=metadata->data_size;
    }
    const auto after=ShieldedStateStartupTestAccess::OperatorGeneration(*f.f.service);
    EXPECT_EQ(after.state,before.state);EXPECT_EQ(after.value,before.value);
    ShieldedStateStartupTestAccess::ApplyActivationPlan(*f.f.service,plan);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    EXPECT_EQ(next->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    EXPECT_EQ(f.f.ingress->mempool().size(),1u);f.f.CheckUnpublished();
    f.f.service->PumpReplayMetadataRecovery();
    EXPECT_EQ(f.f.service->GetActiveTip(),next);EXPECT_EQ(f.notices->published,1u);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
#endif
