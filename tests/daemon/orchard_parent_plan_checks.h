#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
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
