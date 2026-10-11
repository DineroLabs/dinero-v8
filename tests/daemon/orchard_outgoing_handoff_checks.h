#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
struct OrchardOutgoingHandoffTestAccess {
    static auto Capture(ChainstateService& service,const std::vector<CBlockIndex*>& path,CBlockIndex* fork) {
        auto lock=service.AcquireBlockIngressActivationLock();
        return service.CaptureOrchardOutgoingUnderLock(path,fork);
    }
    static bool Complete(ChainstateService& service,ChainstateService::PreparedOrchardOutgoing& owner) {
        return service.CompleteOrchardOutgoing(owner);
    }
    static bool Bind(ChainstateService& service,const ChainstateService::PreparedOrchardOutgoing& owner,
                     const std::vector<CBlockIndex*>& path,CBlockIndex* fork) {
        auto lock=service.AcquireBlockIngressActivationLock();
        return service.BindOrchardOutgoingUnderLock(owner,path,fork);
    }
};
TEST(OrchardOutgoingHandoff, BothSelectedBlocksRequireCompleteReplay) {
    bool executed=false;
    CompactStartupFixture fixture([&](auto& f) {
        auto& service=*f.f.service;auto* tip=service.GetActiveTip();
        ASSERT_EQ(tip->height,103u);std::vector<CBlockIndex*> path{tip,tip->pprev};
        const auto state=f.f.db.getOrchardState();ASSERT_TRUE(state.ok());
        const auto rows=f.f.db.getTip();ASSERT_TRUE(rows.ok());
        auto owner=OrchardOutgoingHandoffTestAccess::Capture(service,path,f.parent);ASSERT_TRUE(owner);
        EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(service,*owner,path,f.parent));
        {
            auto lock=service.AcquireBlockIngressActivationLock();
            EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Complete(service,*owner));
        }
        ASSERT_TRUE(OrchardOutgoingHandoffTestAccess::Complete(service,*owner));
        EXPECT_TRUE(OrchardOutgoingHandoffTestAccess::Bind(service,*owner,path,f.parent));
        EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Complete(service,*owner));
        // A rejected repeated completion cannot discard an already valid owner.
        EXPECT_TRUE(OrchardOutgoingHandoffTestAccess::Bind(service,*owner,path,f.parent));
        EXPECT_EQ(service.GetActiveTip(),tip);EXPECT_EQ(*f.f.db.getOrchardState(),*state);
        EXPECT_EQ(f.f.db.getTip()->hash,rows->hash);executed=true;
    });
    EXPECT_TRUE(executed);
}
TEST(OrchardOutgoingHandoff, IncompleteReorderedAndForeignPathsRefuse) {
    bool executed=false;
    CompactStartupFixture fixture([&](auto& f) {
        auto& service=*f.f.service;auto* tip=service.GetActiveTip();
        const std::vector<CBlockIndex*> path{tip,tip->pprev};
        EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Capture(service,{},f.parent));
        EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Capture(service,{tip},f.parent));
        EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Capture(service,{tip,tip},f.parent));
        EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Capture(service,{tip->pprev,tip},f.parent));
        auto owner=OrchardOutgoingHandoffTestAccess::Capture(service,path,f.parent);ASSERT_TRUE(owner);
        ASSERT_TRUE(OrchardOutgoingHandoffTestAccess::Complete(service,*owner));
        EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(service,*owner,{tip},f.parent));
        EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(service,*owner,path,tip->pprev));
        auto foreign=std::make_shared<ChainstateService>();
        EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(*foreign,*owner,path,f.parent));
        EXPECT_TRUE(OrchardOutgoingHandoffTestAccess::Bind(service,*owner,path,f.parent));executed=true;
    });
    EXPECT_TRUE(executed);
}
TEST(OrchardOutgoingHandoff, ChangedIndexAndDurableSelectedTipRefuse) {
    bool executed=false;
    CompactStartupFixture fixture([&](auto& f) {
        auto& service=*f.f.service;auto* tip=service.GetActiveTip();
        std::vector<CBlockIndex*> path{tip,tip->pprev};
        auto owner=OrchardOutgoingHandoffTestAccess::Capture(service,path,f.parent);ASSERT_TRUE(owner);
        ASSERT_TRUE(OrchardOutgoingHandoffTestAccess::Complete(service,*owner));
        const auto nonce=tip->pprev->nonce;tip->pprev->nonce^=1;
        EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(service,*owner,path,f.parent));
        tip->pprev->nonce=nonce;
        EXPECT_TRUE(OrchardOutgoingHandoffTestAccess::Bind(service,*owner,path,f.parent));
        const auto validated=f.f.db.getValidatedTip();ASSERT_TRUE(validated.ok());
        ASSERT_EQ(f.f.db.setValidatedTip(f.f.token,tip->pprev->hash,tip->pprev->height),Status::Ok);
        EXPECT_FALSE(OrchardOutgoingHandoffTestAccess::Bind(service,*owner,path,f.parent));
        ASSERT_EQ(f.f.db.setValidatedTip(f.f.token,validated->hash,validated->height),Status::Ok);
        EXPECT_TRUE(OrchardOutgoingHandoffTestAccess::Bind(service,*owner,path,f.parent));executed=true;
    });
    EXPECT_TRUE(executed);
}
} // namespace dinero
#endif
