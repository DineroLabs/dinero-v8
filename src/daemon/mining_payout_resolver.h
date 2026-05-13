#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {

class ChainParamsImpl;

/**
 * Set mining payout address with validation
 * @param addr Bech32 address for mining payouts
 * @param params Chain parameters for validation
 * @return true on success, throws on invalid address
 */
bool SetMiningAddress(const std::string& addr, const ChainParamsImpl& params);

/**
 * Get the current mining payout address
 * @return Current mining address, or empty string if none set
 */
std::string GetMiningAddress();

/**
 * Get the current mining script for coinbase outputs
 * @return Script bytes for coinbase vout[0], or empty if none set
 */
std::vector<uint8_t> GetMiningScript();

/**
 * Load mining address from persistent storage at startup
 * @param datadir Data directory path
 */
void LoadMiningAddress(const std::string& datadir);

/**
 * Initialize mining payout resolver
 */
void InitMiningPayoutResolver();

} // namespace dinero
