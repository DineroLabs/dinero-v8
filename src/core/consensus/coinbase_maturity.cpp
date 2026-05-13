#include "consensus/coinbase_maturity.h"
#include <algorithm>

namespace dinero {

bool CoinbaseMaturity::isCoinbaseMature(uint32_t coinbase_height, uint32_t current_height) {
    if (current_height < coinbase_height) {
        return false; // Invalid: current height before coinbase
    }
    
    uint32_t confirmations = current_height - coinbase_height + 1;
    return confirmations >= CoinbaseMaturity::COINBASE_MATURITY;
}

uint32_t CoinbaseMaturity::getCoinbaseSpendableHeight(uint32_t coinbase_height) {
    // Handle overflow case
    if (coinbase_height > UINT32_MAX - CoinbaseMaturity::COINBASE_MATURITY + 1) {
        return UINT32_MAX;
    }
    
    return coinbase_height + CoinbaseMaturity::COINBASE_MATURITY - 1;
}

uint32_t CoinbaseMaturity::getBlocksUntilMature(uint32_t coinbase_height, uint32_t current_height) {
    if (CoinbaseMaturity::isCoinbaseMature(coinbase_height, current_height)) {
        return 0;
    }
    
    if (current_height < coinbase_height) {
        return CoinbaseMaturity::COINBASE_MATURITY; // Conservative estimate
    }
    
    uint32_t confirmations = current_height - coinbase_height + 1;
    return CoinbaseMaturity::COINBASE_MATURITY - confirmations;
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
