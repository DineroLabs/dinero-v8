#pragma once

#include <cstdint>
#include <vector>
#include <utility>

namespace dinero {

/**
 * @brief Coinbase maturity rules and enforcement
 * 
 * Ensures coinbase outputs cannot be spent until they have sufficient confirmations.
 * This prevents issues during reorgs where coinbase transactions might become invalid.
 */
class CoinbaseMaturity {
public:
    // Coinbase outputs must have this many confirmations before spending
    static constexpr uint32_t COINBASE_MATURITY = 100;
    
    /**
     * @brief Check if a coinbase output is mature enough to spend
     * @param coinbase_height Height where coinbase was mined
     * @param current_height Current blockchain tip height
     * @return true if coinbase can be spent
     */
    static bool isCoinbaseMature(uint32_t coinbase_height, uint32_t current_height);
    
    /**
     * @brief Calculate when a coinbase will become spendable
     * @param coinbase_height Height where coinbase was mined
     * @return Height at which coinbase becomes spendable
     */
    static uint32_t getCoinbaseSpendableHeight(uint32_t coinbase_height);
    
    /**
     * @brief Get remaining blocks until coinbase matures
     * @param coinbase_height Height where coinbase was mined
     * @param current_height Current blockchain tip height
     * @return Blocks remaining (0 if already mature)
     */
    static uint32_t getBlocksUntilMature(uint32_t coinbase_height, uint32_t current_height);
    
    /**
     * @brief Check if a transaction spends immature coinbase
     * @param tx_inputs List of input heights and coinbase flags
     * @param current_height Current blockchain tip height
     * @return true if any input is an immature coinbase
     */
    static bool spendsImmatureCoinbase(const std::vector<std::pair<uint32_t, bool>>& tx_inputs, 
                                      uint32_t current_height);
};
