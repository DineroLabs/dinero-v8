#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardParentDrain, PreparedResultRefusesSameThreadStopBeforeAdmissionChanges) {
    OwnedSelectedParentFixture f;
    const auto owners=f.service_owner.use_count();
    auto prepared=f.service.PrepareSelectedOrchardParent(); ASSERT_TRUE(prepared);
    EXPECT_GT(f.service_owner.use_count(),owners);
    EXPECT_THROW(f.service.Stop(),std::logic_error);
    EXPECT_TRUE(f.service.CheckPreparedOrchardParent(*prepared));
    {
        auto guard=ChainstateService::AcquireMempoolChainstateRead(f.service_owner);
        EXPECT_TRUE(guard);
    }
    prepared.reset(); EXPECT_EQ(f.service_owner.use_count(),owners);
    EXPECT_NO_THROW(f.service.Stop());
    EXPECT_FALSE(f.service.PrepareSelectedOrchardParent());
    EXPECT_FALSE(ChainstateService::AcquireMempoolChainstateRead(f.service_owner));
    EXPECT_FALSE(ChainstateService::AcquireMiningReadGuard(f.service_owner));
    EXPECT_NO_THROW(f.service.Stop()); f.CheckUnpublished();
}
TEST(OrchardParentDrain, GuardLifetimePreservesNestedReadsAndStopClosesAdmission) {
    OrchardAdmissionFixture f;
    {
        auto guard=ChainstateService::AcquireMiningReadGuard(f.service); ASSERT_TRUE(guard);
        EXPECT_THROW(f.service->Stop(),std::logic_error);
        auto nested=ChainstateService::AcquireMempoolChainstateRead(f.service); ASSERT_TRUE(nested);
        EXPECT_TRUE(nested->ValidateBlockSelection({},102).result.accepted());
    }
    EXPECT_NO_THROW(f.service->Stop());
    EXPECT_FALSE(ChainstateService::AcquireMiningReadGuard(f.service));
    EXPECT_FALSE(ChainstateService::AcquireMempoolChainstateRead(f.service));
    f.CheckUnpublished();
}
TEST(OrchardParentDrain, FailedPreparationReleasesUseAndStackServiceRefuses) {
    OwnedSelectedParentFixture f;
    auto changed=f.blocks.front(); changed.vtx.front().vout.front().value=AmountUna::Una(1);
    ASSERT_EQ(f.db.putBlock(f.token,changed.GetHash(),changed),Status::Ok);
    EXPECT_FALSE(f.service.PrepareSelectedOrchardParent());
    ASSERT_EQ(f.db.putBlock(f.token,f.blocks.front().GetHash(),f.blocks.front()),Status::Ok);
    ChainstateService stack; stack.setOwnedChainDB(f.database);
    ShieldedStateStartupTestAccess::BoundaryState(stack,f.tip,*f.replay);
    EXPECT_FALSE(stack.PrepareSelectedOrchardParent());
    EXPECT_NO_THROW(f.service.Stop());
    EXPECT_FALSE(f.service.PrepareSelectedOrchardParent()); f.CheckUnpublished();
}
#else
TEST(OrchardParentDrain, BackendOffStopClosesGuardAdmission) {
    auto service=std::make_shared<ChainstateService>();
    {auto guard=ChainstateService::AcquireMiningReadGuard(service); ASSERT_TRUE(guard);}
    EXPECT_NO_THROW(service->Stop());
    EXPECT_FALSE(ChainstateService::AcquireMiningReadGuard(service));
    EXPECT_FALSE(ChainstateService::AcquireMempoolChainstateRead(service));
}
#endif
