#pragma once

#include <cstdint>
#include "consensus/consensus.hpp"

namespace dinero {

/**
 * @brief Calculate block subsidy (consensus-critical)
 *
 * Dinero subsidy schedule:
 * - Block 0: 100 DIN (genesis, unspendable)
 * - Block 1+: 100 DIN initially, halving every 1,314,000 blocks
 *
 * This is the ONLY canonical implementation of block subsidy.
 * All nodes MUST use this exact function to prevent chain splits.
 *
 * Uses canonical constants from ConsensusSubsidy (subsidy.h).
 * No runtime parameters - monetary policy is immutable.
 *
 * @param height Block height
 * @return Block subsidy in una (1 DIN = 100,000,000 una)
 */
int64_t GetBlockSubsidy(uint32_t height);

} // namespace dinero
