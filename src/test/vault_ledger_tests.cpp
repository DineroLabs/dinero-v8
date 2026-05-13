// Copyright (c) 2026 Dinero Labs.
//
// Daemon-side gtest port of `Core/Vault/LedgerReplayTests.swift`.
// Behavioural-parity gate for the C++ ledger primitives.
//
// Each test case mirrors a Swift @Test function. Passing every test
// here on the C++ implementation guarantees the C++ ledger preserves
// the Swift reference's invariants.

#include <gtest/gtest.h>

#include "vault/ledger.h"
#include "vault/ledger_account.h"
#include "vault/ledger_entry.h"
#include "vault/vault_types.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace dinero::vault::testing {
namespace {

AccountId acct(const std::string& name) {
    return AccountId{name};
}

BackendId backendId(const std::string& name) {
    return BackendId{name};
}

OutpointId outpoint(uint8_t tag, uint32_t vout = 0) {
    OutpointId op;
    op.txid_raw.fill(tag);
    op.vout = vout;
    return op;
}

constexpr LedgerTimestamp T0 = 1'776'384'000'000'000'000;  // pinned UTC ns

// ----- happy path lifecycle -----

TEST(VaultLedger, depositObservedThenCreditedThenSettled) {
    Ledger l;
    AccountId a = acct("alice");
    OutpointId dep = outpoint(1);
    l.append(DepositObserved{1, T0, a, dep, 100});
    l.append(CreditOpened{2, T0, a, dep, 100});
    l.append(CreditSettled{3, T0, a, dep});

    auto account = l.accounts().at(a);
    EXPECT_EQ(account.pending(), 0U);
    EXPECT_EQ(account.confirmed(), 100U);
    EXPECT_EQ(account.spendable(), 100U);
    EXPECT_EQ(l.totalOpenCredits(), 0U);
    auto deposits_it = account.deposits().find(dep);
    ASSERT_NE(deposits_it, account.deposits().end());
    auto* settled = std::get_if<DepositSettledState>(&deposits_it->second);
    ASSERT_NE(settled, nullptr);
    EXPECT_EQ(settled->amount, 100U);
}

TEST(VaultLedger, creditRevertedDecrementsPending) {
    Ledger l;
    AccountId a = acct("bob");
    OutpointId dep = outpoint(2);
    l.append(DepositObserved{1, T0, a, dep, 50});
    l.append(CreditOpened{2, T0, a, dep, 50});
    l.append(CreditReverted{3, T0, a, dep});
    EXPECT_EQ(l.accounts().at(a).pending(), 0U);
    EXPECT_EQ(l.totalOpenCredits(), 0U);
}

// ----- invariant §6.2.4: sequence monotonicity -----

TEST(VaultLedger, nonMonotonicSequenceRejected) {
    Ledger l;
    l.append(DepositObserved{5, T0, acct("a"), outpoint(3), 1});
    EXPECT_THROW({
        try {
            l.append(DepositObserved{4, T0, acct("a"), outpoint(4), 1});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::SEQUENCE_NOT_MONOTONIC);
            throw;
        }
    }, LedgerError);
}

// ----- invariant §6.2.2: float safety / caps -----

TEST(VaultLedger, perDepositCapEnforced) {
    LedgerCaps caps;
    caps.per_deposit = 100;
    Ledger l{caps};
    AccountId a = acct("rich");
    l.append(DepositObserved{1, T0, a, outpoint(5), 200});
    EXPECT_THROW({
        try {
            l.append(CreditOpened{2, T0, a, outpoint(5), 200});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::OPEN_CREDITS_EXCEED_CAP);
            throw;
        }
    }, LedgerError);
}

TEST(VaultLedger, perUserCapEnforced) {
    LedgerCaps caps;
    caps.per_user = 100;
    Ledger l{caps};
    AccountId a = acct("cap");
    l.append(DepositObserved{1, T0, a, outpoint(6), 60});
    l.append(CreditOpened{2, T0, a, outpoint(6), 60});
    l.append(DepositObserved{3, T0, a, outpoint(7), 60});
    EXPECT_THROW({
        try {
            l.append(CreditOpened{4, T0, a, outpoint(7), 60});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::PER_USER_CAP_EXCEEDED);
            throw;
        }
    }, LedgerError);
}

TEST(VaultLedger, globalCapEnforced) {
    LedgerCaps caps;
    caps.global = 100;
    Ledger l{caps};
    l.append(DepositObserved{1, T0, acct("u1"), outpoint(8), 60});
    l.append(CreditOpened{2, T0, acct("u1"), outpoint(8), 60});
    l.append(DepositObserved{3, T0, acct("u2"), outpoint(9), 60});
    EXPECT_THROW({
        try {
            l.append(CreditOpened{4, T0, acct("u2"), outpoint(9), 60});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::OPEN_CREDITS_EXCEED_CAP);
            throw;
        }
    }, LedgerError);
}

// ----- invariant §6.2.3: no negative balance from reorg loss -----

TEST(VaultLedger, compensatingDebitProducesOperatorLossNotNegativeBalance) {
    Ledger l;
    AccountId a = acct("victim");
    OutpointId dep = outpoint(10);
    l.append(DepositObserved{1, T0, a, dep, 100});
    l.append(CreditOpened{2, T0, a, dep, 100});
    l.append(CreditSettled{3, T0, a, dep});
    l.append(WithdrawalInitiated{4, T0, a, outpoint(11), 100, backendId("test")});
    l.append(WithdrawalSettled{5, T0, a, outpoint(11)});
    l.append(CreditReverted{6, T0, a, dep});
    l.append(CompensatingDebit{7, T0, a, dep, 100, 100});

    auto account = l.accounts().at(a);
    EXPECT_EQ(account.spendable(), 0U);
    EXPECT_EQ(account.confirmed(), 0U);
    EXPECT_EQ(account.pending(), 0U);
    EXPECT_EQ(account.operatorLoss(), 100U);
    EXPECT_EQ(l.totalOperatorLoss(), 100U);
}

// ----- replay protection §5.4 -----

TEST(VaultLedger, reCreditAfterSettleRejected) {
    Ledger l;
    AccountId a = acct("u");
    OutpointId dep = outpoint(12);
    l.append(DepositObserved{1, T0, a, dep, 50});
    l.append(CreditOpened{2, T0, a, dep, 50});
    l.append(CreditSettled{3, T0, a, dep});
    EXPECT_THROW({
        try {
            l.append(CreditOpened{4, T0, a, dep, 50});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::DEPOSIT_LIFECYCLE_CLOSED);
            EXPECT_STREQ(e.what(), "settled");
            throw;
        }
    }, LedgerError);
}

TEST(VaultLedger, reCreditAfterRevertRejected) {
    Ledger l;
    AccountId a = acct("u");
    OutpointId dep = outpoint(13);
    l.append(DepositObserved{1, T0, a, dep, 50});
    l.append(CreditOpened{2, T0, a, dep, 50});
    l.append(CreditReverted{3, T0, a, dep});
    EXPECT_THROW({
        try {
            l.append(CreditOpened{4, T0, a, dep, 50});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::DEPOSIT_LIFECYCLE_CLOSED);
            throw;
        }
    }, LedgerError);
}

// ----- replay determinism -----

TEST(VaultLedger, replayIsDeterministic) {
    std::vector<LedgerEntry> entries{
        DepositObserved{1, T0, acct("alice"), outpoint(20), 100},
        CreditOpened{2, T0, acct("alice"), outpoint(20), 100},
        DepositObserved{3, T0, acct("bob"), outpoint(21), 50},
        CreditOpened{4, T0, acct("bob"), outpoint(21), 50},
        CreditSettled{5, T0, acct("alice"), outpoint(20)},
        WithdrawalInitiated{6, T0, acct("alice"), outpoint(22), 30, backendId("b1")},
    };
    Ledger l1 = Ledger::replay(entries);
    Ledger l2 = Ledger::replay(entries);
    EXPECT_EQ(l1.totalOpenCredits(), l2.totalOpenCredits());
    EXPECT_EQ(l1.totalOperatorLoss(), l2.totalOperatorLoss());
    EXPECT_EQ(l1.accounts().size(), l2.accounts().size());
    for (const auto& [k, v] : l1.accounts()) {
        ASSERT_TRUE(l2.accounts().count(k) != 0U);
        EXPECT_EQ(v, l2.accounts().at(k));
    }
}

// ----- withdrawal lifecycle -----

TEST(VaultLedger, withdrawalSettledDrainsConfirmedBeforePending) {
    Ledger l;
    AccountId a = acct("x");
    l.append(DepositObserved{1, T0, a, outpoint(30), 80});
    l.append(CreditOpened{2, T0, a, outpoint(30), 80});
    l.append(CreditSettled{3, T0, a, outpoint(30)});
    l.append(DepositObserved{4, T0, a, outpoint(31), 50});
    l.append(CreditOpened{5, T0, a, outpoint(31), 50});
    l.append(WithdrawalInitiated{6, T0, a, outpoint(32), 100, backendId("b")});
    l.append(WithdrawalSettled{7, T0, a, outpoint(32)});

    auto account = l.accounts().at(a);
    EXPECT_EQ(account.confirmed(), 0U);
    EXPECT_EQ(account.pending(), 30U);
    EXPECT_EQ(account.locked(), 0U);
    EXPECT_EQ(account.spendable(), 30U);
}

TEST(VaultLedger, withdrawalRevertedReturnsLockedToSpendable) {
    Ledger l;
    AccountId a = acct("y");
    l.append(DepositObserved{1, T0, a, outpoint(40), 100});
    l.append(CreditOpened{2, T0, a, outpoint(40), 100});
    l.append(CreditSettled{3, T0, a, outpoint(40)});
    l.append(WithdrawalInitiated{4, T0, a, outpoint(41), 60, backendId("b")});
    EXPECT_EQ(l.accounts().at(a).locked(), 60U);
    EXPECT_EQ(l.accounts().at(a).spendable(), 40U);
    l.append(WithdrawalReverted{5, T0, a, outpoint(41)});
    EXPECT_EQ(l.accounts().at(a).locked(), 0U);
    EXPECT_EQ(l.accounts().at(a).spendable(), 100U);
}

// ----- lifecycle inconsistencies rejected -----

TEST(VaultLedger, settleWithoutCreditRejected) {
    Ledger l;
    EXPECT_THROW({
        try {
            l.append(CreditSettled{1, T0, acct("ghost"), outpoint(50)});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::LIFECYCLE_INCONSISTENT);
            throw;
        }
    }, LedgerError);
}

TEST(VaultLedger, revertWithoutCreditRejected) {
    Ledger l;
    EXPECT_THROW({
        try {
            l.append(CreditReverted{1, T0, acct("ghost"), outpoint(51)});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::LIFECYCLE_INCONSISTENT);
            throw;
        }
    }, LedgerError);
}

TEST(VaultLedger, compensatingDebitWithoutRevertRejected) {
    Ledger l;
    AccountId a = acct("u");
    l.append(DepositObserved{1, T0, a, outpoint(52), 100});
    l.append(CreditOpened{2, T0, a, outpoint(52), 100});
    l.append(CreditSettled{3, T0, a, outpoint(52)});
    EXPECT_THROW({
        try {
            l.append(CompensatingDebit{4, T0, a, outpoint(52), 100, 0});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::LIFECYCLE_INCONSISTENT);
            throw;
        }
    }, LedgerError);
}

}  // namespace
}  // namespace dinero::vault::testing
