#pragma once
#include "vault/ledger_store.h"
#include <fstream>
#include <filesystem>
#include <atomic>
#include <chrono>
namespace dinero {
namespace {
struct VaultRuntimeGuardFiles {
    std::filesystem::path root;
    VaultRuntimeGuardFiles() {
        static std::atomic<uint64_t> count{0};
        root=std::filesystem::temp_directory_path()/("dinero-vault-runtime-guard-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(count++));
        if(!std::filesystem::create_directory(root))throw std::runtime_error("fixture directory unavailable");
    }
    ~VaultRuntimeGuardFiles(){std::error_code ec;std::filesystem::remove_all(root,ec);}
    static void write(const std::filesystem::path& p,const std::string& bytes) {
        std::ofstream f(p,std::ios::binary|std::ios::trunc);f.write(bytes.data(),bytes.size());f.close();
        if(!f)throw std::runtime_error("fixture write failed");
    }
    static std::string read(const std::filesystem::path& p) {
        std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("fixture read failed");
        return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
    }
};
}
TEST(VaultRuntimeRestoreGuard, DisabledAndMissingReadersNeverPublish) {
    VaultRuntimeReset reset;
    auto cfg=VaultOwnerConfig();cfg.enabled=false;EXPECT_FALSE(vault::InitializeVaultRuntime(cfg));
    EXPECT_FALSE(vault::GetVaultRuntimeService());
    cfg.enabled=true;cfg.block_hash_at_height={};EXPECT_THROW(vault::InitializeVaultRuntime(cfg),std::runtime_error);
    EXPECT_FALSE(vault::GetVaultRuntimeService());
    cfg=VaultOwnerConfig();cfg.tx_included_at={};EXPECT_THROW(vault::InitializeVaultRuntime(cfg),std::runtime_error);
    EXPECT_FALSE(vault::GetVaultRuntimeService());EXPECT_TRUE(vault::GetVaultOperator().address.empty());
}
TEST(VaultRuntimeRestoreGuard, RecordedLedgerNeverBecomesEmptyRuntime) {
    VaultRuntimeGuardFiles files;VaultRuntimeReset reset;const auto path=files.root/"ledger.jsonl";
    {vault::FileLedgerStore store(path.string());vault::OutpointId out{};out.txid_raw.fill(3);
     store.append(vault::DepositObserved{0,1,vault::AccountId{"recorded-owner"},out,750});store.flush();}
    const auto before=files.read(path);ASSERT_FALSE(before.empty());auto cfg=VaultOwnerConfig();cfg.persistence_path=path.string();
    EXPECT_THROW(vault::InitializeVaultRuntime(cfg),std::runtime_error);EXPECT_EQ(files.read(path),before);
    EXPECT_FALSE(vault::GetVaultRuntimeService());EXPECT_TRUE(vault::GetVaultOperator().address.empty());
    EXPECT_FALSE(std::filesystem::exists(files.root/"idempotency.jsonl"));
    EXPECT_THROW(vault::InitializeVaultRuntime(cfg),std::runtime_error);EXPECT_EQ(files.read(path),before);
}
TEST(VaultRuntimeRestoreGuard, HistoricalSidecarAndMalformedStorageRefuse) {
    VaultRuntimeGuardFiles files;VaultRuntimeReset reset;const auto path=files.root/"ledger.jsonl",sidecar=files.root/"idempotency.jsonl";
    files.write(path,"");files.write(sidecar,"historical sidecar bytes\n");const auto original=files.read(sidecar);
    auto cfg=VaultOwnerConfig();cfg.persistence_path=path.string();
    EXPECT_THROW(vault::InitializeVaultRuntime(cfg),std::runtime_error);EXPECT_FALSE(vault::GetVaultRuntimeService());
    EXPECT_EQ(files.read(sidecar),original);EXPECT_TRUE(files.read(path).empty());
    // A separate malformed ledger is preserved too; no parser or repair may
    // turn a historical prefix into permission to publish empty state.
    const auto other=files.root/"other";ASSERT_TRUE(std::filesystem::create_directory(other));
    files.write(other/"ledger.jsonl","{partial");cfg.persistence_path=(other/"ledger.jsonl").string();
    EXPECT_THROW(vault::InitializeVaultRuntime(cfg),std::runtime_error);EXPECT_EQ(files.read(other/"ledger.jsonl"),"{partial");
    EXPECT_FALSE(vault::GetVaultRuntimeService());
}
TEST(VaultRuntimeRestoreGuard, FailedPreparationPreservesNoBindingAndAllowsExplicitRetry) {
    VaultRuntimeGuardFiles files;VaultRuntimeReset reset;auto cfg=VaultOwnerConfig();
    cfg.operator_address="invalid-address";cfg.persistence_path=(files.root/"absent"/"ledger.jsonl").string();
    EXPECT_THROW(vault::InitializeVaultRuntime(cfg),std::exception);EXPECT_FALSE(std::filesystem::exists(files.root/"absent"));
    cfg.operator_address="";files.write(files.root/"blocked","preserved");cfg.persistence_path=(files.root/"blocked"/"ledger.jsonl").string();
    EXPECT_THROW(vault::InitializeVaultRuntime(cfg),std::exception);EXPECT_EQ(files.read(files.root/"blocked"),"preserved");
    EXPECT_FALSE(vault::GetVaultRuntimeService());EXPECT_TRUE(vault::GetVaultOperator().address.empty());
    cfg.persistence_path=(files.root/"fresh"/"ledger.jsonl").string();EXPECT_TRUE(vault::InitializeVaultRuntime(cfg));
    const auto owner=vault::GetVaultRuntimeService();ASSERT_TRUE(owner);EXPECT_EQ(owner->accountCount(),0u);
    EXPECT_TRUE(files.read(files.root/"fresh"/"ledger.jsonl").empty());
    cfg.operator_address="invalid-address";EXPECT_TRUE(vault::InitializeVaultRuntime(cfg));
    EXPECT_EQ(vault::GetVaultRuntimeService(),owner);EXPECT_TRUE(vault::GetVaultOperator().address.empty());
}
}
