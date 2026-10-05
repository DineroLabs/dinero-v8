#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
// Persist an unvalidated candidate exactly as a retained body, without calling
// ingress or claiming validity. The real ABC/ConnectTip pipeline must validate
// and commit it. This isolates the root activation entry from ingress guards.
CBlockIndex* RetainActivationCandidate(CanonicalPoolFixture& f,const OrchardMiningTemplate& body) {
    const auto hash=body.Header().GetHash();const auto height=body.Height();
    const auto work=*f.f.db.getBlockWork(f.parent->hash)+GetBlockProof(body.Header().difficulty);
    const auto position=f.files->writeBlockBytes(hash,{body.WireBytes().begin(),body.WireBytes().end()});
    OrchardAdmissionFixture::Require(position.ok());
    ChainDB::PersistedHeaderMetadata metadata;
    metadata.height=height;metadata.parent_hash=body.Header().prev_block_hash;metadata.chainwork=work;
    metadata.status_flags=BLOCK_VALID_HEADER|BLOCK_HAVE_DATA;
    metadata.file_number=position->file_number;metadata.data_pos=position->offset;metadata.data_size=position->size;
    OrchardAdmissionFixture::Require(f.f.db.putHeader(f.f.token,hash,body.Header(),height,work)==Status::Ok);
    OrchardAdmissionFixture::Require(f.f.db.putHeaderMetadata(f.f.token,hash,metadata)==Status::Ok);
    auto headers=std::make_shared<consensus::HeaderChainSelector>();
    for(const auto& b:f.f.blocks)OrchardAdmissionFixture::Require(headers->AddHeader(b.header));
    OrchardAdmissionFixture::Require(headers->AddHeader(body.Header()));f.f.service->setHeaderChainSelector(headers);
    auto* index=dinero::AddBlockIndex(body.Header(),height);OrchardAdmissionFixture::Require(index!=nullptr);
    index->status=metadata.status_flags;index->file_number=metadata.file_number;
    index->data_pos=metadata.data_pos;index->data_size=metadata.data_size;
    f.f.service->AddCandidate(index);return index;
}
}
TEST(OrchardParentActivation, RootPassPreparesAndCommitsRetainedCandidate) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    auto* next=RetainActivationCandidate(f,*built);ASSERT_NE(next,f.parent);
    f.f.service->ActivateBestChain();
    EXPECT_EQ(f.f.service->GetActiveTip(),next);EXPECT_EQ(f.notices->published,1u);
    ASSERT_TRUE(f.f.db.getLegacyRetirementState().ok());
    EXPECT_EQ(f.f.db.getLegacyRetirementState()->record.boundary_parent,f.parent->hash);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    EXPECT_EQ(f.f.ingress->mempool().size(),0u);
}
TEST(OrchardParentActivation, InvalidHistoricalBodyRefusesBeforeEffectsThenRetries) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    auto* next=RetainActivationCandidate(f,*built);
    auto changed=f.f.blocks.front();changed.vtx.front().vout.front().value=AmountUna::Una(1);
    ASSERT_EQ(f.f.db.putBlock(f.f.token,changed.GetHash(),changed),Status::Ok);
    f.f.service->ActivateBestChain();
    EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);EXPECT_EQ(f.notices->published,0u);
    f.f.CheckUnpublished();EXPECT_EQ(f.f.ingress->mempool().size(),1u);
    EXPECT_EQ(next->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    ASSERT_EQ(f.f.db.putBlock(f.f.token,f.f.blocks.front().GetHash(),f.f.blocks.front()),Status::Ok);
    f.f.service->ActivateBestChain();
    EXPECT_EQ(f.f.service->GetActiveTip(),next);EXPECT_EQ(f.notices->published,1u);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
TEST(OrchardParentActivation, RecursiveUnpreparedPassDefersWithoutDroppingCallerLock) {
    CanonicalPoolFixture f;const auto built=f.Build();ASSERT_TRUE(built);
    auto* next=RetainActivationCandidate(f,*built);
    {
        auto caller=f.f.service->AcquireBlockIngressActivationLock();
        f.f.service->ActivateBestChain();
        EXPECT_TRUE(caller.owns_lock());EXPECT_EQ(f.f.service->GetActiveTip(),f.parent);
        EXPECT_EQ(f.notices->published,0u);f.f.CheckUnpublished();
    }
    f.f.service->ActivateBestChain();
    EXPECT_EQ(f.f.service->GetActiveTip(),next);EXPECT_EQ(f.notices->published,1u);
}
#endif
