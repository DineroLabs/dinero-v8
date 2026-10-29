#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
namespace {
// Retain an ordinary child using its actual parent work and the existing header
// selector. This fixture does not certify validity or invoke ingress.
CBlockIndex* RetainOrdinaryExtension(ProofHandoffFixture& f) {
    auto selected=f.f.service->AcquireBlockIngressActivationLock();
    const auto& body=*f.second;const auto hash=body.Header().GetHash();
    const auto height=body.Height();
    const auto work=*f.f.db.getBlockWork(body.Header().prev_block_hash)+GetBlockProof(body.Header().difficulty);
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
    OrchardAdmissionFixture::Require(headers->AddHeader(f.first->Header()));
    OrchardAdmissionFixture::Require(headers->AddHeader(body.Header()));
    f.f.service->setHeaderChainSelector(headers);
    auto* index=dinero::AddBlockIndex(body.Header(),height);OrchardAdmissionFixture::Require(index!=nullptr);
    index->status=metadata.status_flags;index->file_number=metadata.file_number;
    index->data_pos=metadata.data_pos;index->data_size=metadata.data_size;
    f.f.service->AddCandidate(index);return index;
}
}
TEST(OrchardExtension, RootOwnerNestedIngressAndSuccessfulMtp) {
    ProofHandoffFixture f;const auto hex=util::hex(f.second->WireBytes());
    EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
    auto owner=ChainstateService::AcquireBlockIngressUse(f.f.service,&hex);ASSERT_TRUE(owner);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
    const auto accepted=BlockAcceptor::AcceptBlockFromRPC(hex,"extension-fixture");
    ASSERT_TRUE(accepted.connected)<<accepted.reason;
    EXPECT_EQ(accepted.height,103u);EXPECT_EQ(f.f.service->GetActiveTip()->hash,f.second->Header().GetHash());
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
    owner.reset();EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
    // The ordinary parent and same-block child both use real signatures and
    // successful absolute-time MTP. The exact stored body is retained.
    const auto stored=ReadRuntimeBlockUnderLock(f.f.db,f.files.get(),accepted.block_hash,103);
    ASSERT_TRUE(stored.ok());EXPECT_EQ(stored->Orchard().WireBytes(),f.second->WireBytes());
}
TEST(OrchardExtension, ChangedCapturedCoinAndWireRefuseWithoutEffects) {
    {
        ProofHandoffFixture f;const auto hex=util::hex(f.second->WireBytes());
        auto owner=ChainstateService::AcquireBlockIngressUse(f.f.service,&hex);ASSERT_TRUE(owner);
        const OutPoint point(f.first->Transactions().at(1).GetTxid(),0);
        const auto original=f.f.db.getCoin(point.txid.AsUint256(),point.vout);ASSERT_TRUE(original.ok());
        auto wrong=*original;++wrong.amount;
        ASSERT_EQ(f.f.db.putCoin(f.f.token,point.txid.AsUint256(),point.vout,wrong),Status::Ok);
        const auto before=ChainDBTransactionReadTestPeer::HandoffRows(f.f.db);
        const auto refused=BlockAcceptor::AcceptBlockFromRPC(hex,"extension-fixture");
        EXPECT_TRUE(refused.rejected());EXPECT_FALSE(refused.connected);
        EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(f.f.db),before);
        EXPECT_EQ(f.f.service->GetActiveTip()->hash,f.first->Header().GetHash());
        EXPECT_TRUE(f.f.service->IsInSafeMode());
        ASSERT_EQ(f.f.db.putCoin(f.f.token,point.txid.AsUint256(),point.vout,*original),Status::Ok);
        ASSERT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
        // Repairing one row must not silently clear the existing corruption
        // safeguard. A new root submission still refuses without side effects.
        owner.reset();const auto restored=ChainDBTransactionReadTestPeer::HandoffRows(f.f.db);
        const auto retry=BlockAcceptor::AcceptBlockFromRPC(hex,"extension-fixture");
        EXPECT_TRUE(retry.rejected());EXPECT_FALSE(retry.connected);
        EXPECT_TRUE(f.f.service->IsInSafeMode());
        EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(f.f.db),restored);
        EXPECT_EQ(f.f.service->GetActiveTip()->hash,f.first->Header().GetHash());
    }
    {
        // Independent healthy owner: a different submitted wire must not
        // consume the prepared packet; the captured exact wire remains usable.
        ProofHandoffFixture f;const auto hex=util::hex(f.second->WireBytes());
        auto owner=ChainstateService::AcquireBlockIngressUse(f.f.service,&hex);ASSERT_TRUE(owner);
        auto changed=f.second->WireBytes();auto header=f.second->Header();++header.timestamp;
        const auto prefix=header.SerializeForHash();std::copy(prefix.begin(),prefix.end(),changed.begin());
        const auto before=ChainDBTransactionReadTestPeer::HandoffRows(f.f.db);
        const auto other=BlockAcceptor::AcceptBlockFromRPC(util::hex(changed),"extension-fixture");
        EXPECT_TRUE(other.rejected());EXPECT_FALSE(other.connected);
        EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(f.f.db),before);
        EXPECT_FALSE(f.f.service->IsInSafeMode());
        const auto accepted=BlockAcceptor::AcceptBlockFromRPC(hex,"extension-fixture");
        ASSERT_TRUE(accepted.connected)<<accepted.reason;
        EXPECT_EQ(f.f.service->GetActiveTip()->hash,f.second->Header().GetHash());
    }
}
TEST(OrchardExtension, UnpreparedRecursiveOwnerRefusesThenRootRetry) {
    ProofHandoffFixture f;const auto hex=util::hex(f.second->WireBytes());
    const auto before=ChainDBTransactionReadTestPeer::HandoffRows(f.f.db);
    {
        auto caller=f.f.service->AcquireBlockIngressActivationLock();
        const auto refused=BlockAcceptor::AcceptBlockFromRPC(hex,"extension-fixture");
        EXPECT_TRUE(refused.rejected());EXPECT_FALSE(refused.connected);
        EXPECT_TRUE(caller.owns_lock());
        EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(f.f.db),before);
    }
    const auto accepted=BlockAcceptor::AcceptBlockFromRPC(hex,"extension-fixture");
    ASSERT_TRUE(accepted.connected)<<accepted.reason;
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
TEST(OrchardExtension, StoredChildExactPlanRebindAndMaintenanceRetry) {
    ProofHandoffFixture f;auto* next=RetainOrdinaryExtension(f);
    auto plan=ShieldedStateStartupTestAccess::CaptureActivationPlan(*f.f.service);
    const auto before=ChainDBTransactionReadTestPeer::HandoffRows(f.f.db);
    {
        auto caller=f.f.service->AcquireBlockIngressActivationLock();
        EXPECT_FALSE(ShieldedStateStartupTestAccess::CompleteActivationPlan(*f.f.service,plan));
        EXPECT_TRUE(caller.owns_lock());
    }
    EXPECT_EQ(ChainDBTransactionReadTestPeer::HandoffRows(f.f.db),before);
    ASSERT_TRUE(ShieldedStateStartupTestAccess::CompleteActivationPlan(*f.f.service,plan));
    // Serialized operator decisions invalidate the exact plan even after
    // restoring the candidate's original flags. No synchronization is changed.
    std::string error;
    ASSERT_TRUE(f.f.service->InvalidateBlock(next->hash,error))<<error;
    ASSERT_TRUE(f.f.service->ReconsiderBlock(next->hash,error))<<error;
    ShieldedStateStartupTestAccess::ApplyActivationPlan(*f.f.service,plan);
    EXPECT_EQ(f.f.service->GetActiveTip()->hash,f.first->Header().GetHash());
    EXPECT_EQ(next->status&(BLOCK_FAILED_VALID|BLOCK_FAILED_CHILD),0u);
    f.f.service->PumpReplayMetadataRecovery();
    EXPECT_EQ(f.f.service->GetActiveTip(),next);
    EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
}
TEST(OrchardExtension, ActualMiningSubmissionConsumesJobAfterCanonicalExtension) {
    CanonicalPoolFixture f;auto observer=std::make_shared<TypedForkNotices>();
    f.f.service->setRuntimeBlockNotifications(observer);
    ExecutionContext execution;execution.daemon=&f.context;
    din::Json request;request["address"]=OrchardMiningPayout;
    for(uint32_t height:{102u,103u}) {
        EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
        const auto job=::rpc_mining_getjob(execution,request);
        ASSERT_FALSE(job.isMember("error"))<<job["error"].asString();
        ASSERT_EQ(job["height"].asUInt(),height);
        auto header=OrchardRpcMiningFixture::Header(job);
        header.nonce=OrchardRpcMiningFixture::Nonce(job);
        din::Json solved;solved["job_id"]=job["job_id"];solved["nonce"]=header.nonce;
        const auto accepted=::rpc_mining_submit(execution,solved);
        ASSERT_TRUE(accepted.isNull())<<accepted["error"].asString();
        EXPECT_FALSE(ShieldedStateStartupTestAccess::VaultSelectedHeld(*f.f.service));
        EXPECT_EQ(f.f.service->GetActiveTip()->hash,header.GetHash());
        EXPECT_EQ(f.f.service->GetActiveTip()->height,height);
        EXPECT_TRUE(ShieldedStateStartupTestAccess::AuditBoundary(*f.f.service));
        ASSERT_EQ(observer->event_count,height-101u);
        EXPECT_EQ(observer->events[height-102u].height,height);
        EXPECT_EQ(observer->events[height-102u].hash,header.GetHash());
        EXPECT_EQ(::rpc_mining_submit(execution,solved)["code"].asString(),"stale-job");
    }
}
} // namespace dinero
#endif
