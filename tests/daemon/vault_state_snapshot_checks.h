#pragma once

#include "vault/state_snapshot.h"
#include <algorithm>
#include <limits>

namespace dinero::vault::state_snapshot_checks {
std::array<uint8_t,32> Hash(uint8_t value) {
    std::array<uint8_t,32> out{};out.fill(value);return out;
}
std::unique_ptr<VaultService> Service() {
    return std::make_unique<VaultService>(
        std::make_unique<InMemorySigningBackend>(BackendId{"test"}),VaultServiceConfig{},
        [](uint64_t){return Hash(9);},
        [](const OutpointId&,uint64_t,const std::array<uint8_t,32>&){return true;});
}
VaultStateSnapshot SettledServiceState() {
    auto service=Service();
    service->recordDeposit(Hash(1),7,AccountId{"alice"},1000,100,Hash(9));
    service->tipChanged(105);
    auto id=service->enqueueWithdrawal(AccountId{"alice"},200,{0x51,0x20,3});
    EXPECT_EQ(service->processNextWithdrawal(),std::optional<WithdrawalId>{id});
    service->markWithdrawalIncluded(id,110);
    service->tipChanged(111);
    auto state=service->captureState();
    EXPECT_EQ(service->accountConfirmed(AccountId{"alice"}),800U);
    return state;
}

TEST(VaultStateSnapshot, ActualServiceCaptureAndLedgerReplay) {
    auto state=SettledServiceState();
    auto bytes=EncodeVaultState(state);
    auto decoded=DecodeVaultState(bytes);
    EXPECT_EQ(EncodeVaultState(decoded),bytes);
    auto ledger=ReplayVaultStateLedger(decoded);
    EXPECT_EQ(ledger.entries(),state.entries);
    EXPECT_EQ(ledger.accountOr(AccountId{"alice"}).confirmed(),800U);
    EXPECT_EQ(ledger.accountOr(AccountId{"alice"}).locked(),0U);
    ASSERT_EQ(decoded.deposits.size(),1U);
    EXPECT_EQ(decoded.deposits[0].deposit.outpoint.vout,7U);
    EXPECT_EQ(decoded.deposits[0].deposit.deposit_height,100U);
    EXPECT_EQ(decoded.deposits[0].observed_block,Hash(9));
    ASSERT_EQ(decoded.withdrawals.size(),1U);
    EXPECT_EQ(decoded.withdrawals[0].request.destination_script_pub_key,
              (std::vector<uint8_t>{0x51,0x20,3}));
    EXPECT_TRUE(std::holds_alternative<WithdrawalSettledOnChain>(decoded.withdrawals[0].state));
}

TEST(VaultStateSnapshot, ExactFormatAllVariantsAndCanonicalInventory) {
    VaultStateSnapshot state;state.revision=99;
    state.config.confirmation_policy.size_matrix={{50,7},{100,9}};
    state.config.shadow_mode=true;
    AccountId account{std::string("a\0b",3)};
    OutpointId op;op.txid_raw=Hash(2);op.vout=UINT32_MAX;
    state.entries={DepositObserved{0,-7,account,op,100},CreditOpened{2,0,account,op,100},
        CreditSettled{3,1,account,op},CreditReverted{4,2,account,op},
        WithdrawalInitiated{5,3,account,op,20,BackendId{"backend"}},
        WithdrawalSettled{6,4,account,op},WithdrawalReverted{7,5,account,op},
        CompensatingDebit{8,6,account,op,100,1},
        PolicyAdjustment{9,7,account,std::string("note\0tail",9),INT64_MIN,INT64_MAX}};
    for (uint8_t i=1;i<=5;++i) {
        TrackedDeposit deposit;deposit.outpoint.txid_raw=Hash(i);deposit.account=account;
        deposit.stage=static_cast<DepositStage>(i-1);
        state.deposits.push_back({deposit,Hash(8)});
    }
    const std::vector<WithdrawalState> variants={WithdrawalPending{},WithdrawalSigning{},
        WithdrawalBroadcast{Hash(3),77},WithdrawalSettledOnChain{Hash(4)},
        WithdrawalRevertedOnChain{Hash(5)},WithdrawalFailed{std::string("fail\0tail",9)}};
    for (size_t i=0;i<variants.size();++i) {
        WithdrawalRequest request;request.request_id[0]=static_cast<uint8_t>(i+1);
        request.account=account;request.amount=UINT64_MAX;request.destination_script_pub_key={0,1,0,255};
        request.created_at=INT64_MIN;
        state.withdrawals.push_back({request,variants[i]});
    }
    const auto bytes=EncodeVaultState(state);
    std::reverse(state.deposits.begin(),state.deposits.end());
    std::reverse(state.withdrawals.begin(),state.withdrawals.end());
    EXPECT_EQ(EncodeVaultState(state),bytes);
    const auto decoded=DecodeVaultState(bytes);
    EXPECT_EQ(decoded.entries,state.entries);
    EXPECT_EQ(EncodeVaultState(decoded),bytes);
    for (size_t i=0;i<variants.size();++i) EXPECT_EQ(decoded.withdrawals[i].state,variants[i]);
    // This is a framing fixture, deliberately not a valid service lifecycle.
    EXPECT_THROW((void)ReplayVaultStateLedger(decoded),std::runtime_error);
}

TEST(VaultStateSnapshot, IncompleteDuplicateAndTerminalInputRefuse) {
    auto state=SettledServiceState();
    const auto bytes=EncodeVaultState(state);
    for (size_t size=0;size<bytes.size();++size) {
        SCOPED_TRACE(size);
        EXPECT_THROW((void)DecodeVaultState(std::span<const uint8_t>{bytes.data(),size}),std::runtime_error);
    }
    auto extra=bytes;extra.push_back(0);
    EXPECT_THROW((void)DecodeVaultState(extra),std::runtime_error);
    auto duplicate=state;duplicate.deposits.push_back(duplicate.deposits.front());
    EXPECT_THROW((void)EncodeVaultState(duplicate),std::runtime_error);
    duplicate=state;duplicate.withdrawals.push_back(duplicate.withdrawals.front());
    EXPECT_THROW((void)EncodeVaultState(duplicate),std::runtime_error);
    auto invalid=state;invalid.withdrawals[0].request.request_id={};
    EXPECT_THROW((void)EncodeVaultState(invalid),std::runtime_error);
    invalid=state;invalid.deposits[0].observed_block={};
    EXPECT_THROW((void)EncodeVaultState(invalid),std::runtime_error);
    EXPECT_EQ(EncodeVaultState(state),bytes);
}

TEST(VaultStateSnapshot, SavedLifecycleAndEveryLedgerBindingRequired) {
    auto state=SettledServiceState();
    auto invalid=state;invalid.deposits.clear();
    EXPECT_THROW((void)ReplayVaultStateLedger(invalid),std::runtime_error);
    invalid=state;invalid.withdrawals.clear();
    EXPECT_THROW((void)ReplayVaultStateLedger(invalid),std::runtime_error);
    invalid=state;invalid.deposits[0].deposit.amount+=1;
    EXPECT_THROW((void)ReplayVaultStateLedger(invalid),std::runtime_error);
    invalid=state;invalid.deposits[0].deposit.account=AccountId{"foreign"};
    EXPECT_THROW((void)ReplayVaultStateLedger(invalid),std::runtime_error);
    invalid=state;invalid.deposits[0].deposit.stage=DepositStage::CREDITED;
    EXPECT_THROW((void)ReplayVaultStateLedger(invalid),std::runtime_error);
    invalid=state;invalid.withdrawals[0].request.amount+=1;
    EXPECT_THROW((void)ReplayVaultStateLedger(invalid),std::runtime_error);
    invalid=state;invalid.withdrawals[0].state=WithdrawalSigning{};
    EXPECT_THROW((void)ReplayVaultStateLedger(invalid),std::runtime_error);
    invalid=state;invalid.withdrawals[0].state=WithdrawalSettledOnChain{Hash(88)};
    EXPECT_THROW((void)ReplayVaultStateLedger(invalid),std::runtime_error);
    invalid=state;invalid.revision=0;
    EXPECT_THROW((void)ReplayVaultStateLedger(invalid),std::runtime_error);
    EXPECT_NO_THROW((void)ReplayVaultStateLedger(state));
}

TEST(VaultStateSnapshot, ObservedAndDetectedReversionKeepExistingLedgerShape) {
    for (bool observed:{false,true}) {
        auto backend=std::make_unique<InMemorySigningBackend>(BackendId{"test"});
        bool included=true;
        VaultService service(std::move(backend),VaultServiceConfig{},
            [](uint64_t){return Hash(9);},
            [&](const OutpointId&,uint64_t,const std::array<uint8_t,32>&){return included;});
        service.recordDeposit(Hash(1),0,AccountId{"alice"},100,100,Hash(9));
        if (observed) service.tipChanged(100);
        included=false;service.tipChanged(100);
        auto state=service.captureState();
        ASSERT_EQ(state.deposits.size(),1U);
        EXPECT_EQ(state.deposits[0].deposit.stage,DepositStage::REVERTED);
        EXPECT_EQ(state.entries.size(),observed?1U:0U);
        EXPECT_NO_THROW((void)ReplayVaultStateLedger(state));
    }
}
} // namespace dinero::vault::state_snapshot_checks
