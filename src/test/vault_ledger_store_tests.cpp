// Copyright (c) 2026 Dinero Labs.
//
// Daemon-side gtest for FileLedgerStore. Verifies round-trip
// persistence for every LedgerEntry variant + a full Ledger.replay
// against the loaded entries.

#include <gtest/gtest.h>

#include "vault/ledger.h"
#include "vault/ledger_account.h"
#include "vault/ledger_entry.h"
#include "vault/ledger_store.h"
#include "vault/vault_types.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define DINERO_GETPID _getpid
#else
#include <unistd.h>
#define DINERO_GETPID getpid
#endif

namespace dinero::vault::testing {
namespace {

std::string tempPath() {
    // Why pid+timestamp+counter instead of std::rand(): std::rand() is never
    // seeded here, so on MSVC it produces the same fixed sequence every run
    // and prior failed-cleanup files in %TEMP% poison subsequent runs. The
    // pid+timestamp+counter triplet guarantees uniqueness across processes
    // and across reruns of the same binary.
    static std::atomic<uint64_t> counter{0};
    std::filesystem::path tmp = std::filesystem::temp_directory_path();
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "vault-ledger-%d-%lld-%llu.jsonl",
                  static_cast<int>(DINERO_GETPID()),
                  static_cast<long long>(now),
                  static_cast<unsigned long long>(counter.fetch_add(1)));
    return (tmp / buf).string();
}

OutpointId outpoint(uint8_t tag, uint32_t vout = 0) {
    OutpointId op;
    op.txid_raw.fill(tag);
    op.vout = vout;
    return op;
}

constexpr LedgerTimestamp T0 = 1'776'500'000'000'000'000;

TEST(VaultLedgerStore, roundTripsAllNineEntryTypes) {
    std::string path = tempPath();
    {
        FileLedgerStore store{path};
        store.append(DepositObserved{1, T0, AccountId{"alice"}, outpoint(1), 100});
        store.append(CreditOpened{2, T0, AccountId{"alice"}, outpoint(1), 100});
        store.append(CreditSettled{3, T0, AccountId{"alice"}, outpoint(1)});
        store.append(CreditReverted{4, T0, AccountId{"bob"}, outpoint(2)});
        store.append(WithdrawalInitiated{5, T0, AccountId{"alice"}, outpoint(3), 50, BackendId{"hsm"}});
        store.append(WithdrawalSettled{6, T0, AccountId{"alice"}, outpoint(3)});
        store.append(WithdrawalReverted{7, T0, AccountId{"alice"}, outpoint(4)});
        store.append(CompensatingDebit{8, T0, AccountId{"carol"}, outpoint(5), 100, 25});
        store.append(PolicyAdjustment{9, T0, AccountId{"dana"}, "audit-cleanup", -10, 0});
    }
    // Reload must be scoped so its file handle releases before remove() below;
    // Windows refuses to delete an open file, whereas POSIX unlink() does not.
    std::vector<LedgerEntry> entries;
    {
        FileLedgerStore reload{path};
        entries = reload.loadAll();
    }
    ASSERT_EQ(entries.size(), 9U);

    EXPECT_TRUE(std::holds_alternative<DepositObserved>(entries[0]));
    EXPECT_TRUE(std::holds_alternative<CreditOpened>(entries[1]));
    EXPECT_TRUE(std::holds_alternative<CreditSettled>(entries[2]));
    EXPECT_TRUE(std::holds_alternative<CreditReverted>(entries[3]));
    EXPECT_TRUE(std::holds_alternative<WithdrawalInitiated>(entries[4]));
    EXPECT_TRUE(std::holds_alternative<WithdrawalSettled>(entries[5]));
    EXPECT_TRUE(std::holds_alternative<WithdrawalReverted>(entries[6]));
    EXPECT_TRUE(std::holds_alternative<CompensatingDebit>(entries[7]));
    EXPECT_TRUE(std::holds_alternative<PolicyAdjustment>(entries[8]));

    EXPECT_EQ(std::get<DepositObserved>(entries[0]).account.raw, "alice");
    EXPECT_EQ(std::get<DepositObserved>(entries[0]).amount, 100U);
    EXPECT_EQ(std::get<WithdrawalInitiated>(entries[4]).backend.raw, "hsm");
    EXPECT_EQ(std::get<CompensatingDebit>(entries[7]).operatorLoss, 25U);
    EXPECT_EQ(std::get<PolicyAdjustment>(entries[8]).note, "audit-cleanup");
    EXPECT_EQ(std::get<PolicyAdjustment>(entries[8]).deltaUserBalance, -10);

    std::filesystem::remove(path);
}

TEST(VaultLedgerStore, replaysIntoLiveLedger) {
    std::string path = tempPath();
    {
        FileLedgerStore store{path};
        // A complete deposit→credit→settle cycle.
        store.append(DepositObserved{1, T0, AccountId{"u1"}, outpoint(0xA), 500});
        store.append(CreditOpened{2, T0, AccountId{"u1"}, outpoint(0xA), 500});
        store.append(CreditSettled{3, T0, AccountId{"u1"}, outpoint(0xA)});
    }
    std::vector<LedgerEntry> entries;
    {
        FileLedgerStore reload{path};
        entries = reload.loadAll();
    }
    Ledger l = Ledger::replay(entries);
    EXPECT_EQ(l.entries().size(), 3U);
    EXPECT_EQ(l.accountOr(AccountId{"u1"}).confirmed(), 500U);
    EXPECT_EQ(l.accountOr(AccountId{"u1"}).spendable(), 500U);
    EXPECT_EQ(l.totalOpenCredits(), 0U);
    std::filesystem::remove(path);
}

TEST(VaultLedgerStore, appendThenReloadPreservesOrder) {
    std::string path = tempPath();
    {
        FileLedgerStore store{path};
        for (uint64_t i = 1; i <= 50; ++i) {
            store.append(DepositObserved{i, T0 + static_cast<LedgerTimestamp>(i),
                                         AccountId{"acct-" + std::to_string(i)},
                                         outpoint(static_cast<uint8_t>(i)), i * 10});
        }
        store.flush();
    }

    std::vector<LedgerEntry> entries;
    {
        FileLedgerStore reload{path};
        entries = reload.loadAll();
    }
    ASSERT_EQ(entries.size(), 50U);
    for (size_t i = 0; i < 50; ++i) {
        const auto& e = std::get<DepositObserved>(entries[i]);
        EXPECT_EQ(e.seq, i + 1);
        EXPECT_EQ(e.amount, (i + 1) * 10);
    }
    std::filesystem::remove(path);
}

TEST(VaultLedgerStore, inMemoryStoreIsEphemeral) {
    InMemoryLedgerStore store;
    store.append(DepositObserved{1, T0, AccountId{"x"}, outpoint(0x99), 1});
    EXPECT_EQ(store.loadAll().size(), 1U);
}

}  // namespace
}  // namespace dinero::vault::testing
