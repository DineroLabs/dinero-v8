#pragma once
#include <cstdint>

namespace dinero {

// Forward declaration
struct ChainParams;

/**
 * Get the block subsidy for a given height according to Dinero's subsidy schedule:
 * - Height 0: Genesis block (unspendable)
 * - Height >=1: Subsidy schedule 99->66->33->16->8->4->2->1->1 forever
 */
uint64_t GetBlockSubsidy(int32_t height, const ChainParams& params);

} // namespace dinero