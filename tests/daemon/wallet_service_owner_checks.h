#pragma once
#include "daemon/services/wallet_service.h"
#include "wallet/utxo_index.h"
#include <functional>
namespace dinero {
namespace {
struct ServiceOwnerLogger final:ILogger {
    bool throw_stop=false;
    std::function<void()> on_refused_binding;
    void info(const std::string& text)override {
        if(throw_stop && text=="[WalletService] Stopping wallet service...")
            throw std::runtime_error("fixture diagnostic unavailable");
    }
    void error(const std::string& text)override {
        if(on_refused_binding && text.find("Script inventory reload refused")!=std::string::npos)
            on_refused_binding();
    }
    void log(LogLevel,const std::string& text)override{info(text);}
    void debug(const std::string&)override{} void warning(const std::string&)override{}
    void setLogLevel(LogLevel)override{} void setLogFile(const std::string&)override{} void shutdown()override{}
};
struct WalletServiceOwnerFixture {
    ServiceOwnerLogger logger;
    DaemonContext context;
    std::filesystem::path root;
    std::shared_ptr<WalletService> service;
    std::unique_ptr<UTXOIndex> index;
    WalletServiceOwnerFixture() {
        char path[]="/tmp/dinero-wallet-service-owner-XXXXXX";
        if(!mkdtemp(path))throw std::runtime_error("fixture temporary directory");root=path;
        auto config=std::make_shared<ConfigService>();config->Set("datadir",root.string());
        context.config=config;context.logger_interface=&logger;
        service=std::make_shared<WalletService>();context.wallet=service;
        if(!service->Init(context))throw std::runtime_error("fixture real service Init");
        index=std::make_unique<UTXOIndex>((root/"index.db").string());
        auto use=WalletService::AcquireWalletUse(service);use->Wallet().setUTXOIndex(index.get());
        use->Wallet().create("owner");
    }
    ~WalletServiceOwnerFixture(){logger.throw_stop=false;logger.on_refused_binding={};if(service)service->Stop();context.wallet.reset();service.reset();index.reset();std::filesystem::remove_all(root);}
};
}
TEST(WalletServiceOwner, MissingAndStoppedServiceRefuse) {
    EXPECT_THROW(WalletService::AcquireWalletUse({}),std::runtime_error);
    auto empty=std::make_shared<WalletService>();EXPECT_THROW(WalletService::AcquireWalletUse(empty),std::runtime_error);
    EXPECT_FALSE(empty->Start());EXPECT_FALSE(empty->EnsureRuntimeWalletBindings());std::string error;
    EXPECT_FALSE(empty->RecoverActiveWalletFromSnapshotIfNeeded(&error));EXPECT_FALSE(error.empty());
    EXPECT_NO_THROW(empty->Stop());
    WalletServiceOwnerFixture f;EXPECT_TRUE(f.service->hasActiveWallet());EXPECT_EQ(f.service->getCurrentWalletName(),"owner");
    f.service->Stop();EXPECT_THROW(WalletService::AcquireWalletUse(f.service),std::runtime_error);
    EXPECT_THROW(f.service->listWallets(),std::runtime_error);EXPECT_FALSE(f.service->EnsureRuntimeWalletBindings());
    EXPECT_FALSE(f.service->RecoverActiveWalletFromSnapshotIfNeeded(&error));EXPECT_FALSE(error.empty());
}
TEST(WalletServiceOwner, NestedOwnerRetainsExactManagerAndRefusesShutdown) {
    WalletServiceOwnerFixture f;auto outer=WalletService::AcquireWalletUse(f.service);
    auto* manager=&outer->Wallet();auto inner=WalletService::AcquireWalletUse(f.service);
    EXPECT_EQ(&inner->Wallet(),manager);EXPECT_FALSE(f.service->Init(f.context));
    EXPECT_THROW(f.service->Stop(),std::logic_error);EXPECT_EQ(manager->getCurrentWalletName(),"owner");
    inner.reset();EXPECT_THROW(f.service->Stop(),std::logic_error);
    std::weak_ptr<WalletService> retained=f.service;f.context.wallet.reset();f.service.reset();
    ASSERT_FALSE(retained.expired());EXPECT_EQ(&outer->Wallet(),manager);EXPECT_TRUE(manager->hasActiveWallet());
    f.service=retained.lock();outer.reset();EXPECT_NO_THROW(f.service->Stop());
}
TEST(WalletServiceOwner, ActualBindingFailureKeepsOwnerThroughDiagnostics) {
    WalletServiceOwnerFixture f;sqlite3* db=nullptr;
    {auto use=WalletService::AcquireWalletUse(f.service);auto lease=use->Wallet().AcquireDatabaseLease();db=lease->Database();}
    unsigned refused=0;f.logger.on_refused_binding=[&]{
        ++refused;EXPECT_THROW(f.service->Stop(),std::logic_error);
        EXPECT_TRUE(f.service->hasActiveWallet());
    };
    sqlite3_set_authorizer(db,[](void*,int action,const char*,const char*,const char*,const char*){
        return action==SQLITE_READ?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_FALSE(f.service->EnsureRuntimeWalletBindings());
    sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_EQ(refused,1u);
    f.logger.on_refused_binding={};EXPECT_TRUE(f.service->EnsureRuntimeWalletBindings());
    EXPECT_EQ(f.service->getCurrentWalletName(),"owner");
}
TEST(WalletServiceOwner, DiagnosticFailureStillClosesAndReopensOriginalWallet) {
    WalletServiceOwnerFixture f;std::string address;
    {auto use=WalletService::AcquireWalletUse(f.service);auto& wallet=use->Wallet();
     address=wallet.getPrimaryAddress();ASSERT_FALSE(address.empty());
     wallet.encryptWallet("service-owner-password");wallet.unlockWallet("service-owner-password",0);}
    f.logger.throw_stop=true;EXPECT_NO_THROW(f.service->Stop());
    EXPECT_THROW(WalletService::AcquireWalletUse(f.service),std::runtime_error);f.logger.throw_stop=false;
    ASSERT_TRUE(f.service->Init(f.context));
    {auto use=WalletService::AcquireWalletUse(f.service);auto& wallet=use->Wallet();wallet.setUTXOIndex(f.index.get());
     wallet.open("owner");EXPECT_TRUE(wallet.isLocked());wallet.unlockWallet("service-owner-password",0);
     EXPECT_EQ(wallet.getCurrentWalletName(),"owner");EXPECT_EQ(wallet.getPrimaryAddress(),address);}
}
} // namespace dinero
