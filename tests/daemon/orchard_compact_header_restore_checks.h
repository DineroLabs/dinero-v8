#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
namespace {
struct CompactHeaderRestoreFixture {
    std::shared_ptr<CompactStartupFixture> storage=std::make_shared<CompactStartupFixture>();
    DaemonContext* previous=DaemonContext::instance();
    DaemonContext context;
    std::shared_ptr<ChainstateService> service=std::make_shared<ChainstateService>();
    std::shared_ptr<consensus::HeaderStore> store;
    CompactHeaderRestoreFixture() {
        GetConfig().utreexo_stateless=true;ConfigureCompactServiceContext(context,*storage);
        context.chainstate=service;DaemonContext::setInstance(&context);
        service->setOwnedChainDB(std::shared_ptr<ChainDB>(storage,&storage->reopened));
        OrchardAdmissionFixture::Require(service->Init(context));
        store=std::make_shared<consensus::HeaderStore>((storage->f.path/"restored-headers").string());
        OrchardAdmissionFixture::Require(store->Open());
    }
    ~CompactHeaderRestoreFixture() {
        if(service)service->Stop();context.chainstate.reset();service.reset();
        BlockAcceptor::SetContext(previous);DaemonContext::setInstance(previous);
    }
};
}
TEST(OrchardCompactHeaderRestore, ActualOwnerSeedsAndStartsWithoutHeaderWrites) {
    CompactHeaderRestoreFixture f;HeaderStore::StartupSnapshot before;
    ASSERT_TRUE(f.store->ReadStartupSnapshot(before));ASSERT_TRUE(before.headers.empty());
    auto headers=f.service->RestoreCompactHeaderSelector(f.store);ASSERT_TRUE(headers);
    HeaderIndexEntry copy;ASSERT_TRUE(headers->GetHeaderCopy(f.storage->second->Header().GetHash(),copy));
    EXPECT_EQ(copy.height,f.storage->second->Height());EXPECT_EQ(f.service->GetActiveTip(),nullptr);
    HeaderStore::StartupSnapshot after;ASSERT_TRUE(f.store->ReadStartupSnapshot(after));
    EXPECT_TRUE(after.headers.empty());EXPECT_EQ(after.best,before.best);
    std::weak_ptr<HeaderStore> weak=f.store;f.store.reset();EXPECT_FALSE(weak.expired());
    f.service->setRuntimeBlockNotifications(std::make_shared<CompactTransitionNotices>());
    ASSERT_TRUE(f.service->Start());EXPECT_EQ(f.service->GetActiveTip()->hash,f.storage->second->Header().GetHash());
    f.service->Stop();f.context.chainstate.reset();f.service.reset();
    EXPECT_FALSE(weak.expired());ASSERT_TRUE(headers->GetHeaderCopy(copy.hash,copy));
    headers.reset();EXPECT_TRUE(weak.expired());
}
TEST(OrchardCompactHeaderRestore, DurableSelectionChangeRefusesThenRetry) {
    CompactHeaderRestoreFixture f;const auto before=f.storage->reopened.getValidatedTip();ASSERT_TRUE(before.ok());
    ASSERT_EQ(f.storage->reopened.setValidatedTip(f.storage->f.token,f.storage->first->Header().GetHash(),f.storage->first->Height()),Status::Ok);
    EXPECT_THROW(f.service->RestoreCompactHeaderSelector(f.store),std::exception);
    EXPECT_EQ(f.service->GetActiveTip(),nullptr);EXPECT_FALSE(f.service->IsStarted());
    ASSERT_EQ(f.storage->reopened.setValidatedTip(f.storage->f.token,before->hash,before->height),Status::Ok);
    ASSERT_TRUE(f.service->RestoreCompactHeaderSelector(f.store));
}
TEST(OrchardCompactHeaderRestore, ExistingBindingCannotBeReplaced) {
    CompactHeaderRestoreFixture f;auto original=CompactBindingSelector(*f.storage);
    f.service->setHeaderChainSelector(original);
    EXPECT_THROW(f.service->RestoreCompactHeaderSelector(f.store),std::exception);
    EXPECT_NO_THROW(f.service->setHeaderChainSelector(original));
    EXPECT_FALSE(f.service->IsStarted());EXPECT_EQ(f.service->GetActiveTip(),nullptr);
}
#endif
