#include "consensus/coinbase_maturity.h"
#include <algorithm>

namespace dinero {

bool CoinbaseMaturity::isCoinbaseMature(uint32_t coinbase_height, uint32_t current_height) {
    if (current_height < coinbase_height) {
        return false; // Invalid: current height before coinbase
    }

    // Bitcoin rule: 100 blocks ON TOP of the coinbase block
    // coinbase at height 1 is spendable at height 101 (100 blocks on top)
    uint32_t blocks_on_top = current_height - coinbase_height;
    return blocks_on_top >= CoinbaseMaturity::COINBASE_MATURITY;
}

uint32_t CoinbaseMaturity::getCoinbaseSpendableHeight(uint32_t coinbase_height) {
    // Handle overflow case
    if (coinbase_height > UINT32_MAX - CoinbaseMaturity::COINBASE_MATURITY) {
        return UINT32_MAX;
    }

    // Coinbase becomes spendable after 100 blocks on top
    // e.g., coinbase at height 1 is spendable at height 101
    return coinbase_height + CoinbaseMaturity::COINBASE_MATURITY;
}

uint32_t CoinbaseMaturity::getBlocksUntilMature(uint32_t coinbase_height, uint32_t current_height) {
    if (CoinbaseMaturity::isCoinbaseMature(coinbase_height, current_height)) {
        return 0;
    }

    if (current_height < coinbase_height) {
        return CoinbaseMaturity::COINBASE_MATURITY; // Conservative estimate
    }

    // Calculate blocks on top, then subtract from required 100
    uint32_t blocks_on_top = current_height - coinbase_height;
    return CoinbaseMaturity::COINBASE_MATURITY - blocks_on_top;
}

bool CoinbaseMaturity::spendsImmatureCoinbase(const std::vector<std::pair<uint32_t, bool>>& tx_inputs, 
                                             uint32_t current_height) {
    return std::any_of(tx_inputs.begin(), tx_inputs.end(), 
        [current_height](const std::pair<uint32_t, bool>& input) {
            uint32_t input_height = input.first;
            bool is_coinbase = input.second;
            
            return is_coinbase && !CoinbaseMaturity::isCoinbaseMature(input_height, current_height);
        });
}

} // namespace dinero
