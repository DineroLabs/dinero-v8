#pragma once
namespace {
namespace cap_vault = dinero::vault;
cap_vault::OutpointId CapOutpoint(uint8_t tag) {cap_vault::OutpointId out;out.txid_raw.fill(tag);return out;}
void OpenCapCredit(cap_vault::Ledger& ledger,const cap_vault::AccountId& account,uint8_t tag,uint64_t amount,bool settle=false) {
    const auto out=CapOutpoint(tag);
    ledger.append(cap_vault::DepositObserved{ledger.nextSeq(),0,account,out,amount});
    ledger.append(cap_vault::CreditOpened{ledger.nextSeq(),0,account,out,amount});
    if(settle)ledger.append(cap_vault::CreditSettled{ledger.nextSeq(),0,account,out});
}
TEST(VaultRevertedCreditCap, SettledRevertPreservesOtherCreditAndPerAccountLimit) {
    cap_vault::Ledger ledger({100,60,500});const cap_vault::AccountId a{"a"};
    OpenCapCredit(ledger,a,1,40,true);OpenCapCredit(ledger,a,2,50);
    ASSERT_EQ(ledger.totalOpenCredits(),50u);
    ledger.append(cap_vault::CreditReverted{ledger.nextSeq(),0,a,CapOutpoint(1)});
    EXPECT_EQ(ledger.totalOpenCredits(),50u);EXPECT_EQ(ledger.accountOr(a).pending(),50u);EXPECT_EQ(ledger.accountOr(a).confirmed(),0u);
    const auto entries=ledger.entries();const auto accounts=ledger.accounts();const auto next=ledger.nextSeq();
    EXPECT_THROW(ledger.append(cap_vault::CreditOpened{ledger.nextSeq(),0,a,CapOutpoint(3),20}),cap_vault::LedgerError);
    EXPECT_EQ(ledger.entries(),entries);EXPECT_EQ(ledger.accounts(),accounts);EXPECT_EQ(ledger.nextSeq(),next);
    const auto replayed=cap_vault::Ledger::replay(entries,ledger.caps());EXPECT_EQ(replayed.totalOpenCredits(),50u);EXPECT_EQ(replayed.accounts(),accounts);
}
TEST(VaultRevertedCreditCap, SettledRevertPreservesGlobalLimitAcrossAccounts) {
    cap_vault::Ledger ledger({100,100,60});const cap_vault::AccountId a{"a"},b{"b"},c{"c"};
    OpenCapCredit(ledger,a,1,40,true);OpenCapCredit(ledger,b,2,50);
    ledger.append(cap_vault::CreditReverted{ledger.nextSeq(),0,a,CapOutpoint(1)});
    EXPECT_EQ(ledger.totalOpenCredits(),50u);const auto entries=ledger.entries();const auto accounts=ledger.accounts();
    EXPECT_THROW(ledger.append(cap_vault::CreditOpened{ledger.nextSeq(),0,c,CapOutpoint(3),20}),cap_vault::LedgerError);
    EXPECT_EQ(ledger.entries(),entries);EXPECT_EQ(ledger.accounts(),accounts);
    ledger.append(cap_vault::CreditOpened{ledger.nextSeq(),0,c,CapOutpoint(3),10});EXPECT_EQ(ledger.totalOpenCredits(),60u);
    EXPECT_EQ(cap_vault::Ledger::replay(ledger.entries(),ledger.caps()).totalOpenCredits(),60u);
}
TEST(VaultRevertedCreditCap, CreditedRevertStillReleasesExactlyItsOwnCapacity) {
    cap_vault::Ledger ledger({100,100,100});const cap_vault::AccountId a{"a"},b{"b"};
    OpenCapCredit(ledger,a,1,40);OpenCapCredit(ledger,a,2,10);OpenCapCredit(ledger,b,3,20);
    ledger.append(cap_vault::CreditReverted{ledger.nextSeq(),0,a,CapOutpoint(1)});
    EXPECT_EQ(ledger.totalOpenCredits(),30u);EXPECT_EQ(ledger.accountOr(a).pending(),10u);EXPECT_EQ(ledger.accountOr(b).pending(),20u);
    OpenCapCredit(ledger,a,4,70);EXPECT_EQ(ledger.totalOpenCredits(),100u);
    ledger.append(cap_vault::CreditSettled{ledger.nextSeq(),0,a,CapOutpoint(4)});EXPECT_EQ(ledger.totalOpenCredits(),30u);
    ledger.append(cap_vault::CreditReverted{ledger.nextSeq(),0,a,CapOutpoint(4)});EXPECT_EQ(ledger.totalOpenCredits(),30u);
    const auto replay=cap_vault::Ledger::replay(ledger.entries(),ledger.caps());EXPECT_EQ(replay.totalOpenCredits(),30u);EXPECT_EQ(replay.accounts(),ledger.accounts());
}
class VaultRevertedCreditCapWallet : public WalletBatchRpc {
protected:
    cap_vault::VaultStateDomain domain;cap_vault::BoundVaultService vault;
    cap_vault::AccountId account{"cap-owner"};bool orphan=false;
    std::array<uint8_t,32> hash{};
    auto backend(){return std::make_unique<cap_vault::InMemorySigningBackend>(cap_vault::BackendId{"cap-fixture"});}
    auto capture(){return [this](uint64_t height,const std::vector<cap_vault::VaultDepositQuery>& queries){
        cap_vault::VaultTipSnapshot snapshot{height,hash,{}};
        for(const auto& q:queries)snapshot.deposits.push_back({q,hash,!(orphan && q.outpoint==CapOutpoint(1))});return snapshot;
    };}
    auto identity(){return dinero::CaptureWalletSigningIdentity(service->get(),"owner");}
    void record(uint8_t tag,uint64_t amount,uint64_t height){vault.service->recordDeposit(CapOutpoint(tag).txid_raw,0,account,amount,height,hash);}
    std::string sealed(){
        auto lease=service->get().AcquireDatabaseLease();sqlite3_stmt* raw=nullptr;
        if(sqlite3_prepare_v2(lease->Database(),"SELECT hex(sealed) FROM wallet_vault_states",-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("cap fixture prepare");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> stmt(raw,sqlite3_finalize);
        if(sqlite3_step(raw)!=SQLITE_ROW)throw std::runtime_error("cap fixture row");std::string result(reinterpret_cast<const char*>(sqlite3_column_text(raw,0)));
        if(sqlite3_step(raw)!=SQLITE_DONE)throw std::runtime_error("cap fixture EOF");return result;
    }
    void SetUp() override {
        dinero::SelectParams(dinero::Chain::REGTEST);WalletBatchRpc::SetUp();if(HasFatalFailure())return;
        domain.network=static_cast<uint8_t>(dinero::GetActiveChain());dinero::uint256 genesis;
        ASSERT_TRUE(dinero::uint256::FromHex(dinero::Params().genesis_hash,genesis));std::copy(genesis.begin(),genesis.end(),domain.genesis.begin());hash.fill(19);
        cap_vault::VaultServiceConfig config;config.ledger_caps={100,60,60};config.confirmation_policy.k_observe=1;config.confirmation_policy.k_credit=2;config.confirmation_policy.k_settle=10;
        vault=cap_vault::WalletVaultStateOwner::CreateNewService(service,identity().session,domain,config,backend(),[this](uint64_t){return hash;},[](const auto&,uint64_t,const auto&){return true;},capture());
        record(1,40,1);vault.service->tipChanged(10);record(2,50,9);vault.service->tipChanged(10);
        ASSERT_EQ(vault.service->totalOpenCredits(),50u);ASSERT_EQ(vault.service->accountConfirmed(account),40u);ASSERT_EQ(vault.service->accountPending(account),50u);
    }
    void TearDown() override {vault.service.reset();WalletBatchRpc::TearDown();}
    void reopen(){
        const auto id=vault.identity;vault.service.reset();service->get().open("owner");service->get().unlockWallet("historical-rpc",0);
        vault=cap_vault::WalletVaultStateOwner::OpenExistingService(service,identity().session,domain,id,backend(),[this](uint64_t){return hash;},[](const auto&,uint64_t,const auto&){return true;},capture());
    }
};
TEST_F(VaultRevertedCreditCapWallet, CheckedCommitReopenAndCapRefusalPreserveAuthenticatedOwner) {
    orphan=true;const auto before=cap_vault::EncodeVaultState(vault.service->captureState());const auto ciphertext=sealed();auto* db=service->get().getCurrentDatabase();
    sql(db,"CREATE TRIGGER deny_cap_state BEFORE UPDATE ON wallet_vault_states BEGIN SELECT RAISE(ABORT,'cap state refusal'); END");
    EXPECT_THROW(vault.service->tipChanged(10),std::runtime_error);
    sql(db,"DROP TRIGGER deny_cap_state");EXPECT_EQ(sealed(),ciphertext);EXPECT_EQ(cap_vault::EncodeVaultState(vault.service->captureState()),before);
    unsigned commits=0;sqlite3_commit_hook(db,[](void* p){++*static_cast<unsigned*>(p);return 1;},&commits);
    EXPECT_THROW(vault.service->tipChanged(10),std::runtime_error);
    sqlite3_commit_hook(db,nullptr,nullptr);EXPECT_GT(commits,0u);EXPECT_EQ(sealed(),ciphertext);EXPECT_EQ(cap_vault::EncodeVaultState(vault.service->captureState()),before);
    vault.service->tipChanged(10);EXPECT_EQ(vault.service->totalOpenCredits(),50u);EXPECT_EQ(vault.service->accountPending(account),50u);
    const auto saved=cap_vault::EncodeVaultState(vault.service->captureState());const auto saved_cipher=sealed();reopen();
    EXPECT_EQ(cap_vault::EncodeVaultState(vault.service->captureState()),saved);EXPECT_EQ(sealed(),saved_cipher);EXPECT_EQ(vault.service->totalOpenCredits(),50u);
    record(3,20,9);const auto prior=cap_vault::EncodeVaultState(vault.service->captureState());const auto prior_cipher=sealed();
    EXPECT_THROW(vault.service->tipChanged(10),cap_vault::DepositFlowError);
    EXPECT_EQ(cap_vault::EncodeVaultState(vault.service->captureState()),prior);EXPECT_EQ(sealed(),prior_cipher);EXPECT_EQ(vault.service->totalOpenCredits(),50u);
    EXPECT_TRUE(service->get().getPendingPayments().empty());EXPECT_EQ(ingress->tests,0);EXPECT_EQ(ingress->submits,0);
}
} // namespace
