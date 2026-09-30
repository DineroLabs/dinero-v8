#pragma once
#include "wallet/wallet_worker.h"
#include "wallet/utxo_index.h"
namespace dinero {
struct WalletObservationTestAccess {
    static size_t ActiveLeases(WalletManager& wallet) {
        std::lock_guard<std::recursive_mutex> lock(wallet.database_lifecycle_mutex_);
        return wallet.database_leases_;
    }
};
struct WalletWorkerTestAccess {
    static void Connect(WalletWorker& worker,const Transaction& tx) {
        worker.ProcessConnect(20,std::string(64,'1'),{tx});
    }
};
namespace {
struct WalletObservationFixture {
    std::filesystem::path root;
    std::unique_ptr<WalletManager> wallet;
    std::unique_ptr<UTXOIndex> index;
    std::vector<uint8_t> script;
    Transaction tx;
    WalletObservationFixture() {
        char name[]="/tmp/dinero-vault-wallet-observation-XXXXXX";
        if(!mkdtemp(name))throw std::runtime_error("fixture directory");root=name;
        wallet=std::make_unique<WalletManager>(root/"wallet");wallet->create("owner");
        index=std::make_unique<UTXOIndex>((root/"index.db").string());
        if(!index->Initialize())throw std::runtime_error("fixture index");
        script.assign(34,7);script[0]=0x51;script[1]=32;
        index->RegisterAddress(script,"m/86'/1448'/0'/0/0");
        uint256 parent;parent.begin()[0]=3;tx.vin.resize(1);
        tx.vin[0].prevout=TxOutPoint(TxId(parent),0);
        tx.vout.emplace_back(AmountUna::Una(800),script);
    }
    ~WalletObservationFixture(){vault::ShutdownVaultRuntime();index.reset();wallet.reset();std::filesystem::remove_all(root);}
    vault::VaultRuntimeConfig Config(std::function<void()> during={}) {
        auto cfg=VaultOwnerConfig();cfg.operator_address=VaultOwnerAddress(script);
        cfg.default_account="captured";cfg.k_observe=cfg.k_credit=cfg.k_settle=1;
        cfg.capture_tip=[during=std::move(during)](uint64_t h,const std::vector<vault::VaultDepositQuery>& q) {
            if(during)during();std::array<uint8_t,32> hash{};hash.fill(0x11);
            vault::VaultTipSnapshot out{h,hash,{}};
            for(const auto& v:q)out.deposits.push_back({v,hash,true});return out;
        };return cfg;
    }
};
}
TEST(VaultWalletObservation, ActualWorkerCommitsHeightAndReleasesLeaseBeforeCallback) {
    VaultRuntimeReset reset;WalletObservationFixture f;unsigned calls=0;
    vault::InitializeVaultRuntime(f.Config([&]{
        ++calls;EXPECT_EQ(WalletObservationTestAccess::ActiveLeases(*f.wallet),0u);
        EXPECT_EQ(sqlite3_get_autocommit(f.wallet->getCurrentDatabase()),1);
        EXPECT_EQ(f.wallet->getBlockchainHeight(),20);
        EXPECT_TRUE(f.index->GetUTXO(f.tx.GetTxid(),0).has_value());
        sqlite3_stmt* row=nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(f.wallet->getCurrentDatabase(),"SELECT amount FROM utxos",-1,&row,nullptr),SQLITE_OK);
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> done(row,sqlite3_finalize);
        ASSERT_EQ(sqlite3_step(row),SQLITE_ROW);EXPECT_EQ(sqlite3_column_int64(row,0),800);
        EXPECT_EQ(sqlite3_step(row),SQLITE_DONE);
    }));
    WalletWorker worker(f.index.get(),f.wallet.get());
    EXPECT_NO_THROW(WalletWorkerTestAccess::Connect(worker,f.tx));EXPECT_EQ(calls,1u);
    const auto service=vault::GetVaultRuntimeService();ASSERT_TRUE(service);
    EXPECT_EQ(service->accountConfirmed(vault::AccountId{"captured"}),800u);
}
TEST(VaultWalletObservation, CapturedServiceAndBindingDoNotRetargetAfterDetach) {
    VaultRuntimeReset reset;WalletObservationFixture f;
    vault::InitializeVaultRuntime(f.Config());const auto prior=vault::GetVaultRuntimeService();
    const auto captured=vault::CaptureVaultWalletOutputObserver();ASSERT_TRUE(captured);
    std::string error;ASSERT_TRUE(vault::SetVaultOperator(VaultOwnerAddress(f.script),"changed",&error));
    vault::ShutdownVaultRuntime();auto cfg=f.Config();cfg.default_account="replacement";
    vault::InitializeVaultRuntime(cfg);const auto replacement=vault::GetVaultRuntimeService();
    std::array<uint8_t,32> txid{};txid.fill(3);
    EXPECT_NO_THROW(captured(txid,0,f.script,800,20,std::string(64,'1')));
    EXPECT_EQ(prior->accountConfirmed(vault::AccountId{"captured"}),800u);
    EXPECT_EQ(prior->accountConfirmed(vault::AccountId{"changed"}),0u);
    EXPECT_EQ(replacement->accountCount(),0u);
}
TEST(VaultWalletObservation, InvalidHashesAndUnmatchedScriptsHaveNoEffects) {
    VaultRuntimeReset reset;WalletObservationFixture f;
    EXPECT_FALSE(vault::CaptureVaultWalletOutputObserver());
    vault::InitializeVaultRuntime(f.Config());const auto service=vault::GetVaultRuntimeService();
    const auto observer=vault::CaptureVaultWalletOutputObserver();ASSERT_TRUE(observer);
    std::array<uint8_t,32> txid{};txid.fill(3);
    for(const auto& hash:std::vector<std::string>{std::string(63,'1'),std::string(64,'z'),std::string(64,'0'),std::string(65,'1')})
        EXPECT_THROW(observer(txid,0,f.script,800,20,hash),std::runtime_error);
    auto other=f.script;other.back()^=1;
    EXPECT_NO_THROW(observer(txid,0,other,800,20,std::string(64,'1')));
    EXPECT_EQ(service->accountCount(),0u);EXPECT_EQ(service->ledgerNextSeq(),0u);
    EXPECT_NO_THROW(observer(txid,0,f.script,800,20,std::string(64,'1')));
    EXPECT_EQ(service->accountConfirmed(vault::AccountId{"captured"}),800u);
}
TEST(VaultWalletObservation, FailedWalletWriteDoesNotInvokeObserverAndRetrySucceeds) {
    VaultRuntimeReset reset;WalletObservationFixture f;unsigned calls=0;
    vault::InitializeVaultRuntime(f.Config([&]{++calls;}));
    sqlite3* db=f.wallet->getCurrentDatabase();
    ASSERT_EQ(sqlite3_exec(db,"CREATE TRIGGER refuse_observation BEFORE INSERT ON utxos BEGIN SELECT RAISE(ABORT,'fixture write refusal'); END",nullptr,nullptr,nullptr),SQLITE_OK);
    WalletWorker worker(f.index.get(),f.wallet.get());const auto height=f.wallet->getBlockchainHeight();
    EXPECT_THROW(WalletWorkerTestAccess::Connect(worker,f.tx),std::runtime_error);
    EXPECT_EQ(calls,0u);EXPECT_EQ(f.wallet->getBlockchainHeight(),height);
    EXPECT_EQ(vault::GetVaultRuntimeService()->accountCount(),0u);
    EXPECT_EQ(sqlite3_get_autocommit(db),1);
    ASSERT_EQ(sqlite3_exec(db,"DROP TRIGGER refuse_observation",nullptr,nullptr,nullptr),SQLITE_OK);
    EXPECT_NO_THROW(WalletWorkerTestAccess::Connect(worker,f.tx));EXPECT_EQ(calls,1u);
    EXPECT_EQ(vault::GetVaultRuntimeService()->accountConfirmed(vault::AccountId{"captured"}),800u);
}
} // namespace dinero
