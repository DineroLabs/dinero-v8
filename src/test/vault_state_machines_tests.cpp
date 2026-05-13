// Copyright (c) 2026 Dinero Labs.
//
// Daemon-side gtest port of the three Swift state-machine tests:
//   - DepositFlowMachineTests.swift  (14 cases)
//   - ReorgWatcherTests.swift        (8 cases)
//   - WithdrawalQueueTests.swift     (15 cases)
//
// Behavioural-parity gate for C.2.

#include <gtest/gtest.h>

#include "vault/deposit_flow.h"
#include "vault/ledger.h"
#include "vault/ledger_account.h"
#include "vault/reorg_watcher.h"
#include "vault/signing_backend.h"
#include "vault/vault_types.h"
#include "vault/withdrawal_queue.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dinero::vault::testing {
namespace {

AccountId acct(const std::string& name) {
    return AccountId{name};
}
OutpointId outpoint(uint8_t tag, uint32_t vout = 0) {
    OutpointId op;
    op.txid_raw.fill(tag);
    op.vout = vout;
    return op;
}
std::array<uint8_t, 32> bhash(uint8_t tag) {
    std::array<uint8_t, 32> a{};
    a.fill(tag);
    return a;
}
std::string hashKey(const std::array<uint8_t, 32>& h) {
    return std::string{reinterpret_cast<const char*>(h.data()), h.size()};
}
std::string opKey(const OutpointId& op) {
    std::string s;
    s.reserve(64 + 8);
    char buf[3];
    for (auto byte : op.txid_raw) {
        std::snprintf(buf, sizeof(buf), "%02x", static_cast<int>(byte));
        s += buf;
    }
    s += ":";
    s += std::to_string(op.vout);
    return s;
}

constexpr LedgerTimestamp T0 = 1'776'500'000'000'000'000;

// ───── DepositFlowMachine tests (Swift parity) ─────

TEST(VaultDepositFlow, detectedIsTheInitialStage) {
    Ledger l;
    DepositFlowMachine m{&l};
    m.observe(outpoint(1), acct("a"), 100, 100);
    auto it = m.tracked().find(outpoint(1));
    ASSERT_NE(it, m.tracked().end());
    EXPECT_EQ(it->second.stage, DepositStage::DETECTED);
    EXPECT_TRUE(l.entries().empty());
}

TEST(VaultDepositFlow, tipAdvanceMovesDetectedToObservedAtKObserve) {
    Ledger l;
    DepositFlowMachine m{&l};
    m.observe(outpoint(2), acct("a"), 100, 100);
    int n = m.tipChanged(100);
    EXPECT_EQ(n, 1);
    EXPECT_EQ(m.tracked().at(outpoint(2)).stage, DepositStage::OBSERVED);
}

TEST(VaultDepositFlow, tipAdvanceCascadesToCreditAtKCredit) {
    Ledger l;
    DepositFlowMachine m{&l};
    m.observe(outpoint(3), acct("a"), 100, 100);
    int n = m.tipChanged(101);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(m.tracked().at(outpoint(3)).stage, DepositStage::CREDITED);
    EXPECT_EQ(l.totalOpenCredits(), 100U);
}

TEST(VaultDepositFlow, tipAdvanceCascadesToSettleAtKSettle) {
    Ledger l;
    DepositFlowMachine m{&l};
    m.observe(outpoint(4), acct("a"), 100, 100);
    int n = m.tipChanged(105);
    EXPECT_EQ(n, 3);
    EXPECT_EQ(m.tracked().at(outpoint(4)).stage, DepositStage::SETTLED);
    EXPECT_EQ(l.totalOpenCredits(), 0U);
    EXPECT_EQ(l.accounts().at(acct("a")).confirmed(), 100U);
}

TEST(VaultDepositFlow, sizeMatrixOverridesKCreditForLargeDeposits) {
    Ledger l;
    ConfirmationPolicy policy = ConfirmationPolicy::defaults();
    policy.size_matrix = {{1000, 4}, {10000, 6}};
    DepositFlowMachine m{&l, policy};
    m.observe(outpoint(5), acct("a"), 5000, 100);
    m.tipChanged(101);
    EXPECT_EQ(m.tracked().at(outpoint(5)).stage, DepositStage::OBSERVED);
    m.tipChanged(103);
    EXPECT_EQ(m.tracked().at(outpoint(5)).stage, DepositStage::CREDITED);
}

TEST(VaultDepositFlow, sizeMatrixOverridesKCreditForJumboDeposits) {
    Ledger l;
    ConfirmationPolicy policy = ConfirmationPolicy::defaults();
    policy.size_matrix = {{1000, 4}, {10000, 6}};
    DepositFlowMachine m{&l, policy};
    m.observe(outpoint(6), acct("a"), 50000, 100);
    m.tipChanged(103);
    EXPECT_EQ(m.tracked().at(outpoint(6)).stage, DepositStage::OBSERVED);
    m.tipChanged(105);
    EXPECT_EQ(m.tracked().at(outpoint(6)).stage, DepositStage::SETTLED);
}

TEST(VaultDepositFlow, observeIsIdempotent) {
    Ledger l;
    DepositFlowMachine m{&l};
    m.observe(outpoint(7), acct("a"), 100, 100);
    m.observe(outpoint(7), acct("a"), 999, 200);
    auto& t = m.tracked().at(outpoint(7));
    EXPECT_EQ(t.amount, 100U);
    EXPECT_EQ(t.deposit_height, 100U);
}

TEST(VaultDepositFlow, capPressurePreservesSequenceAndSurfaceLedgerError) {
    LedgerCaps caps;
    caps.per_user = 50;
    Ledger l{caps};
    DepositFlowMachine m{&l};
    m.observe(outpoint(8), acct("a"), 100, 100);
    EXPECT_THROW({
        try {
            m.tipChanged(101);
        } catch (const DepositFlowError& e) {
            EXPECT_EQ(e.kind(), DepositFlowError::Kind::LEDGER);
            throw;
        }
    }, DepositFlowError);
    EXPECT_EQ(l.entries().size(), 1U);
    EXPECT_EQ(m.tracked().at(outpoint(8)).stage, DepositStage::OBSERVED);
}

TEST(VaultDepositFlow, revertOfCreditedDepositOpensCompensatingDebit) {
    Ledger l;
    DepositFlowMachine m{&l};
    m.observe(outpoint(9), acct("a"), 100, 100);
    m.tipChanged(101);
    EXPECT_EQ(m.tracked().at(outpoint(9)).stage, DepositStage::CREDITED);
    m.revert(outpoint(9), 0);
    EXPECT_EQ(m.tracked().at(outpoint(9)).stage, DepositStage::REVERTED);
    EXPECT_EQ(l.totalOpenCredits(), 0U);
    EXPECT_EQ(l.accounts().at(acct("a")).operatorLoss(), 0U);
}

TEST(VaultDepositFlow, revertOfSpentSettledDepositCarriesOperatorLoss) {
    Ledger l;
    DepositFlowMachine m{&l};
    OutpointId dep = outpoint(10);
    m.observe(dep, acct("a"), 100, 100);
    m.tipChanged(105);
    EXPECT_EQ(m.tracked().at(dep).stage, DepositStage::SETTLED);
    l.append(WithdrawalInitiated{l.nextSeq(), T0, acct("a"), outpoint(11), 100, BackendId{"test"}});
    l.append(WithdrawalSettled{l.nextSeq(), T0, acct("a"), outpoint(11)});
    m.revert(dep, 100);
    EXPECT_EQ(l.accounts().at(acct("a")).operatorLoss(), 100U);
    EXPECT_EQ(l.totalOperatorLoss(), 100U);
    EXPECT_EQ(l.accounts().at(acct("a")).spendable(), 0U);
}

TEST(VaultDepositFlow, revertOfDetectedDepositIsNoLedgerWrite) {
    Ledger l;
    DepositFlowMachine m{&l};
    m.observe(outpoint(12), acct("a"), 100, 100);
    m.revert(outpoint(12));
    EXPECT_EQ(m.tracked().at(outpoint(12)).stage, DepositStage::REVERTED);
    EXPECT_TRUE(l.entries().empty());
}

TEST(VaultDepositFlow, revertOfUnknownDepositRejects) {
    Ledger l;
    DepositFlowMachine m{&l};
    EXPECT_THROW({
        try {
            m.revert(outpoint(13));
        } catch (const DepositFlowError& e) {
            EXPECT_EQ(e.kind(), DepositFlowError::Kind::UNKNOWN_DEPOSIT);
            throw;
        }
    }, DepositFlowError);
}

TEST(VaultDepositFlow, tipBelowDepositHeightIsNoOp) {
    Ledger l;
    DepositFlowMachine m{&l};
    m.observe(outpoint(14), acct("a"), 100, 200);
    m.tipChanged(150);
    EXPECT_EQ(m.tracked().at(outpoint(14)).stage, DepositStage::DETECTED);
}

TEST(VaultDepositFlow, multiDepositTipAdvanceTracksEachIndependently) {
    Ledger l;
    DepositFlowMachine m{&l};
    m.observe(outpoint(20), acct("a"), 100, 100);
    m.observe(outpoint(21), acct("b"), 50, 102);
    int n = m.tipChanged(104);
    EXPECT_EQ(n, 4);
    EXPECT_EQ(m.tracked().at(outpoint(20)).stage, DepositStage::CREDITED);
    EXPECT_EQ(m.tracked().at(outpoint(21)).stage, DepositStage::CREDITED);
}

// ───── ReorgWatcher tests ─────

class MiniChain {
   public:
    void setBlock(uint64_t height, const std::array<uint8_t, 32>& hash,
                  const std::vector<OutpointId>& txids) {
        hashes_[height] = hash;
        for (const auto& op : txids) {
            txids_per_block_[hashKey(hash)].insert(opKey(op));
        }
    }
    void reorg(uint64_t height, const std::array<uint8_t, 32>& new_hash,
               const std::vector<OutpointId>& txids) {
        hashes_[height] = new_hash;
        std::set<std::string> set;
        for (const auto& op : txids) {
            set.insert(opKey(op));
        }
        txids_per_block_[hashKey(new_hash)] = set;
    }
    std::array<uint8_t, 32> headerHash(uint64_t height) const {
        auto it = hashes_.find(height);
        if (it == hashes_.end()) {
            return std::array<uint8_t, 32>{};
        }
        return it->second;
    }
    bool contains(const OutpointId& op, const std::array<uint8_t, 32>& hash) const {
        auto it = txids_per_block_.find(hashKey(hash));
        if (it == txids_per_block_.end()) {
            return false;
        }
        return it->second.count(opKey(op)) != 0U;
    }
    void clearHeight(uint64_t height) { hashes_.erase(height); }

   private:
    std::unordered_map<uint64_t, std::array<uint8_t, 32>> hashes_;
    std::unordered_map<std::string, std::set<std::string>> txids_per_block_;
};

TEST(VaultReorg, unchangedChainEmitsZeroReverts) {
    Ledger l;
    DepositFlowMachine m{&l};
    MiniChain chain;
    OutpointId dep = outpoint(31);
    chain.setBlock(100, bhash(0xAA), {dep});
    m.observe(dep, acct("a"), 100, 100);
    ReorgWatcher w{
        &m,
        [&](uint64_t h) { return chain.headerHash(h); },
        [&](const OutpointId& op, uint64_t /*h*/, const std::array<uint8_t, 32>& hh) {
            return chain.contains(op, hh);
        }};
    w.recordObservation(dep, bhash(0xAA));
    m.tipChanged(101);
    int n = w.tipChanged(101);
    EXPECT_EQ(n, 0);
    EXPECT_EQ(m.tracked().at(dep).stage, DepositStage::CREDITED);
}

TEST(VaultReorg, reMineSameTxidUpdatesHashAndDoesNotRevert) {
    Ledger l;
    DepositFlowMachine m{&l};
    MiniChain chain;
    OutpointId dep = outpoint(32);
    chain.setBlock(100, bhash(0xAA), {dep});
    m.observe(dep, acct("a"), 100, 100);
    ReorgWatcher w{
        &m,
        [&](uint64_t h) { return chain.headerHash(h); },
        [&](const OutpointId& op, uint64_t /*h*/, const std::array<uint8_t, 32>& hh) {
            return chain.contains(op, hh);
        }};
    w.recordObservation(dep, bhash(0xAA));
    m.tipChanged(101);
    chain.reorg(100, bhash(0xBB), {dep});
    int reverts = w.tipChanged(102);
    EXPECT_EQ(reverts, 0);
    EXPECT_EQ(w.depositBlockHashes().at(dep), bhash(0xBB));
    EXPECT_EQ(m.tracked().at(dep).stage, DepositStage::CREDITED);
}

TEST(VaultReorg, orphanedDepositTriggersRevert) {
    Ledger l;
    DepositFlowMachine m{&l};
    MiniChain chain;
    OutpointId dep = outpoint(33);
    chain.setBlock(100, bhash(0xAA), {dep});
    m.observe(dep, acct("a"), 100, 100);
    ReorgWatcher w{
        &m,
        [&](uint64_t h) { return chain.headerHash(h); },
        [&](const OutpointId& op, uint64_t /*h*/, const std::array<uint8_t, 32>& hh) {
            return chain.contains(op, hh);
        }};
    w.recordObservation(dep, bhash(0xAA));
    m.tipChanged(101);
    EXPECT_EQ(m.tracked().at(dep).stage, DepositStage::CREDITED);
    chain.reorg(100, bhash(0xCC), {});
    int reverts = w.tipChanged(102);
    EXPECT_EQ(reverts, 1);
    EXPECT_EQ(m.tracked().at(dep).stage, DepositStage::REVERTED);
    EXPECT_EQ(l.totalOpenCredits(), 0U);
}

TEST(VaultReorg, orphanedSettledDepositTriggersRevertAndOperatorLoss) {
    Ledger l;
    DepositFlowMachine m{&l};
    MiniChain chain;
    OutpointId dep = outpoint(34);
    chain.setBlock(100, bhash(0xAA), {dep});
    m.observe(dep, acct("a"), 100, 100);
    ReorgWatcher w{
        &m,
        [&](uint64_t h) { return chain.headerHash(h); },
        [&](const OutpointId& op, uint64_t /*h*/, const std::array<uint8_t, 32>& hh) {
            return chain.contains(op, hh);
        }};
    w.recordObservation(dep, bhash(0xAA));
    m.tipChanged(105);
    EXPECT_EQ(m.tracked().at(dep).stage, DepositStage::SETTLED);
    l.append(WithdrawalInitiated{l.nextSeq(), T0, acct("a"), outpoint(0xF1), 100, BackendId{"test"}});
    l.append(WithdrawalSettled{l.nextSeq(), T0, acct("a"), outpoint(0xF1)});
    chain.reorg(100, bhash(0xDD), {});
    int reverts = w.tipChanged(106);
    EXPECT_EQ(reverts, 1);
    EXPECT_EQ(l.totalOperatorLoss(), 100U);
}

TEST(VaultReorg, unknownChainStateIsNoOp) {
    Ledger l;
    DepositFlowMachine m{&l};
    MiniChain chain;
    OutpointId dep = outpoint(35);
    chain.setBlock(100, bhash(0xAA), {dep});
    m.observe(dep, acct("a"), 100, 100);
    ReorgWatcher w{
        &m,
        [&](uint64_t h) { return chain.headerHash(h); },
        [&](const OutpointId& op, uint64_t /*h*/, const std::array<uint8_t, 32>& hh) {
            return chain.contains(op, hh);
        }};
    w.recordObservation(dep, bhash(0xAA));
    m.tipChanged(101);
    chain.clearHeight(100);
    int reverts = w.tipChanged(102);
    EXPECT_EQ(reverts, 0);
    EXPECT_EQ(m.tracked().at(dep).stage, DepositStage::CREDITED);
}

TEST(VaultReorg, recordObservationIsIdempotent) {
    Ledger l;
    DepositFlowMachine m{&l};
    MiniChain chain;
    ReorgWatcher w{
        &m, [&](uint64_t h) { return chain.headerHash(h); },
        [&](const OutpointId& op, uint64_t, const std::array<uint8_t, 32>& hh) {
            return chain.contains(op, hh);
        }};
    OutpointId dep = outpoint(36);
    w.recordObservation(dep, bhash(0xAA));
    w.recordObservation(dep, bhash(0xBB));
    EXPECT_EQ(w.depositBlockHashes().at(dep), bhash(0xAA));
}

TEST(VaultReorg, unrecordedObservationOnCreditedDepositRaises) {
    Ledger l;
    DepositFlowMachine m{&l};
    MiniChain chain;
    OutpointId dep = outpoint(37);
    chain.setBlock(100, bhash(0xAA), {dep});
    m.observe(dep, acct("a"), 100, 100);
    ReorgWatcher w{
        &m, [&](uint64_t h) { return chain.headerHash(h); },
        [&](const OutpointId& op, uint64_t, const std::array<uint8_t, 32>& hh) {
            return chain.contains(op, hh);
        }};
    // Skip recordObservation intentionally.
    m.tipChanged(101);
    EXPECT_THROW({
        try {
            w.tipChanged(102);
        } catch (const ReorgError& e) {
            EXPECT_EQ(e.kind(), ReorgError::Kind::UNRECORDED_OBSERVATION);
            throw;
        }
    }, ReorgError);
}

TEST(VaultReorg, detectedAndObservedDepositsSkipped) {
    Ledger l;
    DepositFlowMachine m{&l};
    MiniChain chain;
    chain.setBlock(100, bhash(0xAA), {outpoint(38)});
    chain.setBlock(105, bhash(0xBB), {outpoint(39)});
    m.observe(outpoint(38), acct("a"), 100, 100);
    m.observe(outpoint(39), acct("a"), 100, 105);
    m.tipChanged(105);
    ReorgWatcher w{
        &m, [&](uint64_t h) { return chain.headerHash(h); },
        [&](const OutpointId& op, uint64_t, const std::array<uint8_t, 32>& hh) {
            return chain.contains(op, hh);
        }};
    w.recordObservation(outpoint(38), bhash(0xAA));
    int reverts = w.tipChanged(106);
    EXPECT_EQ(reverts, 0);
}

// ───── WithdrawalQueue tests ─────

namespace {

WithdrawalId makeRid(uint8_t seed) {
    WithdrawalId rid{};
    rid.fill(seed);
    return rid;
}

std::vector<uint8_t> taprootDest() {
    std::vector<uint8_t> spk;
    spk.reserve(34);
    spk.push_back(0x51);
    spk.push_back(0x20);
    for (int i = 0; i < 32; ++i) {
        spk.push_back(0xab);
    }
    return spk;
}

struct WithdrawalFixture {
    Ledger ledger;
    InMemorySigningBackend backend;
    WithdrawalQueue queue;
    uint8_t next_rid_seed{1};

    explicit WithdrawalFixture(UnaAmount confirmed = 1'000, UnaAmount float_amt = 1'000'000,
                               WithdrawalCaps caps = WithdrawalCaps::unbounded(),
                               WithdrawalConfirmationPolicy policy = WithdrawalConfirmationPolicy::defaults())
        : backend{BackendId{"test"}, float_amt}, queue{&ledger, &backend, caps, policy} {
        // Pre-seed a settled deposit so the user has spendable confirmed.
        ledger.append(DepositObserved{1, T0, acct("a"), outpoint(0xD0), confirmed});
        ledger.append(CreditOpened{2, T0, acct("a"), outpoint(0xD0), confirmed});
        ledger.append(CreditSettled{3, T0, acct("a"), outpoint(0xD0)});
        queue.setRequestIdGenerator([this]() {
            uint8_t s = next_rid_seed++;
            return makeRid(s);
        });
    }
};

}  // namespace

TEST(VaultWithdrawal, enqueueRejectsZeroAmount) {
    WithdrawalFixture fx;
    EXPECT_THROW({
        try {
            fx.queue.enqueue(acct("a"), 0, taprootDest());
        } catch (const WithdrawalQueueError& e) {
            EXPECT_EQ(e.kind(), WithdrawalQueueError::Kind::ZERO_AMOUNT);
            throw;
        }
    }, WithdrawalQueueError);
}

TEST(VaultWithdrawal, enqueueRejectsAboveSpendable) {
    WithdrawalFixture fx{100};
    EXPECT_THROW({
        try {
            fx.queue.enqueue(acct("a"), 200, taprootDest());
        } catch (const WithdrawalQueueError& e) {
            EXPECT_EQ(e.kind(), WithdrawalQueueError::Kind::INSUFFICIENT_SPENDABLE);
            throw;
        }
    }, WithdrawalQueueError);
}

TEST(VaultWithdrawal, enqueueRespectsPerRequestCap) {
    WithdrawalCaps caps;
    caps.per_request = 50;
    WithdrawalFixture fx{1000, 1'000'000, caps};
    EXPECT_THROW({
        try {
            fx.queue.enqueue(acct("a"), 100, taprootDest());
        } catch (const WithdrawalQueueError& e) {
            EXPECT_EQ(e.kind(), WithdrawalQueueError::Kind::PER_REQUEST_CAP_EXCEEDED);
            throw;
        }
    }, WithdrawalQueueError);
}

TEST(VaultWithdrawal, enqueueRespectsPerAccountOutstandingCap) {
    WithdrawalCaps caps;
    caps.per_account_outstanding = 100;
    WithdrawalFixture fx{1000, 1'000'000, caps};
    fx.queue.enqueue(acct("a"), 60, taprootDest());
    EXPECT_THROW({
        try {
            fx.queue.enqueue(acct("a"), 60, taprootDest());
        } catch (const WithdrawalQueueError& e) {
            EXPECT_EQ(e.kind(), WithdrawalQueueError::Kind::PER_ACCOUNT_OUTSTANDING_EXCEEDED);
            throw;
        }
    }, WithdrawalQueueError);
}

TEST(VaultWithdrawal, enqueueRespectsGlobalQueueDepth) {
    WithdrawalCaps caps;
    caps.global_queue_depth = 2;
    WithdrawalFixture fx{1000, 1'000'000, caps};
    fx.queue.enqueue(acct("a"), 10, taprootDest());
    fx.queue.enqueue(acct("a"), 10, taprootDest());
    EXPECT_THROW({
        try {
            fx.queue.enqueue(acct("a"), 10, taprootDest());
        } catch (const WithdrawalQueueError& e) {
            EXPECT_EQ(e.kind(), WithdrawalQueueError::Kind::GLOBAL_QUEUE_FULL);
            throw;
        }
    }, WithdrawalQueueError);
}

TEST(VaultWithdrawal, enqueueRespectsDestinationValidator) {
    WithdrawalFixture fx;
    fx.queue.setDestinationValidator([](const std::vector<uint8_t>& spk) {
        return spk.size() >= 2 && spk[0] == 0x51;  // taproot only
    });
    std::vector<uint8_t> bad_dest{0x76, 0xa9, 0x14};
    EXPECT_THROW({
        try {
            fx.queue.enqueue(acct("a"), 10, bad_dest);
        } catch (const WithdrawalQueueError& e) {
            EXPECT_EQ(e.kind(), WithdrawalQueueError::Kind::DESTINATION_REJECTED);
            throw;
        }
    }, WithdrawalQueueError);
}

TEST(VaultWithdrawal, processNextSignsAndWritesInitiated) {
    WithdrawalFixture fx;
    WithdrawalId id = fx.queue.enqueue(acct("a"), 100, taprootDest());
    auto processed = fx.queue.processNext();
    ASSERT_TRUE(processed.has_value());
    EXPECT_EQ(*processed, id);
    auto acct_state = fx.ledger.accounts().at(acct("a"));
    EXPECT_EQ(acct_state.locked(), 100U);
    EXPECT_EQ(acct_state.spendable(), 900U);
    auto state = fx.queue.state(id);
    EXPECT_TRUE(std::holds_alternative<WithdrawalBroadcast>(state));
}

TEST(VaultWithdrawal, tipChangedSettlesAtKConfirmations) {
    WithdrawalConfirmationPolicy policy;
    policy.k_settle = 3;
    WithdrawalFixture fx{1000, 1'000'000, WithdrawalCaps::unbounded(), policy};
    WithdrawalId id = fx.queue.enqueue(acct("a"), 100, taprootDest());
    fx.queue.processNext();
    fx.queue.markBroadcastIncluded(id, 50);
    fx.queue.tipChanged(50);  // 1 conf
    EXPECT_TRUE(std::holds_alternative<WithdrawalBroadcast>(fx.queue.state(id)));
    EXPECT_EQ(fx.ledger.accounts().at(acct("a")).locked(), 100U);
    fx.queue.tipChanged(52);  // 3 confs
    EXPECT_TRUE(std::holds_alternative<WithdrawalSettledOnChain>(fx.queue.state(id)));
    auto s = fx.ledger.accounts().at(acct("a"));
    EXPECT_EQ(s.locked(), 0U);
    EXPECT_EQ(s.confirmed(), 900U);
}

TEST(VaultWithdrawal, backendErrorMarksFailedAndPreservesLedger) {
    WithdrawalFixture fx;
    fx.backend.setNextErrorTrap(SigningBackendError::Kind::REJECTED_BY_POLICY);
    WithdrawalId id = fx.queue.enqueue(acct("a"), 100, taprootDest());
    EXPECT_THROW({
        try {
            fx.queue.processNext();
        } catch (const WithdrawalQueueError& e) {
            EXPECT_EQ(e.kind(), WithdrawalQueueError::Kind::BACKEND_ERROR);
            throw;
        }
    }, WithdrawalQueueError);
    EXPECT_TRUE(std::holds_alternative<WithdrawalFailed>(fx.queue.state(id)));
    EXPECT_EQ(fx.ledger.entries().size(), 3U);
    EXPECT_EQ(fx.ledger.accounts().at(acct("a")).locked(), 0U);
}

TEST(VaultWithdrawal, revertOfBroadcastReleasesLock) {
    WithdrawalFixture fx;
    WithdrawalId id = fx.queue.enqueue(acct("a"), 100, taprootDest());
    fx.queue.processNext();
    EXPECT_EQ(fx.ledger.accounts().at(acct("a")).locked(), 100U);
    fx.queue.revert(id);
    EXPECT_TRUE(std::holds_alternative<WithdrawalRevertedOnChain>(fx.queue.state(id)));
    auto s = fx.ledger.accounts().at(acct("a"));
    EXPECT_EQ(s.locked(), 0U);
    EXPECT_EQ(s.spendable(), 1000U);
}

TEST(VaultWithdrawal, unknownRequestRejects) {
    WithdrawalFixture fx;
    EXPECT_THROW({
        try {
            fx.queue.markBroadcastIncluded(makeRid(0x99), 100);
        } catch (const WithdrawalQueueError& e) {
            EXPECT_EQ(e.kind(), WithdrawalQueueError::Kind::UNKNOWN_REQUEST);
            throw;
        }
    }, WithdrawalQueueError);
}

TEST(VaultWithdrawal, markBroadcastBeforeProcessNextRejects) {
    WithdrawalFixture fx;
    WithdrawalId id = fx.queue.enqueue(acct("a"), 100, taprootDest());
    EXPECT_THROW({
        try {
            fx.queue.markBroadcastIncluded(id, 100);
        } catch (const WithdrawalQueueError& e) {
            EXPECT_EQ(e.kind(), WithdrawalQueueError::Kind::LIFECYCLE_VIOLATION);
            throw;
        }
    }, WithdrawalQueueError);
}

TEST(VaultWithdrawal, outstandingDepthAndOutstandingPerAccount) {
    WithdrawalFixture fx;
    fx.queue.enqueue(acct("a"), 50, taprootDest());
    fx.queue.enqueue(acct("a"), 30, taprootDest());
    EXPECT_EQ(fx.queue.outstandingDepth(), 2);
    EXPECT_EQ(fx.queue.currentOutstanding(acct("a")), 80U);
}

}  // namespace
}  // namespace dinero::vault::testing
