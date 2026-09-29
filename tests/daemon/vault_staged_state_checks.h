#pragma once
namespace dinero {
namespace {
struct VaultStateSource {
    std::array<uint8_t,32> hash{};
    bool unknown=false, included=true;
    unsigned calls=0, fail_at=0;
    std::unique_ptr<vault::VaultService> service;
    VaultStateSource() {
        hash.fill(1);
        service=std::make_unique<vault::VaultService>(
            std::make_unique<vault::InMemorySigningBackend>(vault::BackendId{"fixture"}),
            vault::VaultServiceConfig{},
            [this](uint64_t){return unknown?std::array<uint8_t,32>{}:hash;},
            [this](const auto&,uint64_t,const auto&){
                ++calls;
                if(fail_at && calls==fail_at)throw std::runtime_error("fixture source unavailable");
                return included;
            });
    }
    std::array<uint8_t,32> Add(uint8_t tag,const char* account,uint64_t amount) {
        std::array<uint8_t,32> txid{};txid.fill(tag);
        service->recordDeposit(txid,0,vault::AccountId{account},amount,100,hash);return txid;
    }
};
}
TEST(VaultStagedState, UnknownAndLateReadFailureKeepWholeTipUnpublished) {
    VaultStateSource f;f.Add(1,"first",100);f.Add(2,"second",200);
    const auto empty_metrics=f.service->metrics();
    const auto empty_account=f.service->accountMetrics(vault::AccountId{"first"});
    f.unknown=true;
    EXPECT_THROW(f.service->tipChanged(101),vault::ReorgError);
    EXPECT_EQ(f.service->metrics(),empty_metrics);
    EXPECT_EQ(f.service->accountMetrics(vault::AccountId{"first"}),empty_account);
    EXPECT_TRUE(f.service->entriesSince(0).empty());EXPECT_EQ(f.service->accountCount(),0u);
    f.unknown=false;f.fail_at=2;
    EXPECT_THROW(f.service->tipChanged(101),std::runtime_error);EXPECT_EQ(f.calls,2u);
    EXPECT_TRUE(f.service->entriesSince(0).empty());EXPECT_EQ(f.service->totalOpenCredits(),0u);
    f.fail_at=0;f.calls=0;f.service->tipChanged(101);
    EXPECT_EQ(f.calls,2u);EXPECT_EQ(f.service->totalOpenCredits(),300u);
    EXPECT_EQ(f.service->accountPending(vault::AccountId{"first"}),100u);
    EXPECT_EQ(f.service->accountPending(vault::AccountId{"second"}),200u);
    EXPECT_EQ(f.service->entriesSince(0).size(),4u);
    const auto published=f.service->metrics();
    EXPECT_EQ(published.total_open_credits,300u);EXPECT_EQ(published.total_operator_loss,0u);
    EXPECT_EQ(published.account_count,2u);EXPECT_EQ(published.ledger_next_seq,4u);
    EXPECT_EQ(published.withdrawal_queue_depth,0);
    const auto account=f.service->accountMetrics(vault::AccountId{"first"});
    EXPECT_EQ(account.pending,100u);EXPECT_EQ(account.spendable,100u);
    EXPECT_EQ(account.confirmed,0u);EXPECT_EQ(account.locked,0u);EXPECT_EQ(account.operator_loss,0u);
    EXPECT_EQ(empty_metrics.ledger_next_seq,0u);EXPECT_EQ(empty_account.pending,0u);
    f.service->tipChanged(105);
    EXPECT_EQ(published.total_open_credits,300u);EXPECT_EQ(account.pending,100u);
    EXPECT_EQ(f.service->metrics().total_open_credits,0u);
    EXPECT_EQ(f.service->accountMetrics(vault::AccountId{"first"}).confirmed,100u);
}
TEST(VaultStagedState, LaterOrphanFailureRetainsEarlierDeposit) {
    VaultStateSource f;f.Add(1,"first",100);f.Add(2,"second",200);f.service->tipChanged(101);
    const auto prior=f.service->entriesSince(0);const auto seq=f.service->ledgerNextSeq();
    f.hash.fill(2);f.included=false;f.calls=0;f.fail_at=2;
    EXPECT_THROW(f.service->tipChanged(105),std::runtime_error);EXPECT_EQ(f.calls,2u);
    EXPECT_EQ(f.service->entriesSince(0),prior);EXPECT_EQ(f.service->ledgerNextSeq(),seq);
    EXPECT_EQ(f.service->totalOpenCredits(),300u);EXPECT_EQ(f.service->totalOperatorLoss(),0u);
    EXPECT_EQ(f.service->accountPending(vault::AccountId{"first"}),100u);
    EXPECT_EQ(f.service->accountPending(vault::AccountId{"second"}),200u);
    f.fail_at=0;f.service->tipChanged(105);
    EXPECT_EQ(f.service->totalOpenCredits(),0u);EXPECT_EQ(f.service->ledgerNextSeq(),seq+4);
    const auto completed=f.service->entriesSince(0);f.service->tipChanged(106);
    EXPECT_EQ(f.service->entriesSince(0),completed);
}
TEST(VaultStagedState, WithdrawalSettlementWaitsForCompleteTip) {
    VaultStateSource f;f.Add(1,"first",100);f.service->tipChanged(105);
    const auto id=f.service->enqueueWithdrawal(vault::AccountId{"first"},40,{0x51});
    ASSERT_EQ(f.service->processNextWithdrawal(),std::optional<vault::WithdrawalId>{id});
    f.service->markWithdrawalIncluded(id,110);
    const auto prior=f.service->entriesSince(0);const auto state=f.service->withdrawalState(id);
    f.unknown=true;EXPECT_THROW(f.service->tipChanged(111),vault::ReorgError);
    EXPECT_EQ(f.service->entriesSince(0),prior);EXPECT_EQ(f.service->withdrawalState(id),state);
    EXPECT_EQ(f.service->accountLocked(vault::AccountId{"first"}),40u);
    f.unknown=false;f.service->tipChanged(111);
    EXPECT_TRUE(std::holds_alternative<vault::WithdrawalSettledOnChain>(f.service->withdrawalState(id)));
    EXPECT_EQ(f.service->accountLocked(vault::AccountId{"first"}),0u);
    EXPECT_EQ(f.service->accountConfirmed(vault::AccountId{"first"}),60u);
}
TEST(VaultStagedState, ConflictingObservationAndCapFailurePreserveOwner) {
    VaultStateSource f;const auto txid=f.Add(1,"first",100);f.service->tipChanged(101);
    const auto prior=f.service->entriesSince(0);
    EXPECT_THROW(f.service->recordDeposit(txid,0,vault::AccountId{"other"},100,100,f.hash),std::runtime_error);
    EXPECT_THROW(f.service->recordDeposit(txid,0,vault::AccountId{"first"},101,100,f.hash),std::runtime_error);
    EXPECT_THROW(f.service->recordDeposit(txid,0,vault::AccountId{"first"},100,101,f.hash),std::runtime_error);
    auto changed=f.hash;changed.fill(2);
    EXPECT_THROW(f.service->recordDeposit(txid,0,vault::AccountId{"first"},100,100,changed),std::runtime_error);
    EXPECT_NO_THROW(f.service->recordDeposit(txid,0,vault::AccountId{"first"},100,100,f.hash));
    EXPECT_EQ(f.service->entriesSince(0),prior);EXPECT_EQ(f.service->accountPending(vault::AccountId{"first"}),100u);
    vault::VaultServiceConfig config;config.ledger_caps.per_deposit=50;
    vault::VaultService capped(std::make_unique<vault::InMemorySigningBackend>(vault::BackendId{"fixture"}),
        config,[&f](uint64_t){return f.hash;},[](const auto&,uint64_t,const auto&){return true;});
    capped.recordDeposit(txid,0,vault::AccountId{"first"},100,100,f.hash);
    EXPECT_THROW(capped.tipChanged(101),vault::DepositFlowError);
    EXPECT_TRUE(capped.entriesSince(0).empty());EXPECT_EQ(capped.accountCount(),0u);
    EXPECT_NO_THROW(capped.tipChanged(100));EXPECT_EQ(capped.entriesSince(0).size(),1u);
    const auto observed=capped.entriesSince(0);EXPECT_THROW(capped.tipChanged(101),vault::DepositFlowError);
    EXPECT_EQ(capped.entriesSince(0),observed);
}
#ifdef DINERO_TEST_ORCHARD_ORIGIN
TEST(VaultStagedState, ActualCanonicalStoreRefusalAndRetryPreserveLedger) {
    VaultObservationFixture f;f.ArchiveHistorical(1);VaultRuntimeReset reset;
    vault::InitializeVaultRuntime(VaultOwnerChainConfig(f));const auto owner=vault::GetVaultRuntimeService();ASSERT_TRUE(owner);
    const auto txid=VaultRawHash(f.f.blocks[1].vtx.front().GetTxid().AsUint256());
    uint64_t amount=0,height=0;std::array<uint8_t,32> hash{};std::string error;
    ASSERT_TRUE(vault::VerifyOperatorDeposit(owner,txid,0,amount,height,hash,error))<<error;
    owner->recordDeposit(txid,0,vault::AccountId{"fixture-owner"},amount,height,hash);
    f.f.db.close();EXPECT_THROW(owner->tipChanged(101),vault::ReorgError);
    EXPECT_EQ(owner->ledgerNextSeq(),0u);EXPECT_EQ(owner->accountCount(),0u);
    ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);EXPECT_NO_THROW(owner->tipChanged(101));
    EXPECT_EQ(owner->accountConfirmed(vault::AccountId{"fixture-owner"}),amount);
    ExecutionContext context;context.daemon=&f.context;din::Json no_params;
    const auto metrics=din::rpc_vault_metrics(context,no_params);
    ASSERT_FALSE(metrics.isMember("error"));
    EXPECT_EQ(metrics["total_open_credits_una"].asUInt64(),0u);
    EXPECT_EQ(metrics["total_operator_loss_una"].asUInt64(),0u);
    EXPECT_EQ(metrics["account_count"].asUInt64(),1u);
    EXPECT_EQ(metrics["ledger_next_seq"].asUInt64(),owner->ledgerNextSeq());
    EXPECT_EQ(metrics["withdrawal_queue_depth"].asInt(),0);
    auto account_params=din::arr();account_params.append("fixture-owner");
    const auto account_metrics=din::rpc_vault_account_metrics(context,account_params);
    ASSERT_FALSE(account_metrics.isMember("error"));
    EXPECT_EQ(account_metrics["account_id"].asString(),"fixture-owner");
    EXPECT_EQ(account_metrics["spendable_una"].asUInt64(),amount);
    EXPECT_EQ(account_metrics["confirmed_una"].asUInt64(),amount);
    EXPECT_EQ(account_metrics["pending_una"].asUInt64(),0u);
    EXPECT_EQ(account_metrics["locked_una"].asUInt64(),0u);
    EXPECT_EQ(account_metrics["operator_loss_una"].asUInt64(),0u);
    const auto prior=owner->entriesSince(0);f.f.db.close();
    EXPECT_THROW(owner->tipChanged(101),vault::ReorgError);EXPECT_EQ(owner->entriesSince(0),prior);
    ASSERT_EQ(f.f.db.init(f.f.path),Status::Ok);EXPECT_NO_THROW(owner->tipChanged(101));
    EXPECT_EQ(owner->entriesSince(0),prior);
}
#endif
} // namespace dinero
