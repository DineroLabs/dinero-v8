#pragma once
#include "primitives/block.h"
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
    std::vector<Block> blocks_; // exact genesis through captured tip
    uint256 tip_;
    std::string network_;
    uint256 genesis_;
};
} // namespace dinero
