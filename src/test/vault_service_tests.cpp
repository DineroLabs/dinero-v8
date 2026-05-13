// Copyright (c) 2026 Dinero Labs.
//
// Daemon-side gtest port of the Swift VaultServiceTests. Verifies
// the orchestrator wires Ledger + DepositFlow + ReorgWatcher +
// WithdrawalQueue + SigningBackend correctly under the same
// chain-event sequencing the daemon will produce in production.

#include <gtest/gtest.h>

#include "vault/signing_backend.h"
#include "vault/vault_service.h"
#include "vault/vault_types.h"
#include "vault/withdrawal_queue.h"

#include <array>
#include <cstdio>
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

std::unique_ptr<VaultService> makeService(MiniChain* chain, VaultServiceConfig cfg = {}) {
    auto backend = std::make_unique<InMemorySigningBackend>(BackendId{"test"}, 1'000'000'000);
    return std::make_unique<VaultService>(
        std::move(backend), cfg,
        [chain](uint64_t h) { return chain->headerHash(h); },
        [chain](const OutpointId& op, uint64_t /*h*/, const std::array<uint8_t, 32>& hh) {
            return chain->contains(op, hh);
        });
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

}  // namespace
}  // namespace dinero::vault::testing
