#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
struct CompactBoundaryAuditTestAccess {
    static bool Audit(ChainstateService& service,const CBlockIndex& tip) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(service.activation_mutex_);
        return service.AuditCompactBoundaryUnderLock(tip,nullptr);
    }
};
}
namespace {
void RestoreCompactAuditBoundary(CompactStartupFixture& fixture) {
    auto branch=CompleteCompactInverseBranch(fixture);auto owner=fixture.Restore();
    OrchardAdmissionFixture::Require(bool(owner));
    for(const auto& source:{fixture.second,fixture.first}) {
        const auto& seal=branch->ProvenBlock(source->Height(),source->Header().GetHash());
        auto write=PrepareCompactSealedInverse(fixture,*owner,source,seal);write->Commit();
    }
    OrchardAdmissionFixture::Require(fixture.reopened.getTip()->hash==fixture.parent->hash);
}
}
TEST(OrchardCompactBoundaryAudit, ReconstructedParentAuditsWithoutPublication) {
    CompactStartupFixture fixture;RestoreCompactAuditBoundary(fixture);
    auto headers=CompactBindingSelector(fixture);
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    auto service=std::make_shared<ChainstateService>();service->setChainDB(&fixture.reopened);
    ASSERT_TRUE(service->Init(context));
    auto* parent=FindBlockIndex(fixture.parent->hash);ASSERT_NE(parent,nullptr);
    EXPECT_FALSE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    ASSERT_NO_THROW(service->setHeaderChainSelector(headers));
    EXPECT_TRUE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    EXPECT_TRUE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    EXPECT_EQ(service->GetActiveTip(),nullptr);EXPECT_FALSE(service->IsStarted());
    EXPECT_THROW((void)ChainstateService::AcquireWalletIndexUse(service),std::exception);
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
TEST(OrchardCompactBoundaryAudit, ChangedIndexAndValidatedTipRefuseThenRetry) {
    CompactStartupFixture fixture;RestoreCompactAuditBoundary(fixture);
    auto headers=CompactBindingSelector(fixture);
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    auto service=std::make_shared<ChainstateService>();service->setChainDB(&fixture.reopened);
    ASSERT_TRUE(service->Init(context));ASSERT_NO_THROW(service->setHeaderChainSelector(headers));
    auto* parent=FindBlockIndex(fixture.parent->hash);ASSERT_NE(parent,nullptr);
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    struct RestoreNonce {CBlockIndex& value;uint32_t nonce;~RestoreNonce(){value.nonce=nonce;}} restore{*parent,parent->nonce};
    parent->nonce^=1;EXPECT_FALSE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    parent->nonce=restore.nonce;ASSERT_TRUE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    const auto validated=fixture.reopened.getValidatedTip();ASSERT_TRUE(validated.ok());
    ASSERT_EQ(fixture.reopened.setValidatedTip(fixture.f.token,fixture.first->Header().GetHash(),fixture.first->Height()),Status::Ok);
    EXPECT_FALSE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    ASSERT_EQ(fixture.reopened.setValidatedTip(fixture.f.token,validated->hash,validated->height),Status::Ok);
    EXPECT_TRUE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    const auto metadata=fixture.reopened.getHeaderMetadata(parent->hash);ASSERT_TRUE(metadata.ok());
    auto wrong=*metadata;ASSERT_LT(wrong.data_pos,UINT32_MAX);++wrong.data_pos;
    ASSERT_EQ(fixture.reopened.putHeaderMetadata(fixture.f.token,parent->hash,wrong),Status::Ok);
    EXPECT_FALSE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    ASSERT_EQ(fixture.reopened.putHeaderMetadata(fixture.f.token,parent->hash,*metadata),Status::Ok);
    EXPECT_TRUE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    const auto frontier=fixture.reopened.getShieldedState(ChainDB::ShieldedStateRecord::Frontier);
    ASSERT_TRUE(frontier.ok());
    ASSERT_EQ(fixture.reopened.putShieldedState(fixture.f.token,ChainDB::ShieldedStateRecord::Frontier,"invalid"),Status::Ok);
    EXPECT_FALSE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    ASSERT_EQ(fixture.reopened.putShieldedState(fixture.f.token,ChainDB::ShieldedStateRecord::Frontier,*frontier),Status::Ok);
    EXPECT_TRUE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    EXPECT_EQ(service->GetActiveTip(),nullptr);EXPECT_FALSE(service->IsStarted());
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
TEST(OrchardCompactBoundaryAudit, OrchardTipCannotUseBoundaryAudit) {
    CompactStartupFixture fixture;auto headers=CompactBindingSelector(fixture);
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    auto service=std::make_shared<ChainstateService>();service->setChainDB(&fixture.reopened);
    ASSERT_TRUE(service->Init(context));ASSERT_NO_THROW(service->setHeaderChainSelector(headers));
    auto* selected=FindBlockIndex(fixture.second->Header().GetHash());ASSERT_NE(selected,nullptr);
    EXPECT_FALSE(CompactBoundaryAuditTestAccess::Audit(*service,*selected));
    EXPECT_EQ(service->GetActiveTip(),nullptr);EXPECT_FALSE(service->IsStarted());
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
TEST(OrchardCompactBoundaryAudit, ProvenValidationStatusAndExactAvailability) {
    CompactStartupFixture fixture;RestoreCompactAuditBoundary(fixture);
    auto headers=CompactBindingSelector(fixture);
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    auto service=std::make_shared<ChainstateService>();service->setChainDB(&fixture.reopened);
    ASSERT_TRUE(service->Init(context));ASSERT_NO_THROW(service->setHeaderChainSelector(headers));
    auto* parent=FindBlockIndex(fixture.parent->hash);ASSERT_NE(parent,nullptr);
    const auto metadata=fixture.reopened.getHeaderMetadata(parent->hash);ASSERT_TRUE(metadata.ok());
    // This real historical fixture retains fewer status levels than replay
    // independently proves. The audit must not repair or rewrite its rows.
    ASSERT_NE(metadata->status_flags & BLOCK_VALID_MASK,uint32_t(BLOCK_VALID_MASK));
    ASSERT_EQ(parent->status,metadata->status_flags | uint32_t(BLOCK_VALID_MASK));
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    struct RestoreStatus {CBlockIndex& node;uint32_t status;~RestoreStatus(){node.status=status;}} restore{*parent,parent->status};
    ASSERT_TRUE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    for(const uint32_t bit:{uint32_t(BLOCK_VALID_HEADER),uint32_t(BLOCK_VALID_TREE),
            uint32_t(BLOCK_VALID_TRANSACTIONS),uint32_t(BLOCK_VALID_CHAIN),uint32_t(BLOCK_VALID_SCRIPTS)}) {
        parent->status=restore.status & ~bit;
        EXPECT_FALSE(CompactBoundaryAuditTestAccess::Audit(*service,*parent)) << "missing proven status bit " << bit;
        parent->status=restore.status;
        EXPECT_TRUE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    }
    for(const uint32_t bit:{uint32_t(BLOCK_HAVE_DATA),uint32_t(BLOCK_HAVE_UNDO),
            uint32_t(BLOCK_FAILED_VALID),uint32_t(BLOCK_FAILED_CHILD)}) {
        parent->status=restore.status ^ bit;
        EXPECT_FALSE(CompactBoundaryAuditTestAccess::Audit(*service,*parent)) << "changed availability/failure bit " << bit;
        parent->status=restore.status;
        EXPECT_TRUE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    }
    auto rejected=*metadata;rejected.status_flags|=BLOCK_FAILED_VALID;
    ASSERT_EQ(fixture.reopened.putHeaderMetadata(fixture.f.token,parent->hash,rejected),Status::Ok);
    EXPECT_FALSE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    ASSERT_EQ(fixture.reopened.putHeaderMetadata(fixture.f.token,parent->hash,*metadata),Status::Ok);
    EXPECT_TRUE(CompactBoundaryAuditTestAccess::Audit(*service,*parent));
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    EXPECT_EQ(service->GetActiveTip(),nullptr);EXPECT_FALSE(service->IsStarted());
}
#endif
