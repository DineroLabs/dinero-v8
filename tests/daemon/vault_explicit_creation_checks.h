#pragma once
#include "vault_runtime_attachment_checks.h"
namespace dinero {
namespace {
struct VaultExplicitCreationFixture : VaultRuntimeAttachmentFixture {
    std::string address;
    VaultExplicitCreationFixture() {
        auto use=WalletService::AcquireWalletUse(f->wallet.service);
        address=use->Wallet().getNewAddress("vault operator");
        f->config.operator_binding=vault::VaultOperatorBinding{
            CreateP2TRScriptPubKey(DecodeTaprootWitnessProgram(address)),"preserved-account"};
    }
    vault::VaultIdentity CreateOwned() {
        auto use=WalletService::AcquireWalletUse(f->wallet.service);
        const auto selected=CaptureWalletSigningIdentity(use->Wallet(),"owner");
        auto tx=vault::VaultStateTransaction::CreateNewOwned(use->Wallet(),selected.session,f->domain,f->config);
        const auto id=tx->Current().identity;tx->Commit();return id;
    }
    std::vector<vault::VaultStateSummary> List() {
        auto use=WalletService::AcquireWalletUse(f->wallet.service);
        const auto selected=CaptureWalletSigningIdentity(use->Wallet(),"owner");
        return vault::VaultStateTransaction::ListExisting(use->Wallet(),selected.session,f->domain);
    }
    din::Json Request() {
        din::Json args(::Json::arrayValue),v;v["operator_address"]=address;
        v["account_id"]="preserved-account";v["shadow_mode"]=true;
        v["k_observe"]=1;v["k_credit"]=2;v["k_settle"]=6;v["withdrawal_k_settle"]=6;
        v["per_deposit_cap_una"]=1000;v["per_account_cap_una"]=2000;v["global_cap_una"]=3000;
        v["per_withdrawal_cap_una"]=500;v["per_account_outstanding_cap_una"]=1000;v["max_queue_depth"]=10;
        args.append(v);return args;
    }
};
}
TEST(VaultExplicitCreation, OwnedCreationListsAndReopensExactPresentState) {
    VaultRuntimeReset reset;VaultExplicitCreationFixture fixture;auto& f=*fixture.f;
    EXPECT_THROW(fixture.List(),std::exception);EXPECT_FALSE(vault::GetVaultRuntimeService());
    const auto first=fixture.CreateOwned();auto funded=f.Open(first);f.Fund(funded.service);
    const auto second=fixture.CreateOwned();const auto rows=f.Rows();const auto changes=sqlite3_total_changes(f.Db());
    const auto list=fixture.List();ASSERT_EQ(list.size(),2u);EXPECT_LT(list[0].identity,list[1].identity);
    for(const auto& entry:list) {
        EXPECT_EQ(entry.operator_binding,f.config.operator_binding);
        if(entry.identity==first)EXPECT_EQ(entry.revision,2u);else {EXPECT_EQ(entry.identity,second);EXPECT_EQ(entry.revision,0u);}
    }
    EXPECT_EQ(f.Rows(),rows);EXPECT_EQ(sqlite3_total_changes(f.Db()),changes);
    {auto use=WalletService::AcquireWalletUse(f.wallet.service);use->Wallet().open("owner");}
    EXPECT_EQ(fixture.List().size(),2u);EXPECT_EQ(f.Rows(),rows);
    fixture.Open(first);ASSERT_TRUE(vault::GetVaultRuntimeService());
    EXPECT_EQ(vault::GetVaultRuntimeService()->accountConfirmed(vault::AccountId{"preserved-account"}),1000u);
}
TEST(VaultExplicitCreation, ExactOperatorKeyReadUsesCallerTransactionWithoutPublication) {
    VaultRuntimeReset reset;VaultExplicitCreationFixture fixture;auto& f=*fixture.f;
    const auto id=fixture.CreateOwned();const auto rows=f.Rows();
    auto use=WalletService::AcquireWalletUse(f.wallet.service);auto& wallet=use->Wallet();
    std::array<uint8_t,32> imported{};imported.back()=23;
    const auto imported_address=wallet.importPrivateKey(std::vector<uint8_t>(imported.begin(),imported.end()),"owned vault import");
    ASSERT_FALSE(imported_address.empty());
    const auto imported_script=CreateP2TRScriptPubKey(DecodeTaprootWitnessProgram(imported_address));
    auto lease=wallet.AcquireDatabaseLease();auto seed=lease->CopyRecoverySeed(lease->Session());auto* db=lease->Database();
    const auto script=f.config.operator_binding->script_pub_key;
    EXPECT_THROW(lease->ResolveSigningKeyInTransaction(util::hex(script),*seed),std::runtime_error);
    VaultStateStoreFixture::Exec(db,"BEGIN IMMEDIATE");const auto changes=sqlite3_total_changes(db);
    for(const auto& candidate:{script,imported_script}) {
        const auto key=lease->ResolveSigningKeyInTransaction(util::hex(candidate),*seed);
        ASSERT_TRUE(key);EXPECT_EQ(key->script,candidate);EXPECT_EQ(key->secret.size(),32u);
        EXPECT_EQ(key->policy,SigningKeyPolicy::TaprootCanonical);EXPECT_FALSE(sqlite3_get_autocommit(db));
        EXPECT_FALSE(lease->ResolveSigningKey(util::hex(candidate),*seed));
    }
    EXPECT_EQ(sqlite3_total_changes(db),changes);EXPECT_FALSE(vault::GetVaultRuntimeService());
    VaultStateStoreFixture::Exec(db,"ROLLBACK");EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Rows(),rows);
    (void)id;
}
TEST(VaultExplicitCreation, UnknownKeyAndCommitRefusalPreservePriorOwners) {
    VaultRuntimeReset reset;VaultExplicitCreationFixture fixture;auto& f=*fixture.f;
    const auto id=fixture.CreateOwned();const auto rows=f.Rows();const auto original=f.config.operator_binding;
    f.config.operator_binding=BindingForVaultFixture();
    EXPECT_THROW(fixture.CreateOwned(),std::exception);
    f.config.operator_binding=original;EXPECT_EQ(f.Rows(),rows);EXPECT_FALSE(vault::GetVaultRuntimeService());
    auto* db=f.Db();unsigned commits=0;sqlite3_commit_hook(db,[](void* p){++*static_cast<unsigned*>(p);return 1;},&commits);
    EXPECT_THROW(fixture.CreateOwned(),std::exception);sqlite3_commit_hook(db,nullptr,nullptr);
    EXPECT_EQ(commits,1u);EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Rows(),rows);
    EXPECT_EQ(fixture.List().size(),1u);EXPECT_EQ(fixture.List()[0].identity,id);
}
TEST(VaultExplicitCreation, LateMalformedAndInterruptedInventoriesReturnNoPrefix) {
    VaultRuntimeReset reset;VaultExplicitCreationFixture fixture;auto& f=*fixture.f;
    for(int i=0;i<3;++i)(void)fixture.CreateOwned();auto* db=f.Db();const auto rows=f.Rows();
    unsigned observed=0;sqlite3_trace_v2(db,SQLITE_TRACE_ROW,[](unsigned,void* data,void* statement,void*) {
        auto* q=static_cast<sqlite3_stmt*>(statement);const auto* sql=sqlite3_sql(q);
        if(sql && std::string(sql).find("SELECT vault_id,length(sealed)")==0 && ++*static_cast<unsigned*>(data)==2)
            sqlite3_interrupt(sqlite3_db_handle(q));
        return 0;
    },&observed);
    EXPECT_THROW(fixture.List(),std::exception);sqlite3_trace_v2(db,0,nullptr,nullptr);
    EXPECT_GE(observed,2u);EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(f.Rows(),rows);EXPECT_EQ(fixture.List().size(),3u);
    VaultStateStoreFixture::Exec(db,"UPDATE wallet_vault_states SET sealed=X'00' WHERE vault_id=(SELECT max(vault_id) FROM wallet_vault_states)");
    const auto corrupted=f.Rows();
    EXPECT_THROW(fixture.List(),std::exception);EXPECT_EQ(f.Rows(),corrupted);EXPECT_FALSE(vault::GetVaultRuntimeService());
}
TEST(VaultExplicitCreation, MissingIdentityDeniedReadAndCallerTransactionNeverEnroll) {
    VaultRuntimeReset reset;VaultExplicitCreationFixture fixture;auto& f=*fixture.f;
    const auto id=fixture.CreateOwned();auto* db=f.Db();const auto rows=f.Rows();
    sqlite3_set_authorizer(db,[](void*,int op,const char* table,const char*,const char*,const char*) {
        return op==SQLITE_READ && table && std::string(table)=="wallet_vault_states"?SQLITE_DENY:SQLITE_OK;
    },nullptr);
    EXPECT_THROW(fixture.List(),std::exception);sqlite3_set_authorizer(db,nullptr,nullptr);EXPECT_TRUE(sqlite3_get_autocommit(db));
    unsigned commits=0;sqlite3_commit_hook(db,[](void* p){++*static_cast<unsigned*>(p);return 1;},&commits);
    EXPECT_THROW(fixture.List(),std::exception);sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_EQ(commits,1u);
    {
        auto use=WalletService::AcquireWalletUse(f.wallet.service);auto lease=use->Wallet().AcquireDatabaseLease();
        VaultStateStoreFixture::Exec(db,"CREATE TABLE vault_caller_probe(value INTEGER); BEGIN IMMEDIATE; INSERT INTO vault_caller_probe VALUES(7)");
        EXPECT_THROW(fixture.List(),std::exception);EXPECT_FALSE(sqlite3_get_autocommit(db));
        VaultStateStoreFixture::Exec(db,"COMMIT");
    }
    EXPECT_EQ(f.Rows(),rows);
    VaultStateStoreFixture::Exec(db,"UPDATE wallet_meta SET runtime_delivery_id=NULL WHERE id=1");
    const auto changes=sqlite3_total_changes(db);
    EXPECT_THROW(fixture.List(),std::exception);
    EXPECT_THROW(f.Open(id),std::exception);
    EXPECT_THROW(fixture.CreateOwned(),std::exception);
    EXPECT_TRUE(sqlite3_get_autocommit(db));EXPECT_EQ(sqlite3_total_changes(db),changes);EXPECT_EQ(f.Rows(),rows);
}
TEST(VaultExplicitCreation, ActualCreateAndListRpcKeepRuntimeAndExistingOwnersSeparate) {
    VaultRuntimeReset reset;VaultExplicitCreationFixture fixture;auto& f=*fixture.f;
    auto request=fixture.Request();din::Json empty(::Json::arrayValue);
    EXPECT_TRUE(din::rpc_vault_list(fixture.Context(),empty).isMember("error"));
    auto wrong=fixture.Context();wrong.walletName="wrong-wallet";
    EXPECT_TRUE(din::rpc_vault_create(wrong,request).isMember("error"));
    auto malformed=request;malformed[0]["k_credit"]=0;
    EXPECT_TRUE(din::rpc_vault_create(fixture.Context(),malformed).isMember("error"));
    const auto created=din::rpc_vault_create(fixture.Context(),request);ASSERT_FALSE(created.isMember("error"))<<created.toStyledString();
    EXPECT_TRUE(created["created"].asBool());EXPECT_FALSE(created["attached"].asBool());EXPECT_FALSE(vault::GetVaultRuntimeService());
    const auto rows=f.Rows();const auto listed=din::rpc_vault_list(fixture.Context(),empty);
    ASSERT_FALSE(listed.isMember("error"));ASSERT_EQ(listed["vaults"].size(),1u);
    EXPECT_EQ(listed["vaults"][0]["vault_id"],created["vault_id"]);EXPECT_FALSE(listed["historical_completeness_verified"].asBool());
    EXPECT_EQ(f.Rows(),rows);EXPECT_FALSE(vault::GetVaultRuntimeService());
    din::Json open(::Json::arrayValue),id;id["vault_id"]=created["vault_id"];open.append(id);
    ASSERT_FALSE(din::rpc_vault_open(fixture.Context(),open).isMember("error"));auto service=vault::GetVaultRuntimeService();ASSERT_TRUE(service);
    const auto second=din::rpc_vault_create(fixture.Context(),request);ASSERT_FALSE(second.isMember("error"));
    EXPECT_NE(second["vault_id"],created["vault_id"]);EXPECT_EQ(vault::GetVaultRuntimeService(),service);
    const auto state=service->captureState();EXPECT_TRUE(state.entries.empty());EXPECT_TRUE(state.deposits.empty());EXPECT_TRUE(state.withdrawals.empty());
    EXPECT_TRUE(state.config.shadow_mode);EXPECT_EQ(state.config.confirmation_policy.k_credit,2u);
    EXPECT_EQ(state.config.ledger_caps.global,3000u);EXPECT_EQ(state.config.withdrawal_caps.global_queue_depth,10);
    EXPECT_EQ(din::rpc_vault_list(fixture.Context(),empty)["vaults"].size(),2u);
}
} // namespace dinero
