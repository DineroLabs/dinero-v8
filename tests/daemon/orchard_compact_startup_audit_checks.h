#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace dinero {
struct CompactStartupAuditTestAccess {
    static auto Prepare(ChainstateService& service) {return service.PrepareCompactStartupAudit();}
    static bool Bind(ChainstateService& service,ChainstateService::PreparedCompactStartupAudit& audit) {
        std::lock_guard<AnnotatedRecursiveMutex> lock(service.activation_mutex_);
        return service.BindCompactStartupAuditUnderLock(audit);
    }
};
}
TEST(OrchardCompactStartupAudit, OrchardProofBeforePublication) {
    auto fixture_owner=std::make_shared<CompactStartupFixture>();
    auto& fixture=*fixture_owner;auto headers=CompactBindingSelector(fixture);
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    auto service=std::make_shared<ChainstateService>();service->setChainDB(&fixture.reopened);
    ASSERT_TRUE(service->Init(context));
    EXPECT_FALSE(CompactStartupAuditTestAccess::Prepare(*service));
    ASSERT_NO_THROW(service->setHeaderChainSelector(headers));
    // A borrowed database remains insufficient even after header binding.
    EXPECT_FALSE(CompactStartupAuditTestAccess::Prepare(*service));
    const std::weak_ptr<CompactStartupFixture> fixture_lifetime=fixture_owner;
    ASSERT_NO_THROW(service->setOwnedChainDB(std::shared_ptr<ChainDB>(fixture_owner,&fixture.reopened)));
    fixture_owner.reset();
    EXPECT_FALSE(fixture_lifetime.expired());
    auto audit=CompactStartupAuditTestAccess::Prepare(*service);ASSERT_TRUE(audit);
    EXPECT_TRUE(CompactStartupAuditTestAccess::Bind(*service,*audit));
    EXPECT_EQ(service->GetActiveTip(),nullptr);EXPECT_FALSE(service->IsStarted());
    EXPECT_THROW((void)ChainstateService::AcquireWalletIndexUse(service),std::exception);
    EXPECT_FALSE(ChainstateService::AcquireBlockIngressUse(service));
    // Serialized lifetime refusal, not a race/deadlock or removed-lock control.
    EXPECT_THROW(service->Stop(),std::logic_error);
    audit.reset();EXPECT_NO_THROW(service->Stop());
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    service.reset();EXPECT_TRUE(fixture_lifetime.expired());
}
TEST(OrchardCompactStartupAudit, ChangedBeforeImageRefusesAndRetries) {
    auto fixture_owner=std::make_shared<CompactStartupFixture>();
    auto& fixture=*fixture_owner;auto headers=CompactBindingSelector(fixture);
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    auto service=std::make_shared<ChainstateService>();service->setOwnedChainDB(std::shared_ptr<ChainDB>(fixture_owner,&fixture.reopened));
    ASSERT_TRUE(service->Init(context));ASSERT_NO_THROW(service->setHeaderChainSelector(headers));
    auto audit=CompactStartupAuditTestAccess::Prepare(*service);ASSERT_TRUE(audit);
    auto* tip=FindBlockIndex(fixture.second->Header().GetHash());ASSERT_NE(tip,nullptr);
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    struct RestoreNonce {CBlockIndex& tip;uint32_t nonce;~RestoreNonce(){tip.nonce=nonce;}} restore{*tip,tip->nonce};
    tip->nonce^=1;EXPECT_FALSE(CompactStartupAuditTestAccess::Bind(*service,*audit));
    tip->nonce=restore.nonce;ASSERT_TRUE(CompactStartupAuditTestAccess::Bind(*service,*audit));
    const auto validated=fixture.reopened.getValidatedTip();ASSERT_TRUE(validated.ok());
    ASSERT_EQ(fixture.reopened.setValidatedTip(fixture.f.token,fixture.first->Header().GetHash(),fixture.first->Height()),Status::Ok);
    EXPECT_FALSE(CompactStartupAuditTestAccess::Bind(*service,*audit));
    ASSERT_EQ(fixture.reopened.setValidatedTip(fixture.f.token,validated->hash,validated->height),Status::Ok);
    EXPECT_TRUE(CompactStartupAuditTestAccess::Bind(*service,*audit));
    EXPECT_EQ(service->GetActiveTip(),nullptr);EXPECT_FALSE(service->IsStarted());
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
TEST(OrchardCompactStartupAudit, BoundaryUsesFrozenRetirementWithoutPublication) {
    auto fixture_owner=std::make_shared<CompactStartupFixture>();
    auto& fixture=*fixture_owner;RestoreCompactAuditBoundary(fixture);auto headers=CompactBindingSelector(fixture);
    const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
    GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
    auto service=std::make_shared<ChainstateService>();service->setOwnedChainDB(std::shared_ptr<ChainDB>(fixture_owner,&fixture.reopened));
    ASSERT_TRUE(service->Init(context));ASSERT_NO_THROW(service->setHeaderChainSelector(headers));
    auto audit=CompactStartupAuditTestAccess::Prepare(*service);ASSERT_TRUE(audit);
    EXPECT_TRUE(CompactStartupAuditTestAccess::Bind(*service,*audit));
    EXPECT_FALSE(ChainstateService::AcquireBlockIngressUse(service));
    EXPECT_EQ(service->GetActiveTip(),nullptr);EXPECT_FALSE(service->IsStarted());
    EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
}
TEST(OrchardCompactStartupAudit, DeliveryOriginAndHeadRefuseWithoutPublication) {
    // Exercise both an active Orchard tip and a real rollback to its parent.
    // Data mutations are serialized; no concurrent writer or removed lock.
    for(const bool boundary:{false,true}) {
        auto fixture_owner=std::make_shared<CompactStartupFixture>();
        auto& fixture=*fixture_owner;
        if(boundary)RestoreCompactAuditBoundary(fixture);
        auto headers=CompactBindingSelector(fixture);
        const auto rows=fixture.Rows();const auto archives=fixture.ArchiveBytes();
        GetConfig().utreexo_stateless=true;DaemonContext context;ConfigureCompactServiceContext(context,fixture);
        auto service=std::make_shared<ChainstateService>();service->setOwnedChainDB(std::shared_ptr<ChainDB>(fixture_owner,&fixture.reopened));
        ASSERT_TRUE(service->Init(context));ASSERT_NO_THROW(service->setHeaderChainSelector(headers));
        auto audit=CompactStartupAuditTestAccess::Prepare(*service);ASSERT_TRUE(audit);
        for(const std::string key:{std::string("runtime_orchard_outbox:v1:head"),
                std::string("runtime_orchard_outbox:v1:event:0000000000000001")}) {
            std::string original;ASSERT_EQ(fixture.reopened.getRaw(key,original),Status::Ok);
            rocksdb::WriteBatch remove;remove.Delete(key);
            ASSERT_EQ(fixture.reopened.writeBatch(fixture.f.token,std::move(remove),true),Status::Ok);
            EXPECT_FALSE(CompactStartupAuditTestAccess::Bind(*service,*audit));
            rocksdb::WriteBatch malformed;malformed.Put(key,"invalid");
            ASSERT_EQ(fixture.reopened.writeBatch(fixture.f.token,std::move(malformed),true),Status::Ok);
            EXPECT_FALSE(CompactStartupAuditTestAccess::Bind(*service,*audit));
            rocksdb::WriteBatch restore;restore.Put(key,original);
            ASSERT_EQ(fixture.reopened.writeBatch(fixture.f.token,std::move(restore),true),Status::Ok);
            EXPECT_TRUE(CompactStartupAuditTestAccess::Bind(*service,*audit));
        }
        audit.reset();
        // A missing origin must also refuse the initial capture, before proof
        // completion or an active-tip/started/admission publication.
        const std::string origin="runtime_orchard_outbox:v1:event:0000000000000001";
        std::string saved;ASSERT_EQ(fixture.reopened.getRaw(origin,saved),Status::Ok);
        rocksdb::WriteBatch missing;missing.Delete(origin);
        ASSERT_EQ(fixture.reopened.writeBatch(fixture.f.token,std::move(missing),true),Status::Ok);
        EXPECT_FALSE(CompactStartupAuditTestAccess::Prepare(*service));
        rocksdb::WriteBatch restored;restored.Put(origin,saved);
        ASSERT_EQ(fixture.reopened.writeBatch(fixture.f.token,std::move(restored),true),Status::Ok);
        audit=CompactStartupAuditTestAccess::Prepare(*service);ASSERT_TRUE(audit);
        EXPECT_EQ(service->GetActiveTip(),nullptr);EXPECT_FALSE(service->IsStarted());
        EXPECT_FALSE(ChainstateService::AcquireBlockIngressUse(service));
        EXPECT_EQ(fixture.Rows(),rows);EXPECT_EQ(fixture.ArchiveBytes(),archives);
    }
}
#endif
