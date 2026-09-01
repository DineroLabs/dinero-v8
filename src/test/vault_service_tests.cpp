// Copyright (c) 2026 Dinero Labs.
//
// Daemon-side gtest port of the Swift VaultServiceTests. Verifies
// the orchestrator wires Ledger + DepositFlow + ReorgWatcher +
// WithdrawalQueue + SigningBackend correctly under the same
// chain-event sequencing the daemon will produce in production.

#include <gtest/gtest.h>

#include "vault/ledger.h"
#include "vault/ledger_store.h"
#include "vault/ledger_entry.h"
#include "vault/signing_backend.h"
#include "vault/vault_service.h"
#include "vault/vault_types.h"
#include "vault/withdrawal_queue.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace dinero::vault::testing {
namespace {

AccountId acct(const std::string& name) { return AccountId{name}; }
std::array<uint8_t, 32> txid(uint8_t tag) {
    std::array<uint8_t, 32> a{};
    a.fill(tag);
    return a;
}
std::array<uint8_t, 32> bhash(uint8_t tag) {
    std::array<uint8_t, 32> a{};
    a.fill(tag);
    return a;
}

class MiniChain {
   public:
    void setBlock(uint64_t height, const std::array<uint8_t, 32>& hash,
                  const std::vector<OutpointId>& txids) {
        hashes_[height] = hash;
        std::string key{reinterpret_cast<const char*>(hash.data()), hash.size()};
        for (const auto& op : txids) {
            txids_per_block_[key].insert(opKeyOf(op));
        }
    }
    void reorg(uint64_t height, const std::array<uint8_t, 32>& new_hash,
               const std::vector<OutpointId>& txids) {
        hashes_[height] = new_hash;
        std::string key{reinterpret_cast<const char*>(new_hash.data()), new_hash.size()};
        std::set<std::string> set;
        for (const auto& op : txids) {
            set.insert(opKeyOf(op));
        }
        txids_per_block_[key] = set;
    }
    [[nodiscard]] std::array<uint8_t, 32> headerHash(uint64_t height) const {
        auto it = hashes_.find(height);
        if (it == hashes_.end()) {
            return std::array<uint8_t, 32>{};
        }
        return it->second;
    }
    [[nodiscard]] bool contains(const OutpointId& op, const std::array<uint8_t, 32>& hash) const {
        std::string key{reinterpret_cast<const char*>(hash.data()), hash.size()};
        auto it = txids_per_block_.find(key);
        if (it == txids_per_block_.end()) {
            return false;
        }
        return it->second.count(opKeyOf(op)) != 0U;
    }

   private:
    static std::string opKeyOf(const OutpointId& op) {
        std::string s;
        char buf[3];
        for (auto byte : op.txid_raw) {
            std::snprintf(buf, sizeof(buf), "%02x", static_cast<int>(byte));
            s += buf;
        }
        s += ":";
        s += std::to_string(op.vout);
        return s;
    }
    std::unordered_map<uint64_t, std::array<uint8_t, 32>> hashes_;
    std::unordered_map<std::string, std::set<std::string>> txids_per_block_;
};

std::unique_ptr<VaultService> makeService(MiniChain* chain, VaultServiceConfig cfg = {},
                                         LedgerStore* store = nullptr) {
    auto backend = std::make_unique<InMemorySigningBackend>(BackendId{"test"}, 1'000'000'000);
    return std::make_unique<VaultService>(
        std::move(backend), cfg,
        [chain](uint64_t h) { return chain->headerHash(h); },
        [chain](const OutpointId& op, uint64_t /*h*/, const std::array<uint8_t, 32>& hh) {
            return chain->contains(op, hh);
        },
        store);
}

WithdrawalId makeWid(uint8_t seed) {
    WithdrawalId w{};
    w.fill(seed);
    return w;
}

std::string tempLedgerPath() {
    static std::atomic<uint64_t> counter{0};
    std::filesystem::path tmp = std::filesystem::temp_directory_path();
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "vault-service-%lld-%llu.jsonl",
                  static_cast<long long>(now),
                  static_cast<unsigned long long>(counter.fetch_add(1)));
    return (tmp / buf).string();
}

size_t countLines(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    size_t n = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            ++n;
        }
    }
    return n;
}

TEST(VaultService, recordDepositFlowsThroughToCreditAndSettleAtKSettle) {
    MiniChain chain;
    auto svc = makeService(&chain);
    std::array<uint8_t, 32> tx = txid(0x10);
    OutpointId op;
    op.txid_raw = tx;
    op.vout = 0;
    chain.setBlock(100, bhash(0xAA), {op});
    svc->recordDeposit(tx, 0, acct("alice"), 100, 100, bhash(0xAA));
    svc->tipChanged(101);
    EXPECT_EQ(svc->totalOpenCredits(), 100U);
    EXPECT_EQ(svc->accountSpendable(acct("alice")), 100U);
    svc->tipChanged(105);
    EXPECT_EQ(svc->totalOpenCredits(), 0U);
    EXPECT_EQ(svc->accountConfirmed(acct("alice")), 100U);
}

TEST(VaultService, withdrawalEnqueueAndSettlement) {
    MiniChain chain;
    auto svc = makeService(&chain);
    OutpointId op;
    op.txid_raw = txid(0x20);
    op.vout = 0;
    chain.setBlock(200, bhash(0xBB), {op});
    svc->recordDeposit(txid(0x20), 0, acct("bob"), 1000, 200, bhash(0xBB));
    svc->tipChanged(205);
    EXPECT_EQ(svc->accountConfirmed(acct("bob")), 1000U);

    std::vector<uint8_t> dest{0x51, 0x20};
    for (int i = 0; i < 32; ++i) {
        dest.push_back(0xab);
    }
    WithdrawalId id = svc->enqueueWithdrawal(acct("bob"), 300, dest);
    auto processed = svc->processNextWithdrawal();
    ASSERT_TRUE(processed.has_value());
    EXPECT_EQ(*processed, id);
    EXPECT_EQ(svc->accountLocked(acct("bob")), 300U);
    svc->markWithdrawalIncluded(id, 210);
    svc->tipChanged(211);  // 2 confs → settle
    EXPECT_EQ(svc->accountLocked(acct("bob")), 0U);
    EXPECT_EQ(svc->accountConfirmed(acct("bob")), 700U);
}

TEST(VaultService, reorgEvictionRevertsCredit) {
    MiniChain chain;
    auto svc = makeService(&chain);
    OutpointId op;
    op.txid_raw = txid(0x30);
    op.vout = 0;
    chain.setBlock(300, bhash(0xCC), {op});
    svc->recordDeposit(txid(0x30), 0, acct("carol"), 200, 300, bhash(0xCC));
    svc->tipChanged(301);
    EXPECT_EQ(svc->totalOpenCredits(), 200U);
    chain.reorg(300, bhash(0xDD), {});  // tx evicted
    svc->tipChanged(302);
    EXPECT_EQ(svc->totalOpenCredits(), 0U);
}

TEST(VaultService, metricsReflectLiveState) {
    MiniChain chain;
    auto svc = makeService(&chain);
    EXPECT_EQ(svc->totalOpenCredits(), 0U);
    EXPECT_EQ(svc->accountCount(), 0U);
    OutpointId op;
    op.txid_raw = txid(0x40);
    op.vout = 0;
    chain.setBlock(400, bhash(0xEE), {op});
    svc->recordDeposit(txid(0x40), 0, acct("dana"), 50, 400, bhash(0xEE));
    svc->tipChanged(401);
    EXPECT_GT(svc->ledgerNextSeq(), 0U);
    EXPECT_EQ(svc->accountCount(), 1U);
}

TEST(VaultService, shadowModeNeverOpensCredit) {
    MiniChain chain;
    VaultServiceConfig cfg;
    cfg.shadow_mode = true;
    auto svc = makeService(&chain, cfg);
    OutpointId op;
    op.txid_raw = txid(0x50);
    op.vout = 0;
    chain.setBlock(500, bhash(0xFF), {op});
    svc->recordDeposit(txid(0x50), 0, acct("eve"), 100, 500, bhash(0xFF));
    svc->tipChanged(505);
    EXPECT_EQ(svc->totalOpenCredits(), 0U);
    // depositObserved should still be in the ledger.
    EXPECT_GE(svc->ledgerNextSeq(), 1U);
}

TEST(VaultService, transferMovesSettledBalanceBetweenAccounts) {
    MiniChain chain;
    auto svc = makeService(&chain);
    OutpointId op;
    op.txid_raw = txid(0x60);
    op.vout = 0;
    chain.setBlock(600, bhash(0x11), {op});
    svc->recordDeposit(txid(0x60), 0, acct("alice"), 1000, 600, bhash(0x11));
    svc->tipChanged(605);
    ASSERT_EQ(svc->accountConfirmed(acct("alice")), 1000U);
    uint64_t seq_before = svc->ledgerNextSeq();

    LedgerSeq seq = svc->transfer(acct("alice"), acct("bob"), 250);

    EXPECT_EQ(seq, seq_before);
    EXPECT_EQ(svc->ledgerNextSeq(), seq_before + 1);
    EXPECT_EQ(svc->accountConfirmed(acct("alice")), 750U);
    EXPECT_EQ(svc->accountSpendable(acct("alice")), 750U);
    EXPECT_EQ(svc->accountConfirmed(acct("bob")), 250U);
    EXPECT_EQ(svc->accountSpendable(acct("bob")), 250U);
    EXPECT_EQ(svc->accountCount(), 2U);
}

TEST(VaultService, transferRejectsUnsettledBalance) {
    MiniChain chain;
    auto svc = makeService(&chain);
    OutpointId op;
    op.txid_raw = txid(0x61);
    op.vout = 0;
    chain.setBlock(700, bhash(0x12), {op});
    svc->recordDeposit(txid(0x61), 0, acct("carol"), 500, 700, bhash(0x12));
    svc->tipChanged(701);  // credit opened, not yet settled
    ASSERT_EQ(svc->accountSpendable(acct("carol")), 500U);
    ASSERT_EQ(svc->accountConfirmed(acct("carol")), 0U);

    EXPECT_THROW(svc->transfer(acct("carol"), acct("dave"), 1), LedgerError);
    // The rejected transfer must not have conjured the destination account.
    EXPECT_EQ(svc->accountCount(), 1U);
    EXPECT_EQ(svc->accountSpendable(acct("carol")), 500U);
}

TEST(VaultService, transferIsRecordedInTheLedgerLog) {
    MiniChain chain;
    auto svc = makeService(&chain);
    OutpointId op;
    op.txid_raw = txid(0x62);
    op.vout = 0;
    chain.setBlock(800, bhash(0x13), {op});
    svc->recordDeposit(txid(0x62), 0, acct("erin"), 400, 800, bhash(0x13));
    svc->tipChanged(805);
    LedgerSeq seq = svc->transfer(acct("erin"), acct("frank"), 100);

    auto entries = svc->entriesSince(seq);
    ASSERT_FALSE(entries.empty());
    const auto* transfer = std::get_if<InternalTransfer>(&entries.front());
    ASSERT_NE(transfer, nullptr);
    EXPECT_EQ(transfer->from.raw, "erin");
    EXPECT_EQ(transfer->to.raw, "frank");
    EXPECT_EQ(transfer->amount, 100U);
    EXPECT_GT(transfer->at, 0);
}

// Characterisation test (written after the fact, documenting behaviour
// rather than driving it): what happens when settled funds are moved out
// and a DIFFERENT, still-pending deposit is then orphaned.
TEST(VaultService, transferOutOfSettledFundsShiftsReorgLossAccounting) {
    MiniChain chain;
    auto svc = makeService(&chain);
    OutpointId dep_a;
    dep_a.txid_raw = txid(0x70);
    dep_a.vout = 0;
    chain.setBlock(900, bhash(0x20), {dep_a});
    svc->recordDeposit(txid(0x70), 0, acct("gina"), 100, 900, bhash(0x20));
    svc->tipChanged(906);  // deposit A settles
    ASSERT_EQ(svc->accountConfirmed(acct("gina")), 100U);

    OutpointId dep_b;
    dep_b.txid_raw = txid(0x71);
    dep_b.vout = 0;
    chain.setBlock(910, bhash(0x21), {dep_b});
    svc->recordDeposit(txid(0x71), 0, acct("gina"), 50, 910, bhash(0x21));
    svc->tipChanged(912);  // deposit B credited, still pending
    ASSERT_EQ(svc->accountPending(acct("gina")), 50U);
    ASSERT_EQ(svc->accountTransferable(acct("gina")), 100U);  // A only, not B

    svc->transfer(acct("gina"), acct("hank"), 100);
    chain.reorg(910, bhash(0x22), {});  // deposit B orphaned
    svc->tipChanged(913);

    // The at-risk credit itself was never movable, so the revert unwinds
    // exactly what it credited.
    EXPECT_EQ(svc->accountPending(acct("gina")), 0U);
    EXPECT_EQ(svc->accountConfirmed(acct("gina")), 0U);
    // hank keeps the transferred funds, which are backed by deposit A —
    // the vault as a whole is still fully backed.
    EXPECT_EQ(svc->accountConfirmed(acct("hank")), 100U);
    // But ReorgWatcher::unrecoverableLoss measures what the ORIGINATING
    // ACCOUNT can absorb, so moving settled funds out first books the
    // reverted amount as operator loss. This is the same over-reporting a
    // withdrawal produces (any outflow shrinks the absorbing balance); it
    // is a property of that heuristic, not of the transfer itself.
    EXPECT_EQ(svc->accountOperatorLoss(acct("gina")), 50U);
}

// ----- restart durability -----

TEST(VaultService, settledBalancesAndTransfersSurviveRestart) {
    MiniChain chain;
    const std::string path = tempLedgerPath();
    {
        FileLedgerStore store{path};
        auto svc = makeService(&chain, {}, &store);
        OutpointId op;
        op.txid_raw = txid(0x80);
        op.vout = 0;
        chain.setBlock(1000, bhash(0x30), {op});
        svc->recordDeposit(txid(0x80), 0, acct("ivy"), 800, 1000, bhash(0x30));
        svc->tipChanged(1006);
        ASSERT_EQ(svc->accountConfirmed(acct("ivy")), 800U);
        svc->transfer(acct("ivy"), acct("jack"), 300);
        ASSERT_EQ(svc->accountConfirmed(acct("jack")), 300U);
    }
    const size_t lines_after_first_run = countLines(path);
    ASSERT_GT(lines_after_first_run, 0U);

    {
        FileLedgerStore store{path};
        auto svc = makeService(&chain, {}, &store);
        EXPECT_EQ(svc->accountConfirmed(acct("ivy")), 500U);
        EXPECT_EQ(svc->accountConfirmed(acct("jack")), 300U);
        EXPECT_EQ(svc->accountSpendable(acct("jack")), 300U);
        EXPECT_EQ(svc->accountCount(), 2U);
        // Replay must not re-persist: a restart that doubles the log
        // corrupts every subsequent restart.
        EXPECT_EQ(countLines(path), lines_after_first_run);
        // A post-restart write still lands, and continues the sequence.
        svc->transfer(acct("jack"), acct("ivy"), 100);
        EXPECT_EQ(countLines(path), lines_after_first_run + 1);
    }

    {
        FileLedgerStore store{path};
        auto svc = makeService(&chain, {}, &store);
        EXPECT_EQ(svc->accountConfirmed(acct("ivy")), 600U);
        EXPECT_EQ(svc->accountConfirmed(acct("jack")), 200U);
    }
    std::filesystem::remove(path);
}

TEST(VaultService, replayCountsInFlightWorkThatLostItsStateMachine) {
    // Balances replay from the ledger, but DepositFlowMachine /
    // WithdrawalQueue / ReorgWatcher state does NOT — their inputs
    // (block hash, deposit height, destination script) are not ledgered.
    // So anything mid-lifecycle at shutdown comes back frozen. These
    // counters are what makes that visible instead of silent.
    MiniChain chain;
    const std::string path = tempLedgerPath();
    {
        FileLedgerStore store{path};
        auto svc = makeService(&chain, {}, &store);
        OutpointId settled_op;
        settled_op.txid_raw = txid(0x81);
        settled_op.vout = 0;
        chain.setBlock(1100, bhash(0x31), {settled_op});
        svc->recordDeposit(txid(0x81), 0, acct("kim"), 1000, 1100, bhash(0x31));
        svc->tipChanged(1106);  // settles

        std::vector<uint8_t> dest{0x51, 0x20};
        for (int i = 0; i < 32; ++i) {
            dest.push_back(0xcd);
        }
        svc->enqueueWithdrawal(acct("kim"), 200, dest);
        svc->processNextWithdrawal();  // initiated, not settled
        ASSERT_EQ(svc->accountLocked(acct("kim")), 200U);

        OutpointId pending_op;
        pending_op.txid_raw = txid(0x82);
        pending_op.vout = 0;
        chain.setBlock(1110, bhash(0x32), {pending_op});
        svc->recordDeposit(txid(0x82), 0, acct("kim"), 50, 1110, bhash(0x32));
        svc->tipChanged(1112);  // credited, not settled
        ASSERT_EQ(svc->accountPending(acct("kim")), 50U);
    }

    FileLedgerStore store{path};
    auto svc = makeService(&chain, {}, &store);
    // An initiated-but-unsettled withdrawal reserves via `locked`; it does
    // not reduce `confirmed` until it settles. So the frozen withdrawal
    // holds 200 out of reach indefinitely after the restart.
    EXPECT_EQ(svc->accountConfirmed(acct("kim")), 1000U);
    EXPECT_EQ(svc->accountPending(acct("kim")), 50U);
    EXPECT_EQ(svc->accountLocked(acct("kim")), 200U);
    EXPECT_EQ(svc->accountSpendable(acct("kim")), 850U);
    // One deposit credited-not-settled, and one withdrawal that really was
    // broadcast (processNextWithdrawal ran) so its coins are on the wire.
    EXPECT_EQ(svc->unreconciledDeposits(), 1U);
    EXPECT_EQ(svc->withdrawalsBroadcastNotSettled(), 1U);
    EXPECT_EQ(svc->withdrawalsReservedNotBroadcast(), 0U);
    std::filesystem::remove(path);
}

TEST(VaultService, freshServiceReportsNothingUnreconciled) {
    MiniChain chain;
    auto svc = makeService(&chain);
    EXPECT_EQ(svc->unreconciledDeposits(), 0U);
    EXPECT_EQ(svc->withdrawalsReservedNotBroadcast(), 0U);
    EXPECT_EQ(svc->withdrawalsBroadcastNotSettled(), 0U);
}

TEST(VaultService, replaySeparatesReservedFromActuallyBroadcastWithdrawals) {
    // The distinction that makes a frozen withdrawal actionable: one was
    // reserved and never sent (safe to release), the other's coins are on
    // the wire (must NOT be released). Only the broadcast record tells
    // them apart, so this drives the log straight into a fresh service.
    MiniChain chain;
    const std::string path = tempLedgerPath();
    OutpointId dep;
    dep.txid_raw = txid(0x90);
    OutpointId req_reserved = outpointForWithdrawalRequest(makeWid(0xA1));
    OutpointId req_broadcast = outpointForWithdrawalRequest(makeWid(0xA2));
    std::array<uint8_t, 32> sent_txid{};
    sent_txid.fill(0xbe);
    {
        FileLedgerStore store{path};
        store.append(DepositObserved{0, 1, acct("nina"), dep, 1000});
        store.append(CreditOpened{1, 1, acct("nina"), dep, 1000});
        store.append(CreditSettled{2, 1, acct("nina"), dep});
        store.append(WithdrawalInitiated{3, 1, acct("nina"), req_reserved, 200, BackendId{"hot"}});
        store.append(WithdrawalInitiated{4, 1, acct("nina"), req_broadcast, 300, BackendId{"hot"}});
        store.append(
            WithdrawalBroadcastRecorded{5, 1, acct("nina"), req_broadcast, sent_txid});
    }

    FileLedgerStore store{path};
    auto svc = makeService(&chain, {}, &store);
    EXPECT_EQ(svc->accountLocked(acct("nina")), 500U);
    EXPECT_EQ(svc->withdrawalsReservedNotBroadcast(), 1U);
    EXPECT_EQ(svc->withdrawalsBroadcastNotSettled(), 1U);
    std::filesystem::remove(path);
}

}  // namespace
}  // namespace dinero::vault::testing
