// Test fakes for the wallet's node-facing interfaces (Task 5.1).
// Map-backed, with per-method call counters. FakeChainView never touches
// block storage: reads_block_storage stays false by construction.
#pragma once

#include "wallet/node_interfaces.h"

#include <map>
#include <optional>
#include <vector>

namespace dinero::wallet::testing {

class FakeChainView final : public IWalletChainView {
public:
    std::optional<WalletChainTip> GetTip() const override {
        ++get_tip_calls;
        if (blocks_by_height.empty()) return std::nullopt;
        const auto& [height, hash] = *blocks_by_height.rbegin();
        return WalletChainTip{height, hash};
    }

    std::optional<uint256> GetBlockHashAtHeight(int32_t height) const override {
        ++get_hash_calls;
        auto it = blocks_by_height.find(height);
        if (it == blocks_by_height.end()) return std::nullopt;
        return it->second;
    }

    std::optional<Block> ReadBlock(const uint256& hash) const override {
        ++read_block_calls;
        auto it = blocks_by_hash.find(hash);
        if (it == blocks_by_hash.end()) return std::nullopt;
        return it->second;
    }

    // Fixture state.
    std::map<int32_t, uint256> blocks_by_height;
    std::map<uint256, Block> blocks_by_hash;

    // Call counters.
    mutable int get_tip_calls = 0;
    mutable int get_hash_calls = 0;
    mutable int read_block_calls = 0;

    // The fake never reads block storage; the flag exists so tests can
    // assert the invariant explicitly.
    const bool reads_block_storage = false;
};

class FakeMempoolView final : public IWalletMempoolView {
public:
    bool IsOutputSpent(const OutPoint& out) const override {
        ++is_output_spent_calls;
        return spent.count(out) != 0;
    }

    std::optional<WalletMempoolEntry> GetEntry(const uint256& txid) const override {
        ++get_entry_calls;
        auto it = entries.find(txid);
        if (it == entries.end()) return std::nullopt;
        return it->second;
    }

    WalletMempoolStats GetStats() const override {
        ++get_stats_calls;
        return stats;
    }

    // Fixture state.
    std::map<OutPoint, bool> spent;
    std::map<uint256, WalletMempoolEntry> entries;
    WalletMempoolStats stats{};

    // Call counters.
    mutable int is_output_spent_calls = 0;
    mutable int get_entry_calls = 0;
    mutable int get_stats_calls = 0;
};

} // namespace dinero::wallet::testing
