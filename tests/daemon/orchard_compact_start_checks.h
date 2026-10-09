#pragma once
#ifdef DINERO_TEST_ORCHARD_ORIGIN
#include "dinero/daemon/block_acceptor.h"
namespace {
struct CompactServiceStartFixture {
    std::shared_ptr<CompactStartupFixture> storage=std::make_shared<CompactStartupFixture>();
    DaemonContext* previous=DaemonContext::instance();
    DaemonContext context;
    std::shared_ptr<ChainstateService> service;
    std::shared_ptr<CompactTransitionNotices> notices=std::make_shared<CompactTransitionNotices>();
    explicit CompactServiceStartFixture(bool boundary=false) {
        if(boundary)RestoreCompactAuditBoundary(*storage);
        Open();
    }
    void Open() {
        GetConfig().utreexo_stateless=true;ConfigureCompactServiceContext(context,*storage);
        service=std::make_shared<ChainstateService>();context.chainstate=service;
        DaemonContext::setInstance(&context);
        service->setOwnedChainDB(std::shared_ptr<ChainDB>(storage,&storage->reopened));
        OrchardAdmissionFixture::Require(service->Init(context));
        service->setHeaderChainSelector(CompactBindingSelector(*storage));
        service->setRuntimeBlockNotifications(notices);
    }
    std::filesystem::path WalletPath()const{return storage->f.path/"flatfiles"/"blockchain"/"utxo";}
    void Closed()const {
        EXPECT_FALSE(service->IsStarted());EXPECT_EQ(service->GetActiveTip(),nullptr);
        EXPECT_EQ(service->utxoIndex(),nullptr);EXPECT_EQ(service->GetConsensusUTXOSet(),nullptr);
        EXPECT_THROW((void)ChainstateService::AcquireWalletIndexUse(service),std::exception);
        EXPECT_FALSE(ChainstateService::AcquireBlockIngressUse(service));
    }
    void Close(){service->Stop();context.chainstate.reset();service.reset();BlockAcceptor::SetContext(previous);}
    ~CompactServiceStartFixture(){if(service)Close();DaemonContext::setInstance(previous);}
};
}
TEST(OrchardCompactStart, ActualStartAndReopenKeepCompactAuthority) {
    CompactServiceStartFixture f;const auto rows=f.storage->Rows();const auto archives=f.storage->ArchiveBytes();
    f.Closed();ASSERT_TRUE(f.service->Start());ASSERT_TRUE(f.service->IsStarted());
    EXPECT_TRUE(f.service->IsHealthy());EXPECT_EQ(f.service->GetConsensusUTXOSet(),nullptr);
    EXPECT_EQ(f.service->GetActiveTip()->hash,f.storage->second->Header().GetHash());
    ASSERT_NE(f.service->utxoIndex(),nullptr);
    {auto wallet=ChainstateService::AcquireWalletIndexUse(f.service);ASSERT_TRUE(wallet);}
    {auto ingress=ChainstateService::AcquireBlockIngressUse(f.service);ASSERT_TRUE(ingress);}
    EXPECT_FALSE(f.service->Start());EXPECT_TRUE(f.service->IsStarted());
    EXPECT_EQ(f.storage->Rows(),rows);EXPECT_EQ(f.storage->ArchiveBytes(),archives);
    f.Close();f.storage->reopened.close();ASSERT_EQ(f.storage->reopened.init(f.storage->f.path),Status::Ok);
    f.Open();f.Closed();ASSERT_TRUE(f.service->Start());
    EXPECT_EQ(f.storage->Rows(),rows);EXPECT_EQ(f.storage->ArchiveBytes(),archives);
}
TEST(OrchardCompactStart, MissingCompositionAndWrongValidationPolicyRefuseThenRetry) {
    CompactServiceStartFixture f;
    f.service->setRuntimeBlockNotifications(nullptr);EXPECT_FALSE(f.service->Start());f.Closed();
    EXPECT_FALSE(std::filesystem::exists(f.WalletPath()));
    f.service->setRuntimeBlockNotifications(f.notices);
    DaemonContext::setInstance(f.previous);EXPECT_FALSE(f.service->Start());f.Closed();
    DaemonContext::setInstance(&f.context);
    f.service->setValidationMode(consensus::ValidationMode::STATEFUL);EXPECT_FALSE(f.service->Start());f.Closed();
    f.service->setValidationMode(consensus::ValidationMode::STATELESS);ASSERT_TRUE(f.service->Start());
}
TEST(OrchardCompactStart, InvalidDurableTipRefusesBeforeWalletOpen) {
    CompactServiceStartFixture f;const auto before=f.storage->reopened.getValidatedTip();ASSERT_TRUE(before.ok());
    ASSERT_EQ(f.storage->reopened.setValidatedTip(f.storage->f.token,f.storage->first->Header().GetHash(),f.storage->first->Height()),Status::Ok);
    EXPECT_FALSE(f.service->Start());f.Closed();EXPECT_FALSE(std::filesystem::exists(f.WalletPath()));
    ASSERT_EQ(f.storage->reopened.setValidatedTip(f.storage->f.token,before->hash,before->height),Status::Ok);
    ASSERT_TRUE(f.service->Start());
}
TEST(OrchardCompactStart, WalletOpenFailureDoesNotPublish) {
    CompactServiceStartFixture f;
    ASSERT_TRUE(std::filesystem::create_directories(f.WalletPath().parent_path()));
    ASSERT_TRUE(std::filesystem::create_directory(f.WalletPath()));
    const auto rows=f.storage->Rows();const auto archives=f.storage->ArchiveBytes();
    EXPECT_FALSE(f.service->Start());f.Closed();
    EXPECT_EQ(f.storage->Rows(),rows);EXPECT_EQ(f.storage->ArchiveBytes(),archives);
    ASSERT_TRUE(std::filesystem::remove(f.WalletPath()));ASSERT_TRUE(f.service->Start());
}
TEST(OrchardCompactStart, LegacyRecoveryMarkersRemainIntactAndRefuse) {
    CompactServiceStartFixture f;std::filesystem::create_directories(f.WalletPath().parent_path());
    const auto rows=f.storage->Rows();const auto archives=f.storage->ArchiveBytes();
    for(const std::string key:{"reorg_in_progress","incomplete_reorg_recovery_tip","wallet_snapshot_recovery_base_height","assumeutxo_active","assumeutxo_lifecycle_state"}) {
        {dinero::UTXOIndex index(f.WalletPath().string());ASSERT_TRUE(index.Initialize());ASSERT_TRUE(index.SetMetadata(key,"preserve"));}
        EXPECT_FALSE(f.service->Start());f.Closed();
        {dinero::UTXOIndex index(f.WalletPath().string());ASSERT_TRUE(index.Initialize());EXPECT_EQ(index.GetMetadata(key),"preserve");ASSERT_TRUE(index.DeleteMetadata(key));}
    }
    EXPECT_EQ(f.storage->Rows(),rows);EXPECT_EQ(f.storage->ArchiveBytes(),archives);
    ASSERT_TRUE(f.service->Start());
}
TEST(OrchardCompactStart, ActivationBoundaryStartsWithoutFullCoins) {
    CompactServiceStartFixture f(true);const auto rows=f.storage->Rows();const auto archives=f.storage->ArchiveBytes();
    ASSERT_TRUE(f.service->Start());EXPECT_EQ(f.service->GetActiveTip()->hash,f.storage->parent->hash);
    EXPECT_EQ(f.service->GetConsensusUTXOSet(),nullptr);EXPECT_TRUE(f.service->IsStarted());
    EXPECT_EQ(f.storage->Rows(),rows);EXPECT_EQ(f.storage->ArchiveBytes(),archives);
}
TEST(OrchardCompactStart, MetadataAbsenceChecksSqlErrorsAndBorrowedTransaction) {
    CompactServiceStartFixture f;std::filesystem::create_directories(f.WalletPath().parent_path());
    dinero::UTXOIndex index(f.WalletPath().string());ASSERT_TRUE(index.Initialize());
    ASSERT_TRUE(index.SetMetadata("unrelated","preserve"));
    EXPECT_NO_THROW(index.RequireMetadataAbsent({"reorg_in_progress","assumeutxo_active"}));
    ASSERT_TRUE(index.BeginTransaction());
    EXPECT_THROW(index.RequireMetadataAbsent({"reorg_in_progress"}),std::exception);
    ASSERT_TRUE(index.SetMetadata("borrowed","retain transaction"));
    ASSERT_TRUE(index.RollbackTransaction());EXPECT_FALSE(index.GetMetadata("borrowed"));
    EXPECT_EQ(index.GetMetadata("unrelated"),"preserve");
    sqlite3* raw=nullptr;ASSERT_EQ(sqlite3_open(f.WalletPath().c_str(),&raw),SQLITE_OK);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> connection(raw,sqlite3_close);
    ASSERT_EQ(sqlite3_exec(raw,"DROP TABLE utxo_metadata",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_THROW(index.RequireMetadataAbsent({"reorg_in_progress"}),std::exception);
    f.Closed();
}
#endif
