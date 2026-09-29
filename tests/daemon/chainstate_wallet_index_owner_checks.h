#pragma once
#include "daemon/services/logger_service.h"
namespace dinero {
namespace {
struct WalletIndexOwnerFixture {
    ChainParams params=Params();
    DaemonContext* previous=DaemonContext::instance();
    ServiceOwnerLogger wallet_logger;
    DaemonContext context;
    std::filesystem::path root;
    ChainDB db;
    std::shared_ptr<ChainstateService> source;
    WalletIndexOwnerFixture() {
        SelectParams(Chain::REGTEST);
        char path[]="/tmp/dinero-wallet-index-owner-XXXXXX";
        if(!mkdtemp(path))throw std::runtime_error("fixture directory");root=path;
        if(db.init((root/"chain").string())!=Status::Ok)throw std::runtime_error("fixture database");
        auto config=std::make_shared<ConfigService>();config->Set("datadir",root.string());
        auto logger=std::make_shared<LoggerService>("");
        context.config=config;context.logger=logger;context.logger_interface=&wallet_logger;
        source=std::make_shared<ChainstateService>();context.chainstate=source;
        DaemonContext::setInstance(&context);source->setChainDB(&db);
        if(!source->Init(context))throw std::runtime_error("fixture actual chainstate Init");
    }
    ~WalletIndexOwnerFixture() {
        if(source)source->Stop();context.chainstate.reset();source.reset();
        BlockAcceptor::SetContext(previous);DaemonContext::setInstance(previous);
        db.close();MutableParams()=params;std::filesystem::remove_all(root);
    }
};
}
TEST(ChainstateWalletIndexOwner, MissingAndStoppedSourcesRefuse) {
    EXPECT_THROW(ChainstateService::AcquireWalletIndexUse({}),std::runtime_error);
    auto empty=std::make_shared<ChainstateService>();
    EXPECT_THROW(ChainstateService::AcquireWalletIndexUse(empty),std::runtime_error);
    WalletIndexOwnerFixture f;
    {auto owned=ChainstateService::AcquireWalletIndexUse(f.source);EXPECT_EQ(&owned->Index(),f.source->utxoIndex());}
    f.source->Stop();EXPECT_THROW(ChainstateService::AcquireWalletIndexUse(f.source),std::runtime_error);
    EXPECT_NO_THROW(f.source->Stop());
}
TEST(ChainstateWalletIndexOwner, NestedOwnershipPreservesExactIndex) {
    WalletIndexOwnerFixture f;auto outer=ChainstateService::AcquireWalletIndexUse(f.source);
    auto inner=ChainstateService::AcquireWalletIndexUse(f.source);
    EXPECT_EQ(&outer->Index(),&inner->Index());EXPECT_FALSE(f.source->Init(f.context));
    EXPECT_THROW(f.source->Stop(),std::logic_error);
    outer->Index().ApplyAtomically([&]{if(!outer->Index().SetMetadata("owner_probe","retained"))throw std::runtime_error("fixture metadata");});
    EXPECT_EQ(inner->Index().GetMetadata("owner_probe"),std::optional<std::string>("retained"));
    inner.reset();EXPECT_THROW(f.source->Stop(),std::logic_error);
    std::weak_ptr<ChainstateService> retained=f.source;f.context.chainstate.reset();f.source.reset();
    ASSERT_FALSE(retained.expired());EXPECT_EQ(outer->Index().GetMetadata("owner_probe"),std::optional<std::string>("retained"));
    f.source=retained.lock();outer.reset();EXPECT_NO_THROW(f.source->Stop());
}
TEST(ChainstateWalletIndexOwner, SelectedOwnershipRefusesShutdownBeforeEffects) {
    WalletIndexOwnerFixture f;
    {auto selected=f.source->AcquireBlockIngressActivationLock();EXPECT_THROW(f.source->Stop(),std::logic_error);}
    auto owner=ChainstateService::AcquireWalletIndexUse(f.source);
    EXPECT_EQ(&owner->Index(),f.source->utxoIndex());
}
TEST(ChainstateWalletIndexOwner, WalletBindingRequiresCurrentOwnedIndex) {
    WalletIndexOwnerFixture f;auto wallet=std::make_shared<WalletService>();
    ASSERT_TRUE(wallet->Init(f.context));f.context.wallet=wallet;
    {auto use=WalletService::AcquireWalletUse(wallet);auto index=ChainstateService::AcquireWalletIndexUse(f.source);
     use->Wallet().setUTXOIndex(&index->Index());use->Wallet().create("owned-index");}
    EXPECT_TRUE(wallet->EnsureRuntimeWalletBindings());
    f.source->Stop();EXPECT_FALSE(wallet->EnsureRuntimeWalletBindings());
    std::string error;EXPECT_FALSE(wallet->RecoverActiveWalletFromSnapshotIfNeeded(&error));EXPECT_FALSE(error.empty());
    {auto use=WalletService::AcquireWalletUse(wallet);EXPECT_EQ(use->Wallet().getCurrentWalletName(),"owned-index");}
    wallet->Stop();f.context.wallet.reset();
}
} // namespace dinero
