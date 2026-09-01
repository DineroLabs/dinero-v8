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
#include "vault/ledger_store.h"
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

// ----- internal transfer (account -> account, settled funds only) -----
//
// Transferable balance is min(spendable, confirmed): a user may move only
// money that has already settled on-chain. `pending` credits are
// operator-at-risk (a reorg lands a CompensatingDebit on the *original*
// account), so they must not be movable out from under that debit.

TEST(VaultLedger, internalTransferMovesConfirmedBetweenAccounts) {
    Ledger l;
    AccountId a = acct("alice");
    AccountId b = acct("bob");
    OutpointId dep = outpoint(60);
    l.append(DepositObserved{1, T0, a, dep, 100});
    l.append(CreditOpened{2, T0, a, dep, 100});
    l.append(CreditSettled{3, T0, a, dep});
    l.append(InternalTransfer{4, T0, a, b, 40});

    EXPECT_EQ(l.accounts().at(a).confirmed(), 60U);
    EXPECT_EQ(l.accounts().at(a).spendable(), 60U);
    EXPECT_EQ(l.accounts().at(b).confirmed(), 40U);
    EXPECT_EQ(l.accounts().at(b).spendable(), 40U);
    // Conservation: nothing entered or left the vault, and no credit
    // moved into or out of the operator-at-risk bucket.
    EXPECT_EQ(l.accounts().at(a).pending(), 0U);
    EXPECT_EQ(l.accounts().at(b).pending(), 0U);
    EXPECT_EQ(l.totalOpenCredits(), 0U);
    EXPECT_EQ(l.totalOperatorLoss(), 0U);
}

TEST(VaultLedger, internalTransferCannotMoveUnsettledCredits) {
    Ledger l;
    AccountId a = acct("alice");
    OutpointId dep = outpoint(61);
    l.append(DepositObserved{1, T0, a, dep, 100});
    l.append(CreditOpened{2, T0, a, dep, 100});
    // Spendable, but NOT transferable: the credit has not settled.
    ASSERT_EQ(l.accounts().at(a).spendable(), 100U);
    ASSERT_EQ(l.accounts().at(a).confirmed(), 0U);

    EXPECT_THROW({
        try {
            l.append(InternalTransfer{3, T0, a, acct("bob"), 1});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::INSUFFICIENT_TRANSFERABLE_BALANCE);
            throw;
        }
    }, LedgerError);
}

TEST(VaultLedger, internalTransferRespectsLockedBalance) {
    Ledger l;
    AccountId a = acct("alice");
    AccountId b = acct("bob");
    OutpointId dep = outpoint(62);
    l.append(DepositObserved{1, T0, a, dep, 100});
    l.append(CreditOpened{2, T0, a, dep, 100});
    l.append(CreditSettled{3, T0, a, dep});
    l.append(WithdrawalInitiated{4, T0, a, outpoint(63), 80, backendId("hot")});
    ASSERT_EQ(l.accounts().at(a).locked(), 80U);
    ASSERT_EQ(l.accounts().at(a).spendable(), 20U);

    // One una over the un-locked remainder is refused...
    EXPECT_THROW({
        try {
            l.append(InternalTransfer{5, T0, a, b, 21});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::INSUFFICIENT_TRANSFERABLE_BALANCE);
            throw;
        }
    }, LedgerError);
    // ...and the rejected append left the ledger untouched, seq included.
    EXPECT_EQ(l.nextSeq(), 5U);
    EXPECT_EQ(l.accounts().at(a).confirmed(), 100U);

    l.append(InternalTransfer{5, T0, a, b, 20});
    EXPECT_EQ(l.accounts().at(a).confirmed(), 80U);
    EXPECT_EQ(l.accounts().at(a).locked(), 80U);
    EXPECT_EQ(l.accounts().at(a).spendable(), 0U);
    EXPECT_EQ(l.accounts().at(b).confirmed(), 20U);
}

TEST(VaultLedger, internalTransferToSelfRejected) {
    Ledger l;
    AccountId a = acct("alice");
    OutpointId dep = outpoint(64);
    l.append(DepositObserved{1, T0, a, dep, 100});
    l.append(CreditOpened{2, T0, a, dep, 100});
    l.append(CreditSettled{3, T0, a, dep});
    EXPECT_THROW({
        try {
            l.append(InternalTransfer{4, T0, a, a, 10});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::TRANSFER_INVALID);
            throw;
        }
    }, LedgerError);
}

TEST(VaultLedger, internalTransferZeroAmountRejected) {
    Ledger l;
    AccountId a = acct("alice");
    OutpointId dep = outpoint(65);
    l.append(DepositObserved{1, T0, a, dep, 100});
    l.append(CreditOpened{2, T0, a, dep, 100});
    l.append(CreditSettled{3, T0, a, dep});
    EXPECT_THROW({
        try {
            l.append(InternalTransfer{4, T0, a, acct("bob"), 0});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::TRANSFER_INVALID);
            throw;
        }
    }, LedgerError);
}

TEST(VaultLedger, internalTransferEmptyCounterpartyRejected) {
    // An empty AccountId would mint a phantom account on replay.
    Ledger l;
    AccountId a = acct("alice");
    OutpointId dep = outpoint(66);
    l.append(DepositObserved{1, T0, a, dep, 100});
    l.append(CreditOpened{2, T0, a, dep, 100});
    l.append(CreditSettled{3, T0, a, dep});
    EXPECT_THROW({
        try {
            l.append(InternalTransfer{4, T0, a, acct(""), 10});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::TRANSFER_INVALID);
            throw;
        }
    }, LedgerError);
    EXPECT_THROW(l.append(InternalTransfer{4, T0, acct(""), a, 10}), LedgerError);
}

TEST(VaultLedger, internalTransferReplayIsDeterministic) {
    Ledger l;
    AccountId a = acct("alice");
    AccountId b = acct("bob");
    OutpointId dep = outpoint(67);
    l.append(DepositObserved{1, T0, a, dep, 500});
    l.append(CreditOpened{2, T0, a, dep, 500});
    l.append(CreditSettled{3, T0, a, dep});
    l.append(InternalTransfer{4, T0, a, b, 125});
    l.append(InternalTransfer{5, T0, b, a, 25});

    Ledger replayed = Ledger::replay(l.entries());
    EXPECT_EQ(replayed.accounts().at(a), l.accounts().at(a));
    EXPECT_EQ(replayed.accounts().at(b), l.accounts().at(b));
    EXPECT_EQ(replayed.accounts().at(a).confirmed(), 400U);
    EXPECT_EQ(replayed.accounts().at(b).confirmed(), 100U);
    EXPECT_EQ(replayed.nextSeq(), l.nextSeq());
}

TEST(VaultLedger, internalTransferDoesNotConsumePerUserCap) {
    // Caps bound OUTSTANDING credits (credit_opened not yet settled).
    // A transfer moves settled funds only, so it must be cap-neutral even
    // when the receiver is already at the per-user cap.
    LedgerCaps caps;
    caps.per_user = 100;
    Ledger l{caps};
    AccountId a = acct("alice");
    AccountId b = acct("bob");
    OutpointId dep_a = outpoint(68);
    OutpointId dep_b = outpoint(69);
    l.append(DepositObserved{1, T0, a, dep_a, 100});
    l.append(CreditOpened{2, T0, a, dep_a, 100});
    l.append(CreditSettled{3, T0, a, dep_a});
    l.append(DepositObserved{4, T0, b, dep_b, 100});
    l.append(CreditOpened{5, T0, b, dep_b, 100});  // bob at per-user cap
    l.append(CreditSettled{6, T0, b, dep_b});

    l.append(InternalTransfer{7, T0, a, b, 100});
    EXPECT_EQ(l.accounts().at(b).confirmed(), 200U);
    EXPECT_EQ(l.accounts().at(a).confirmed(), 0U);
}

// ----- write-ahead persistence tee -----

TEST(VaultLedger, appendWithoutStoreIsANoOp) {
    // Default construction must stay store-less: every existing caller
    // (and every other test in this file) relies on it.
    Ledger l;
    l.append(DepositObserved{0, T0, acct("alice"), outpoint(80), 10});
    EXPECT_EQ(l.entries().size(), 1U);
}

TEST(VaultLedger, appendTeesEveryEntryToTheStore) {
    InMemoryLedgerStore store;
    Ledger l{LedgerCaps::unbounded(), &store};
    AccountId a = acct("alice");
    OutpointId dep = outpoint(81);
    l.append(DepositObserved{0, T0, a, dep, 100});
    l.append(CreditOpened{1, T0, a, dep, 100});
    l.append(CreditSettled{2, T0, a, dep});
    l.append(InternalTransfer{3, T0, a, acct("bob"), 40});

    auto persisted = store.loadAll();
    ASSERT_EQ(persisted.size(), 4U);
    EXPECT_NE(std::get_if<InternalTransfer>(&persisted[3]), nullptr);
    // Replaying what was persisted reproduces the live balances.
    Ledger replayed = Ledger::replay(persisted);
    EXPECT_EQ(replayed.accountOr(a).confirmed(), 60U);
    EXPECT_EQ(replayed.accountOr(acct("bob")).confirmed(), 40U);
}

TEST(VaultLedger, rejectedAppendIsNeverPersisted) {
    // Write-ahead only past validation: a refused entry must leave no
    // trace on disk, or replay would resurrect it as valid.
    InMemoryLedgerStore store;
    Ledger l{LedgerCaps::unbounded(), &store};
    AccountId a = acct("alice");
    OutpointId dep = outpoint(82);
    l.append(DepositObserved{0, T0, a, dep, 100});
    l.append(CreditOpened{1, T0, a, dep, 100});
    l.append(CreditSettled{2, T0, a, dep});
    ASSERT_EQ(store.loadAll().size(), 3U);

    EXPECT_THROW(l.append(InternalTransfer{3, T0, a, acct("bob"), 999}), LedgerError);
    EXPECT_EQ(store.loadAll().size(), 3U);
    EXPECT_THROW(l.append(CreditSettled{3, T0, acct("ghost"), outpoint(83)}), LedgerError);
    EXPECT_EQ(store.loadAll().size(), 3U);
}

TEST(VaultLedger, replayDoesNotReAppendToTheStore) {
    // The restart trap: if the store is attached before replay, every
    // restart doubles the log.
    InMemoryLedgerStore store;
    Ledger l{LedgerCaps::unbounded(), &store};
    l.append(DepositObserved{0, T0, acct("alice"), outpoint(84), 100});
    ASSERT_EQ(store.loadAll().size(), 1U);

    Ledger restored = Ledger::replay(store.loadAll());
    restored.setStore(&store);
    EXPECT_EQ(store.loadAll().size(), 1U);
    // Post-attach writes still land.
    restored.append(DepositObserved{1, T0, acct("bob"), outpoint(85), 5});
    EXPECT_EQ(store.loadAll().size(), 2U);
}

// ----- withdrawal broadcast record -----
//
// WithdrawalInitiated is now written BEFORE the broadcast (it reserves
// `locked`), so it can no longer carry the txid. This entry binds the
// txid afterwards, and its presence is what distinguishes "reserved but
// never broadcast" from "coins are on the wire".

TEST(VaultLedger, withdrawalBroadcastRecordedDoesNotMoveBalance) {
    Ledger l;
    AccountId a = acct("alice");
    OutpointId dep = outpoint(90);
    OutpointId req = outpoint(91);
    l.append(DepositObserved{0, T0, a, dep, 500});
    l.append(CreditOpened{1, T0, a, dep, 500});
    l.append(CreditSettled{2, T0, a, dep});
    l.append(WithdrawalInitiated{3, T0, a, req, 200, backendId("hot")});
    ASSERT_EQ(l.accounts().at(a).locked(), 200U);

    std::array<uint8_t, 32> txid{};
    txid.fill(0xbe);
    l.append(WithdrawalBroadcastRecorded{4, T0, a, req, txid});

    EXPECT_EQ(l.accounts().at(a).locked(), 200U);
    EXPECT_EQ(l.accounts().at(a).confirmed(), 500U);
    EXPECT_EQ(l.accounts().at(a).spendable(), 300U);
}

TEST(VaultLedger, withdrawalBroadcastRecordedRequiresInitiated) {
    Ledger l;
    std::array<uint8_t, 32> txid{};
    txid.fill(0xbe);
    EXPECT_THROW({
        try {
            l.append(WithdrawalBroadcastRecorded{0, T0, acct("ghost"), outpoint(92), txid});
        } catch (const LedgerError& e) {
            EXPECT_EQ(e.kind(), LedgerError::Kind::LIFECYCLE_INCONSISTENT);
            throw;
        }
    }, LedgerError);
}

TEST(VaultLedger, withdrawalSettlesAfterBroadcastRecord) {
    // Full re-keyed lifecycle: initiated (reserve) -> broadcast -> settled.
    Ledger l;
    AccountId a = acct("alice");
    OutpointId dep = outpoint(93);
    OutpointId req = outpoint(94);
    l.append(DepositObserved{0, T0, a, dep, 500});
    l.append(CreditOpened{1, T0, a, dep, 500});
    l.append(CreditSettled{2, T0, a, dep});
    l.append(WithdrawalInitiated{3, T0, a, req, 200, backendId("hot")});
    std::array<uint8_t, 32> txid{};
    txid.fill(0xbe);
    l.append(WithdrawalBroadcastRecorded{4, T0, a, req, txid});
    l.append(WithdrawalSettled{5, T0, a, req});

    EXPECT_EQ(l.accounts().at(a).locked(), 0U);
    EXPECT_EQ(l.accounts().at(a).confirmed(), 300U);
}

TEST(VaultLedger, reservationRevertedWithoutBroadcastReturnsTheBalance) {
    // The new failure path: reserve, broadcast fails, release.
    Ledger l;
    AccountId a = acct("alice");
    OutpointId dep = outpoint(95);
    OutpointId req = outpoint(96);
    l.append(DepositObserved{0, T0, a, dep, 500});
    l.append(CreditOpened{1, T0, a, dep, 500});
    l.append(CreditSettled{2, T0, a, dep});
    l.append(WithdrawalInitiated{3, T0, a, req, 200, backendId("hot")});
    ASSERT_EQ(l.accounts().at(a).spendable(), 300U);
    l.append(WithdrawalReverted{4, T0, a, req});
    EXPECT_EQ(l.accounts().at(a).locked(), 0U);
    EXPECT_EQ(l.accounts().at(a).spendable(), 500U);
}

}  // namespace
}  // namespace dinero::vault::testing
