#pragma once
#include "vault/state_store.h"
#include "vault/state_snapshot.h"
namespace dinero {
namespace {
struct VaultStateStoreFixture {
    WalletServiceOwnerFixture wallet;
    vault::VaultStateDomain domain;
    vault::VaultServiceConfig config;
    std::array<uint8_t,32> block{};
    VaultStateStoreFixture() {
        domain.network=2;domain.genesis.fill(19);block.fill(41);
        config.confirmation_policy.k_observe=1;
        config.confirmation_policy.k_credit=1;
        config.confirmation_policy.k_settle=1;
    }
    uint64_t Session() {
        auto use=WalletService::AcquireWalletUse(wallet.service);
        auto lease=use->Wallet().AcquireDatabaseLease();return lease->Session();
    }
    std::unique_ptr<vault::SigningBackend> Backend() {
        return std::make_unique<vault::InMemorySigningBackend>(vault::BackendId{"store-fixture"});
    }
    vault::VaultTipSnapshotFn Capture() {
        const auto hash=block;
        return [hash](uint64_t height,const std::vector<vault::VaultDepositQuery>& queries) {
            vault::VaultTipSnapshot out{height,hash,{}};
            for(const auto& q:queries)out.deposits.push_back({q,hash,true});return out;
        };
    }
    vault::BoundVaultService Create() {
        return vault::WalletVaultStateOwner::CreateNewService(wallet.service,Session(),domain,config,
            Backend(),[](uint64_t){return std::array<uint8_t,32>{};},
            [](const auto&,uint64_t,const auto&){return false;},Capture());
    }
    vault::BoundVaultService Open(const vault::VaultIdentity& id,uint64_t session=0) {
        return vault::WalletVaultStateOwner::OpenExistingService(wallet.service,session?session:Session(),domain,id,
            Backend(),[](uint64_t){return std::array<uint8_t,32>{};},
            [](const auto&,uint64_t,const auto&){return false;},Capture());
    }
    sqlite3* Db() {auto use=WalletService::AcquireWalletUse(wallet.service);return use->Wallet().getCurrentDatabase();}
    static void Exec(sqlite3* db,const char* sql) {
        if(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("fixture SQL");
    }
    std::vector<uint8_t> Rows() {
        sqlite3_stmt* raw=nullptr;
        if(sqlite3_prepare_v2(Db(),"SELECT vault_id,revision,predecessor,sealed FROM wallet_vault_states ORDER BY vault_id",-1,&raw,nullptr)!=SQLITE_OK)
            throw std::runtime_error("fixture owner read");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> q(raw,sqlite3_finalize);
        std::vector<uint8_t> result;int rc;
        while((rc=sqlite3_step(raw))==SQLITE_ROW) {
            for(int i=0;i<4;++i) {
                if(i==1) {auto n=static_cast<uint64_t>(sqlite3_column_int64(raw,i));for(unsigned k=0;k<8;++k)result.push_back(static_cast<uint8_t>(n>>(8*k)));}
                else {const auto* b=static_cast<const uint8_t*>(sqlite3_column_blob(raw,i));const auto n=sqlite3_column_bytes(raw,i);result.insert(result.end(),b,b+n);}
            }
        }
        if(rc!=SQLITE_DONE)throw std::runtime_error("fixture owner EOF");return result;
    }
    void Fund(const std::shared_ptr<vault::VaultService>& service,uint8_t id=1) {
        std::array<uint8_t,32> txid{};txid.fill(id);
        service->recordDeposit(txid,3,vault::AccountId{"preserved-account"},1000,20,block);
        service->tipChanged(20);
    }
};
}
TEST(VaultStateStore, ExplicitCreationAndAuthenticatedServiceReopen) {
    VaultStateStoreFixture f;vault::VaultIdentity absent{};absent.fill(9);
    EXPECT_THROW(f.Open(absent),std::exception);
    sqlite3_stmt* table=nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(f.Db(),"SELECT name FROM sqlite_master WHERE name='wallet_vault_states'",-1,&table,nullptr),SQLITE_OK);
    {std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> done(table,sqlite3_finalize);EXPECT_EQ(sqlite3_step(table),SQLITE_DONE);}
    auto bound=f.Create();ASSERT_TRUE(bound.service);
    const auto id=bound.identity;EXPECT_EQ(bound.service->captureState().revision,0u);
    f.Fund(bound.service);const auto request=bound.service->enqueueWithdrawal(
        vault::AccountId{"preserved-account"},200,std::vector<uint8_t>{0x51});
    EXPECT_TRUE(std::holds_alternative<vault::WithdrawalPending>(bound.service->withdrawalState(request)));
    EXPECT_EQ(bound.service->accountConfirmed(vault::AccountId{"preserved-account"}),1000u);
    const auto saved=vault::EncodeVaultState(bound.service->captureState()),rows=f.Rows();
    const auto old_session=f.Session();auto stale=bound.service;bound.service.reset();
    {auto use=WalletService::AcquireWalletUse(f.wallet.service);use->Wallet().open("owner");}
    EXPECT_NE(f.Session(),old_session);auto reopened=f.Open(id);
    EXPECT_EQ(vault::EncodeVaultState(reopened.service->captureState()),saved);EXPECT_EQ(f.Rows(),rows);
    EXPECT_THROW(f.Open(id,old_session),std::exception);
    EXPECT_THROW(stale->enqueueWithdrawal(vault::AccountId{"preserved-account"},1,{0x51}),std::exception);
    EXPECT_EQ(vault::EncodeVaultState(stale->captureState()),saved);EXPECT_EQ(f.Rows(),rows);
    // Durable payment dispatch remains explicitly unavailable in this batch.
    EXPECT_THROW(reopened.service->processNextWithdrawal(),std::runtime_error);
    EXPECT_EQ(vault::EncodeVaultState(reopened.service->captureState()),saved);EXPECT_EQ(f.Rows(),rows);
}
TEST(VaultStateStore, RequiredWriteAndCommitRefusalPreserveLiveAndDurableState) {
    VaultStateStoreFixture f;auto bound=f.Create();f.Fund(bound.service);
    const auto saved=vault::EncodeVaultState(bound.service->captureState()),rows=f.Rows();
    auto* db=f.Db();
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*) {
        return action==SQLITE_UPDATE && table && std::string(table)=="wallet_vault_states"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_THROW(bound.service->enqueueWithdrawal(vault::AccountId{"preserved-account"},200,{0x51}),std::exception);
    sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_EQ(sqlite3_get_autocommit(db),1);EXPECT_EQ(f.Rows(),rows);
    EXPECT_EQ(vault::EncodeVaultState(bound.service->captureState()),saved);
    unsigned commits=0;
    sqlite3_commit_hook(db,[](void* value){++*static_cast<unsigned*>(value);return 1;},&commits);
    EXPECT_THROW(bound.service->enqueueWithdrawal(vault::AccountId{"preserved-account"},200,{0x51}),std::exception);
    sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_EQ(commits,1u);
    EXPECT_EQ(sqlite3_get_autocommit(db),1);EXPECT_EQ(f.Rows(),rows);
    EXPECT_EQ(vault::EncodeVaultState(bound.service->captureState()),saved);
    EXPECT_NO_THROW(bound.service->enqueueWithdrawal(vault::AccountId{"preserved-account"},200,{0x51}));
    EXPECT_NE(f.Rows(),rows);EXPECT_EQ(bound.service->captureState().revision,3u);
}
TEST(VaultStateStore, MissingCorruptAndForeignDomainsNeverCreateReplacement) {
    VaultStateStoreFixture f;auto first=f.Create();f.Fund(first.service);auto second=f.Create();
    const auto original=f.Rows();auto unknown=first.identity;unknown.back()^=1;
    EXPECT_THROW(f.Open(unknown),std::exception);EXPECT_EQ(f.Rows(),original);
    const auto domain=f.domain;f.domain.network=1;EXPECT_THROW(f.Open(first.identity),std::exception);
    f.domain=domain;f.domain.genesis.back()^=1;EXPECT_THROW(f.Open(first.identity),std::exception);
    f.domain=domain;EXPECT_EQ(f.Rows(),original);
    // Move a valid envelope to another authentic row identity. The AEAD owner
    // binding must refuse it; this does not fabricate a replacement identity.
    sqlite3_stmt* raw=nullptr;ASSERT_EQ(sqlite3_prepare_v2(f.Db(),
        "UPDATE wallet_vault_states SET sealed=(SELECT sealed FROM wallet_vault_states WHERE vault_id=?), revision=(SELECT revision FROM wallet_vault_states WHERE vault_id=?), predecessor=(SELECT predecessor FROM wallet_vault_states WHERE vault_id=?) WHERE vault_id=?",-1,&raw,nullptr),SQLITE_OK);
    {std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> done(raw,sqlite3_finalize);
     for(int i=1;i<=3;++i)ASSERT_EQ(sqlite3_bind_blob(raw,i,first.identity.data(),32,SQLITE_TRANSIENT),SQLITE_OK);
     ASSERT_EQ(sqlite3_bind_blob(raw,4,second.identity.data(),32,SQLITE_TRANSIENT),SQLITE_OK);ASSERT_EQ(sqlite3_step(raw),SQLITE_DONE);}
    const auto changed=f.Rows();EXPECT_THROW(f.Open(second.identity),std::exception);EXPECT_EQ(f.Rows(),changed);
    EXPECT_NO_THROW(f.Open(first.identity));
    VaultStateStoreFixture::Exec(f.Db(),"UPDATE wallet_vault_states SET sealed=X'00'");
    const auto corrupt=f.Rows();EXPECT_THROW(f.Open(first.identity),std::exception);EXPECT_EQ(f.Rows(),corrupt);
    EXPECT_EQ(first.service->accountConfirmed(vault::AccountId{"preserved-account"}),1000u);
}
TEST(VaultStateStore, DeniedReadsAndBorrowedTransactionRetainCallerAndOwner) {
    VaultStateStoreFixture f;auto bound=f.Create();f.Fund(bound.service);const auto rows=f.Rows();
    auto* db=f.Db();sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*) {
        return action==SQLITE_READ && table && std::string(table)=="wallet_vault_states"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_THROW(f.Open(bound.identity),std::exception);sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_EQ(sqlite3_get_autocommit(db),1);EXPECT_EQ(f.Rows(),rows);EXPECT_NO_THROW(f.Open(bound.identity));
    const auto expected_session=f.Session();
    auto caller_use=WalletService::AcquireWalletUse(f.wallet.service);
    VaultStateStoreFixture::Exec(db,"CREATE TABLE caller_probe(value INTEGER); BEGIN IMMEDIATE; INSERT INTO caller_probe VALUES(7)");
    EXPECT_THROW(f.Open(bound.identity,expected_session),std::exception);EXPECT_EQ(sqlite3_get_autocommit(db),0);
    EXPECT_THROW(vault::VaultStateTransaction::CreateNew(caller_use->Wallet(),expected_session,f.domain,f.config),std::exception);EXPECT_EQ(sqlite3_get_autocommit(db),0);
    VaultStateStoreFixture::Exec(db,"COMMIT");EXPECT_EQ(f.Rows(),rows);
    sqlite3_stmt* raw=nullptr;ASSERT_EQ(sqlite3_prepare_v2(db,"SELECT value FROM caller_probe",-1,&raw,nullptr),SQLITE_OK);
    {std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> done(raw,sqlite3_finalize);
     ASSERT_EQ(sqlite3_step(raw),SQLITE_ROW);EXPECT_EQ(sqlite3_column_int(raw,0),7);EXPECT_EQ(sqlite3_step(raw),SQLITE_DONE);}
    EXPECT_NO_THROW(f.Open(bound.identity));
}
} // namespace dinero
