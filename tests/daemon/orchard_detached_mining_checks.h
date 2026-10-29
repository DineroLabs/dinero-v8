#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
struct OrchardDetachedMiningTestAccess {
    using Packet=std::shared_ptr<ChainstateService::PreparedOrchardMining>;
    static Packet Capture(ChainstateService& service,const OrchardMiningTemplate& expected) {
        auto selected=service.AcquireBlockIngressActivationLock();
        auto coinbase=expected.Transactions().front().Historical();
        std::erase_if(coinbase.vout,[](const auto& out){return !out.scriptPubKey.empty() && out.scriptPubKey.front()==0x6a;});
        const auto& bodies=expected.Transactions();
        return service.CaptureOrchardMiningUnderLock(expected.Header(),coinbase,
            std::span<const MempoolTransaction>(bodies.data()+1,bodies.size()-1),expected.Height());
    }
    static void Authorize(ChainstateService& service,const Packet& packet){service.AuthorizeOrchardMiningDetached(*packet);}
    static void Commit(ChainstateService& service,const Packet& packet) {
        auto selected=service.AcquireBlockIngressActivationLock();service.CommitOrchardMiningUnderLock(*packet);
    }
    static void Complete(ChainstateService& service,const Packet& packet){service.CompleteOrchardMiningDetached(*packet);}
    static auto Bind(ChainstateService& service,const Packet& packet) {
        auto selected=service.AcquireBlockIngressActivationLock();return service.BindOrchardMiningUnderLock(*packet);
    }
};
TEST(OrchardDetachedMining, DetachedPhasesPreserveExactTemplateAndRefuseHeldProofWork) {
    ProofHandoffFixture f;using Access=OrchardDetachedMiningTestAccess;
    const auto before=f.Rows();auto packet=Access::Capture(*f.f.service,*f.second);ASSERT_TRUE(packet);
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        EXPECT_THROW(Access::Authorize(*f.f.service,packet),std::runtime_error);
        EXPECT_TRUE(selected.owns_lock());
    }
    EXPECT_THROW(Access::Bind(*f.f.service,packet),std::runtime_error);
    ASSERT_NO_THROW(Access::Authorize(*f.f.service,packet));
    EXPECT_THROW(Access::Complete(*f.f.service,packet),std::runtime_error);
    ASSERT_NO_THROW(Access::Commit(*f.f.service,packet));
    {
        auto selected=f.f.service->AcquireBlockIngressActivationLock();
        EXPECT_THROW(Access::Complete(*f.f.service,packet),std::runtime_error);
    }
    ASSERT_NO_THROW(Access::Complete(*f.f.service,packet));
    const auto result=Access::Bind(*f.f.service,packet);ASSERT_TRUE(result);
    EXPECT_EQ(result->WireBytes(),f.second->WireBytes());EXPECT_EQ(result->TotalFees(),f.second->TotalFees());
    EXPECT_EQ(f.Rows(),before);EXPECT_THROW(Access::Bind(*f.f.service,packet),std::runtime_error);
    EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
}
TEST(OrchardDetachedMining, ChangedParentCoinRefusesBeforeCommitAndAfterProofThenRetries) {
    ProofHandoffFixture f;using Access=OrchardDetachedMiningTestAccess;
    auto packet=Access::Capture(*f.f.service,*f.second);ASSERT_TRUE(packet);
    ASSERT_NO_THROW(Access::Authorize(*f.f.service,packet));
    const OutPoint point(f.first->Transactions().at(1).GetTxid(),0);
    const auto original=f.f.db.getCoin(point.txid.AsUint256(),point.vout);ASSERT_TRUE(original.ok());
    const auto corrupt=[&]{auto wrong=*original;++wrong.amount;ASSERT_EQ(f.f.db.putCoin(f.f.token,point.txid.AsUint256(),point.vout,wrong),Status::Ok);};
    const auto restore=[&]{ASSERT_EQ(f.f.db.putCoin(f.f.token,point.txid.AsUint256(),point.vout,*original),Status::Ok);};
    corrupt();auto before=f.Rows();EXPECT_THROW(Access::Commit(*f.f.service,packet),consensus::OrchardStateLookupError);EXPECT_EQ(f.Rows(),before);
    restore();
    ASSERT_NO_THROW(Access::Commit(*f.f.service,packet));
    ASSERT_NO_THROW(Access::Complete(*f.f.service,packet));
    corrupt();before=f.Rows();EXPECT_THROW(Access::Bind(*f.f.service,packet),consensus::OrchardStateLookupError);EXPECT_EQ(f.Rows(),before);
    restore();const auto result=Access::Bind(*f.f.service,packet);ASSERT_TRUE(result);EXPECT_EQ(result->WireBytes(),f.second->WireBytes());
}
TEST(OrchardDetachedMining, FinalCoinbaseCollisionAndChangedProfileRefusePublication) {
    ProofHandoffFixture f;using Access=OrchardDetachedMiningTestAccess;
    auto packet=Access::Capture(*f.f.service,*f.second);ASSERT_TRUE(packet);
    ASSERT_NO_THROW(Access::Authorize(*f.f.service,packet));
    ASSERT_NO_THROW(Access::Commit(*f.f.service,packet));
    ASSERT_NO_THROW(Access::Complete(*f.f.service,packet));
    const auto branch=MutableParams().orchard_branch_id;MutableParams().orchard_branch_id^=1;
    EXPECT_THROW(Access::Bind(*f.f.service,packet),consensus::OrchardStateLookupError);MutableParams().orchard_branch_id=branch;
    const auto id=f.second->Transactions().front().GetTxid();
    const auto original=f.f.db.getCoin(id.AsUint256(),0);ASSERT_EQ(original.status(),Status::NotFound);
    const auto source=f.f.db.getCoin(f.first->Transactions().at(1).GetTxid().AsUint256(),0);ASSERT_TRUE(source.ok());
    ASSERT_EQ(f.f.db.putCoin(f.f.token,id.AsUint256(),0,*source),Status::Ok);
    const auto before=f.Rows();EXPECT_THROW(Access::Bind(*f.f.service,packet),consensus::OrchardStateLookupError);EXPECT_EQ(f.Rows(),before);
}
TEST(OrchardDetachedMining, BorrowedRootBuildAndNestedRefusalRetainSelectedOwner) {
    OrchardAdmissionFixture f;BlockAssembler assembler(&f.db);WireOrchardAssembler(assembler,f);
    auto root=ChainstateService::AcquireMiningReadGuard(f.service);ASSERT_TRUE(root);
    const auto built=assembler.CreateOrchardBlock(OrchardMiningPayout,*root);ASSERT_TRUE(built);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.service));
    {
        auto nested=ChainstateService::AcquireMiningReadGuard(f.service);ASSERT_TRUE(nested);
        auto payout=built->Transactions().front().Historical();
        std::erase_if(payout.vout,[](const auto& out){return !out.scriptPubKey.empty() && out.scriptPubKey.front()==0x6a;});
        EXPECT_FALSE(nested->BuildOrchardTemplate(built->Header(),payout,{},built->Height()));
        EXPECT_TRUE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.service));
    }
    // Real detached validation failure must reacquire the borrowed root owner.
    auto payout=built->Transactions().front().Historical();
    std::erase_if(payout.vout,[](const auto& out){return !out.scriptPubKey.empty() && out.scriptPubKey.front()==0x6a;});
    payout.vout.front().value=AmountUna::Una(orchard::kMaxMoneyUna);
    EXPECT_ANY_THROW(root->BuildOrchardTemplate(built->Header(),payout,{},built->Height()));
    EXPECT_TRUE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.service));
    EXPECT_TRUE(assembler.CreateOrchardBlock(OrchardMiningPayout,*root));
    root.reset();EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.service));f.CheckUnpublished();
}
} // namespace dinero
#else
TEST(OrchardDetachedMining, BackendOffDefaultRefuses) {
    dinero::MiningChainstateReadGuard owner;EXPECT_FALSE(owner.BuildOrchardTemplate({}, {}, {}, 1));
}
#endif
