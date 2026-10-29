#pragma once
#include "primitives/block.h"
#include "daemon/services/snapshot_wallet_source.h"
#include <optional>
#include <set>
#include <vector>

namespace dinero {
class ChainstateService;
class WalletManager;
// Owned, independently replayed historical bodies. Only the selected service
// constructs this value. It is an as-of source, not a wallet baseline receipt,
// complete key discovery, or proof that the selected chain cannot change later.
class SelectedWalletHistory final {
    friend class ChainstateService;
    friend class WalletManager;
    SelectedWalletHistory() = default;
    std::vector<Block> blocks_; // exact first_height_ through captured tip
    uint32_t first_height_=0;
    std::set<std::vector<uint8_t>> snapshot_scripts_;
    std::optional<SnapshotWalletSourceData> snapshot_;
    std::set<std::pair<uint256,uint32_t>> verified_legacy_origins_;
    uint256 tip_;
    std::string network_;
    uint256 genesis_;
};
} // namespace dinero
