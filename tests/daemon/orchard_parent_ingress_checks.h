#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(OrchardParentIngress, ActualRouterBorrowsPreparedHistoryThroughCanonicalCommit) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    {
        const auto submitted=util::hex(built->WireBytes());
        auto ingress=ChainstateService::AcquireBlockIngressUse(f.f.service,&submitted);ASSERT_TRUE(ingress);
        auto changed=f.f.blocks.front();changed.vtx.front().vout.front().value=AmountUna::Una(1);
        ASSERT_EQ(f.f.db.putBlock(f.f.token,changed.GetHash(),changed),Status::Ok);
        // Sequential replacement after capture. Nested router, typed handler
        // and canonical writer must consume the same valid immutable history.
        const auto result=f.Submit(built->WireBytes());ASSERT_TRUE(result.accepted())<<result.reason;
        EXPECT_TRUE(result.connected);EXPECT_EQ(result.height,102u);
        EXPECT_EQ(f.notices->published,1u);EXPECT_EQ(f.f.ingress->mempool().size(),0u);
        ASSERT_EQ(f.f.db.putBlock(f.f.token,f.f.blocks.front().GetHash(),f.f.blocks.front()),Status::Ok);
    }
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    ASSERT_TRUE(f.f.db.getLegacyRetirementState().ok());
    EXPECT_EQ(f.f.db.getLegacyRetirementState()->record.boundary_parent,f.parent->hash);
}
TEST(OrchardParentIngress, UnpreparedRecursiveAndStoppedOwnersRefuseBeforePersistence) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        const auto refused=f.f.service->TryAcceptOrchardBlockFromRPC(util::hex(built->WireBytes()));
        ASSERT_TRUE(refused);EXPECT_FALSE(refused->accepted());
        EXPECT_EQ(f.f.db.getHeader(built->Header().GetHash()).status(),Status::NotFound);
        f.f.CheckUnpublished();EXPECT_EQ(f.notices->published,0u);
    }
    const auto retry=f.Submit(built->WireBytes());ASSERT_TRUE(retry.accepted())<<retry.reason;
    EXPECT_EQ(f.notices->published,1u);
    EXPECT_NO_THROW(f.f.service->Stop());
    EXPECT_FALSE(ChainstateService::AcquireBlockIngressUse(f.f.service));
    EXPECT_FALSE(f.Submit(built->WireBytes()).accepted());
    EXPECT_EQ(f.notices->published,1u);
}
TEST(OrchardParentIngress, DirectTypedHandlerPreparesAndCommitsExactBody) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    const auto result=f.f.service->TryAcceptOrchardBlockFromRPC(util::hex(built->WireBytes()));
    ASSERT_TRUE(result);ASSERT_TRUE(result->accepted())<<result->reason;
    EXPECT_EQ(result->block_hash,built->Header().GetHash());EXPECT_EQ(f.notices->published,1u);
    const auto stored=ReadRuntimeBlockUnderLock(f.f.db,f.files.get(),result->block_hash,102);
    ASSERT_TRUE(stored.ok());ASSERT_TRUE(stored->IsOrchardProfile());
    EXPECT_EQ(stored->Orchard().WireBytes(),built->WireBytes());
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
#else
TEST(OrchardParentIngress, InactiveStackServiceKeepsHistoricalRouting) {
    ChainstateService service;EXPECT_FALSE(service.TryAcceptOrchardBlockFromRPC(""));
}
#endif
