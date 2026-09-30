#pragma once
#include "vault/ledger.h"
#include <limits>

namespace dinero {
namespace {
struct LedgerAppendImage {
    std::vector<vault::LedgerEntry> entries;
    std::unordered_map<vault::AccountId,vault::LedgerAccount> accounts;
    vault::LedgerCaps caps;
    vault::UnaAmount credits,loss;
    vault::LedgerSeq next;
    bool operator==(const LedgerAppendImage&) const = default;
};
LedgerAppendImage CaptureLedgerAppend(const vault::Ledger& ledger) {
    return {ledger.entries(),ledger.accounts(),ledger.caps(),ledger.totalOpenCredits(),
            ledger.totalOperatorLoss(),ledger.nextSeq()};
}
vault::OutpointId LedgerAppendOutpoint(uint8_t id) {
    vault::OutpointId out{};out.txid_raw.fill(id);return out;
}
void ExpectLedgerAppendRefusal(vault::Ledger& ledger,const vault::LedgerEntry& entry,
                              vault::LedgerError::Kind kind) {
    const auto before=CaptureLedgerAppend(ledger);
    try {ledger.append(entry);ADD_FAILURE()<<"expected checked append refusal";}
    catch(const vault::LedgerError& e) {EXPECT_EQ(e.kind(),kind);}
    EXPECT_EQ(CaptureLedgerAppend(ledger),before);
}
void PrepareRevertedLedgerDeposit(vault::Ledger& ledger,const vault::AccountId& account,uint8_t id) {
    const auto op=LedgerAppendOutpoint(id);
    ledger.append(vault::DepositObserved{ledger.nextSeq(),0,account,op,0});
    ledger.append(vault::CreditOpened{ledger.nextSeq(),0,account,op,0});
    ledger.append(vault::CreditReverted{ledger.nextSeq(),0,account,op});
}
}
TEST(VaultLedgerAppend, InitialZeroGapsExhaustionAndReplay) {
    vault::Ledger ledger;const vault::AccountId account{"owner"};
    ledger.append(vault::DepositObserved{0,0,account,LedgerAppendOutpoint(1),10});
    EXPECT_EQ(ledger.nextSeq(),1u);
    ledger.append(vault::CreditOpened{7,0,account,LedgerAppendOutpoint(1),10});
    EXPECT_EQ(ledger.nextSeq(),8u);
    const auto image=CaptureLedgerAppend(ledger);
    EXPECT_EQ(CaptureLedgerAppend(vault::Ledger::replay(ledger.entries())),image);
    ExpectLedgerAppendRefusal(ledger,vault::CreditSettled{7,0,account,LedgerAppendOutpoint(1)},
                             vault::LedgerError::Kind::SEQUENCE_NOT_MONOTONIC);
    ExpectLedgerAppendRefusal(ledger,vault::CreditSettled{UINT64_MAX,0,account,LedgerAppendOutpoint(1)},
                             vault::LedgerError::Kind::SEQUENCE_EXHAUSTED);
    ledger.append(vault::CreditSettled{UINT64_MAX-1,0,account,LedgerAppendOutpoint(1)});
    EXPECT_EQ(ledger.nextSeq(),UINT64_MAX);EXPECT_EQ(ledger.accountOr(account).confirmed(),10u);
    ExpectLedgerAppendRefusal(ledger,vault::PolicyAdjustment{UINT64_MAX,0,account,"exhausted",1,0},
                             vault::LedgerError::Kind::SEQUENCE_EXHAUSTED);
    EXPECT_EQ(CaptureLedgerAppend(vault::Ledger::replay(ledger.entries())),CaptureLedgerAppend(ledger));
}
TEST(VaultLedgerAppend, AccountAndGlobalCapsPreserveOwner) {
    const vault::AccountId a{"a"},b{"b"};const auto first=LedgerAppendOutpoint(1),second=LedgerAppendOutpoint(2);
    vault::Ledger ledger;
    ledger.append(vault::CreditOpened{0,0,a,first,UINT64_MAX});
    ExpectLedgerAppendRefusal(ledger,vault::CreditOpened{1,0,a,second,1},
                             vault::LedgerError::Kind::PER_USER_CAP_EXCEEDED);
    ExpectLedgerAppendRefusal(ledger,vault::CreditOpened{1,0,b,second,1},
                             vault::LedgerError::Kind::OPEN_CREDITS_EXCEED_CAP);
    EXPECT_FALSE(ledger.accounts().contains(b));
    vault::Ledger bounded{vault::LedgerCaps{10,12,15}};
    bounded.append(vault::CreditOpened{0,0,a,first,10});
    ExpectLedgerAppendRefusal(bounded,vault::CreditOpened{1,0,a,second,3},
                             vault::LedgerError::Kind::PER_USER_CAP_EXCEEDED);
    bounded.append(vault::CreditOpened{1,0,a,second,2});
    ExpectLedgerAppendRefusal(bounded,vault::CreditOpened{2,0,b,first,4},
                             vault::LedgerError::Kind::OPEN_CREDITS_EXCEED_CAP);
    bounded.append(vault::CreditOpened{2,0,b,first,3});
    EXPECT_EQ(bounded.totalOpenCredits(),15u);
    EXPECT_EQ(CaptureLedgerAppend(vault::Ledger::replay(bounded.entries(),bounded.caps())),CaptureLedgerAppend(bounded));
}
TEST(VaultLedgerAppend, BalanceAndLossBoundariesPreserveOwner) {
    const vault::AccountId a{"a"},b{"b"};const auto first=LedgerAppendOutpoint(1),second=LedgerAppendOutpoint(2);
    const auto max=std::numeric_limits<int64_t>::max(),min=std::numeric_limits<int64_t>::min();
    vault::Ledger balances;
    balances.append(vault::PolicyAdjustment{0,0,a,"credit",max,0});
    balances.append(vault::PolicyAdjustment{1,0,a,"credit",max,0});
    balances.append(vault::PolicyAdjustment{2,0,a,"credit",1,0});
    EXPECT_EQ(balances.accountOr(a).confirmed(),UINT64_MAX);
    ExpectLedgerAppendRefusal(balances,vault::PolicyAdjustment{3,0,a,"overflow",1,0},vault::LedgerError::Kind::ARITHMETIC_OVERFLOW);
    ExpectLedgerAppendRefusal(balances,vault::CreditOpened{3,0,a,first,1},vault::LedgerError::Kind::ARITHMETIC_OVERFLOW);
    EXPECT_EQ(balances.totalOpenCredits(),0u);
    balances.append(vault::PolicyAdjustment{3,0,a,"debit",min,0});
    EXPECT_EQ(balances.accountOr(a).confirmed(),static_cast<uint64_t>(max));
    vault::Ledger locks;
    locks.append(vault::WithdrawalInitiated{0,0,a,first,UINT64_MAX,vault::BackendId{"test"}});
    ExpectLedgerAppendRefusal(locks,vault::WithdrawalInitiated{1,0,a,second,1,vault::BackendId{"test"}},vault::LedgerError::Kind::ARITHMETIC_OVERFLOW);
    vault::Ledger loss;
    loss.append(vault::PolicyAdjustment{0,0,a,"debit",min,0});
    EXPECT_EQ(loss.accountOr(a).operatorLoss(),uint64_t{1}<<63);
    ExpectLedgerAppendRefusal(loss,vault::PolicyAdjustment{1,0,a,"debit",min,0},vault::LedgerError::Kind::ARITHMETIC_OVERFLOW);
    vault::Ledger global;
    PrepareRevertedLedgerDeposit(global,a,1);PrepareRevertedLedgerDeposit(global,b,2);
    global.append(vault::CompensatingDebit{global.nextSeq(),0,a,first,0,UINT64_MAX});
    ExpectLedgerAppendRefusal(global,vault::CompensatingDebit{global.nextSeq(),0,b,second,0,1},vault::LedgerError::Kind::ARITHMETIC_OVERFLOW);
    EXPECT_EQ(global.accountOr(b).operatorLoss(),0u);EXPECT_EQ(global.totalOperatorLoss(),UINT64_MAX);
    ExpectLedgerAppendRefusal(global,vault::CompensatingDebit{global.nextSeq(),0,a,first,0,1},vault::LedgerError::Kind::ARITHMETIC_OVERFLOW);
    // Direct account callers also retain values on checked numeric refusal.
    vault::LedgerAccount direct{a};direct.applyCreditOpened(first,UINT64_MAX);const auto before=direct;
    EXPECT_THROW(direct.applyPolicyAdjustment(1),std::overflow_error);EXPECT_EQ(direct,before);
    EXPECT_THROW(direct.applyCreditOpened(second,1),std::overflow_error);EXPECT_EQ(direct,before);
}
TEST(VaultLedgerAppend, LifecycleFailureAndSuccessfulRetryPreserveHistory) {
    vault::Ledger ledger;const vault::AccountId a{"a"},b{"unseen"};const auto op=LedgerAppendOutpoint(4);
    ledger.append(vault::DepositObserved{0,17,a,op,25});
    ExpectLedgerAppendRefusal(ledger,vault::CreditSettled{1,18,a,op},vault::LedgerError::Kind::LIFECYCLE_INCONSISTENT);
    ExpectLedgerAppendRefusal(ledger,vault::WithdrawalSettled{1,18,b,op},vault::LedgerError::Kind::LIFECYCLE_INCONSISTENT);
    EXPECT_FALSE(ledger.accounts().contains(b));
    ledger.append(vault::CreditOpened{1,19,a,op,25});ledger.append(vault::CreditSettled{2,20,a,op});
    EXPECT_EQ(ledger.accountOr(a).confirmed(),25u);EXPECT_EQ(ledger.totalOpenCredits(),0u);
    const auto before=CaptureLedgerAppend(ledger);
    ExpectLedgerAppendRefusal(ledger,vault::CreditOpened{3,21,a,op,25},vault::LedgerError::Kind::DEPOSIT_LIFECYCLE_CLOSED);
    EXPECT_EQ(CaptureLedgerAppend(vault::Ledger::replay(ledger.entries())),before);
    ledger.append(vault::WithdrawalInitiated{3,22,a,op,5,vault::BackendId{"test"}});
    ledger.append(vault::WithdrawalReverted{4,23,a,op});
    EXPECT_EQ(ledger.accountOr(a).spendable(),25u);EXPECT_EQ(ledger.accountOr(a).locked(),0u);
    EXPECT_EQ(CaptureLedgerAppend(vault::Ledger::replay(ledger.entries())),CaptureLedgerAppend(ledger));
}
} // namespace dinero
