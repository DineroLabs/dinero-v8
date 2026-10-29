#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardParentRetry, RootMaintenanceConsumesRecursiveRequestWithValidatedHeaders) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    auto* next=RetainActivationCandidate(f,*built);
    {
        auto caller=f.f.service->AcquireBlockIngressActivationLock();
        f.f.service->ActivateBestChain();
        ASSERT_EQ(ShieldedStateStartupTestAccess::ParentRetryCandidate(*f.f.service),next->hash);
        f.f.service->PumpReplayMetadataRecovery();
        EXPECT_TRUE(caller.owns_lock());EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);
        EXPECT_EQ(f.notices->published,0u);f.f.CheckUnpublished();
    }
    // Call the real public stateful maintenance entry. Keep its real header
    // owner: the connector validates both parent and child against it.
    // P2P's height-gated safety-net caller is not invoked by this case.
    f.f.service->PumpReplayMetadataRecovery();
    EXPECT_EQ(f.f.service->GetActiveTip(),next);EXPECT_EQ(f.notices->published,1u);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    const auto due=ShieldedStateStartupTestAccess::ParentRetryAfter(*f.f.service);
    ShieldedStateStartupTestAccess::PumpParentRetryAt(*f.f.service,due);
    EXPECT_FALSE(ShieldedStateStartupTestAccess::ParentRetryCandidate(*f.f.service));
    EXPECT_EQ(f.notices->published,1u);
}
TEST(OrchardParentRetry, FailedReplaySurvivesCooldownAndRetriesAfterRepair) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    auto* next=RetainActivationCandidate(f,*built);
    auto changed=f.f.blocks.front();changed.vtx.front().vout.front().value=AmountUna::Una(1);
    ASSERT_EQ(f.f.db.putBlock(f.f.token,changed.GetHash(),changed),Status::Ok);
    f.f.service->ActivateBestChain();
    f.f.service->PumpReplayMetadataRecovery();
    ASSERT_EQ(ShieldedStateStartupTestAccess::ParentRetryCandidate(*f.f.service),next->hash);
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    f.f.CheckUnpublished();EXPECT_EQ(next->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    const auto due=ShieldedStateStartupTestAccess::ParentRetryAfter(*f.f.service);
    ASSERT_EQ(f.f.db.putBlock(f.f.token,f.f.blocks.front().GetHash(),f.f.blocks.front()),Status::Ok);
    // Sequential controlled-clock boundary; no sleeps, threads or removed locks.
    ShieldedStateStartupTestAccess::PumpParentRetryAt(*f.f.service,due-std::chrono::milliseconds(1));
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    EXPECT_EQ(ShieldedStateStartupTestAccess::ParentRetryAfter(*f.f.service),due);
    ShieldedStateStartupTestAccess::PumpParentRetryAt(*f.f.service,due);
    EXPECT_EQ(f.f.service->GetActiveTip(),next);EXPECT_EQ(f.notices->published,1u);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    EXPECT_EQ(ShieldedStateStartupTestAccess::ParentRetryAfter(*f.f.service),due+std::chrono::seconds(30));
}
TEST(OrchardParentRetry, RemovedCandidateRetiresRequestWithoutEffects) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    auto* next=RetainActivationCandidate(f,*built);
    {
        auto caller=f.f.service->AcquireBlockIngressActivationLock();
        f.f.service->ActivateBestChain();
    }
    ASSERT_EQ(ShieldedStateStartupTestAccess::ParentRetryCandidate(*f.f.service),next->hash);
    f.f.service->RemoveCandidate(next);
    f.f.service->PumpReplayMetadataRecovery();
    EXPECT_FALSE(ShieldedStateStartupTestAccess::ParentRetryCandidate(*f.f.service));
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    EXPECT_EQ(next->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);f.f.CheckUnpublished();
}
TEST(OrchardParentRetry, MissingHeaderOwnerPreservesCandidateAndRequest) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    auto* next=RetainActivationCandidate(f,*built);
    {
        auto caller=f.f.service->AcquireBlockIngressActivationLock();
        f.f.service->ActivateBestChain();
    }
    // Reproduce the earlier fixture's unavailable required owner explicitly as
    // a refusal case. Production checks must remain intact.
    f.f.service->setHeaderChainSelector(nullptr);
    f.f.service->PumpReplayMetadataRecovery();
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    EXPECT_EQ(ShieldedStateStartupTestAccess::ParentRetryCandidate(*f.f.service),next->hash);
    EXPECT_EQ(next->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    EXPECT_EQ(f.f.ingress->mempool().size(),1u);f.f.CheckUnpublished();
}
#endif
