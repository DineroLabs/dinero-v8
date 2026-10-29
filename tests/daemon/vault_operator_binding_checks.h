#pragma once
#include "vault_state_store_checks.h"
namespace dinero {
namespace {
vault::VaultOperatorBinding BindingForVaultFixture() {
    vault::VaultOperatorBinding binding;
    binding.script_pub_key.assign(34,0x29);binding.script_pub_key[0]=0x51;binding.script_pub_key[1]=0x20;
    binding.account="preserved-account";return binding;
}
}
TEST(VaultOperatorBinding, PersistedBindingSurvivesMutationsAndReopen) {
    VaultStateStoreFixture f;const auto binding=BindingForVaultFixture();
    f.config.operator_binding=binding;auto bound=f.Create();
    ASSERT_EQ(bound.service->operatorBinding(),binding);
    auto first=vault::EncodeVaultState(bound.service->captureState());ASSERT_GT(first.size(),6u);EXPECT_EQ(first[5],'3');
    f.Fund(bound.service);bound.service->enqueueWithdrawal(vault::AccountId{binding.account},200,{0x51});
    const auto saved=vault::EncodeVaultState(bound.service->captureState()),rows=f.Rows();
    EXPECT_EQ(vault::DecodeVaultState(saved).config.operator_binding,binding);
    const auto session=f.Session();bound.service.reset();
    {auto use=WalletService::AcquireWalletUse(f.wallet.service);use->Wallet().open("owner");}
    EXPECT_NE(f.Session(),session);auto reopened=f.Open(bound.identity);
    EXPECT_EQ(reopened.service->operatorBinding(),binding);
    EXPECT_EQ(vault::EncodeVaultState(reopened.service->captureState()),saved);EXPECT_EQ(f.Rows(),rows);
    EXPECT_EQ(reopened.service->accountConfirmed(vault::AccountId{binding.account}),1000u);
}
TEST(VaultOperatorBinding, SuccessorsCannotAddRemoveOrRelabelAuthority) {
    VaultStateStoreFixture f;auto historical=f.Create();
    EXPECT_FALSE(historical.service->operatorBinding());
    const auto old=vault::EncodeVaultState(historical.service->captureState());EXPECT_EQ(old[5],'2');
    EXPECT_FALSE(vault::DecodeVaultState(old).config.operator_binding);
    f.config.operator_binding=BindingForVaultFixture();auto bound=f.Create();
    const auto rows=f.Rows();const auto session=f.Session();
    auto reject=[&](const vault::BoundVaultService& owner,auto edit) {
        const auto changes=sqlite3_total_changes(f.Db());
        {auto use=WalletService::AcquireWalletUse(f.wallet.service);
         auto tx=vault::VaultStateTransaction::OpenExisting(use->Wallet(),session,f.domain,owner.identity);
         auto next=tx->Current().state;++next.revision;edit(next.config.operator_binding);
         EXPECT_THROW(tx->Stage(next),std::exception);}
        EXPECT_EQ(sqlite3_total_changes(f.Db()),changes);EXPECT_EQ(f.Rows(),rows);
    };
    reject(historical,[](auto& b){b=BindingForVaultFixture();});
    reject(bound,[](auto& b){b.reset();});
    reject(bound,[](auto& b){b->script_pub_key.back()^=1;});
    reject(bound,[](auto& b){b->account="different-account";});
    EXPECT_FALSE(f.Open(historical.identity).service->operatorBinding());
    EXPECT_EQ(f.Open(bound.identity).service->operatorBinding(),f.config.operator_binding);
    EXPECT_NO_THROW(f.Fund(bound.service));EXPECT_EQ(bound.service->operatorBinding(),f.config.operator_binding);
}
TEST(VaultOperatorBinding, MalformedAndIncompleteBindingRefuse) {
    vault::VaultStateSnapshot state;state.config.operator_binding=BindingForVaultFixture();
    const auto bytes=vault::EncodeVaultState(state);
    EXPECT_EQ(vault::EncodeVaultState(vault::DecodeVaultState(bytes)),bytes);
    for(size_t length=0;length<bytes.size();++length) {
        const std::span<const uint8_t> prefix(bytes.data(),length);
        EXPECT_THROW(vault::DecodeVaultState(prefix),std::exception)<<length;
    }
    auto trailing=bytes;trailing.push_back(0);EXPECT_THROW(vault::DecodeVaultState(trailing),std::exception);
    auto reject=[&](auto edit) {auto invalid=state;edit(*invalid.config.operator_binding);EXPECT_THROW(vault::EncodeVaultState(invalid),std::exception);};
    reject([](auto& b){b.script_pub_key.pop_back();});
    reject([](auto& b){b.script_pub_key[0]=0x00;});
    reject([](auto& b){std::fill(b.script_pub_key.begin()+2,b.script_pub_key.end(),0);});
    reject([](auto& b){b.account.clear();});
    reject([](auto& b){b.account=std::string("owner\0suffix",12);});
    reject([](auto& b){b.account.assign(1025,'a');});
    VaultStateStoreFixture f;f.config.operator_binding=BindingForVaultFixture();auto bound=f.Create();const auto rows=f.Rows();
    f.config.operator_binding->account.clear();EXPECT_THROW(f.Create(),std::exception);EXPECT_EQ(f.Rows(),rows);
    EXPECT_EQ(bound.service->operatorBinding(),BindingForVaultFixture());
}
TEST(VaultOperatorBinding, RequiredReadAndCommitRefusalPreserveBinding) {
    VaultStateStoreFixture f;f.config.operator_binding=BindingForVaultFixture();auto bound=f.Create();f.Fund(bound.service);
    const auto rows=f.Rows(),saved=vault::EncodeVaultState(bound.service->captureState());auto* db=f.Db();
    sqlite3_set_authorizer(db,[](void*,int action,const char* table,const char*,const char*,const char*) {
        return action==SQLITE_READ && table && std::string(table)=="wallet_vault_states"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_THROW(f.Open(bound.identity),std::exception);sqlite3_set_authorizer(db,nullptr,nullptr);
    EXPECT_EQ(sqlite3_get_autocommit(db),1);EXPECT_EQ(f.Rows(),rows);
    unsigned commits=0;sqlite3_commit_hook(db,[](void* p){++*static_cast<unsigned*>(p);return 1;},&commits);
    EXPECT_THROW(bound.service->enqueueWithdrawal(vault::AccountId{"preserved-account"},200,{0x51}),std::exception);
    sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_EQ(commits,1u);
    EXPECT_EQ(f.Rows(),rows);EXPECT_EQ(vault::EncodeVaultState(bound.service->captureState()),saved);
    EXPECT_EQ(f.Open(bound.identity).service->operatorBinding(),f.config.operator_binding);
}
} // namespace dinero
