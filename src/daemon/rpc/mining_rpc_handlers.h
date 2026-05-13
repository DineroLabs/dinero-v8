#pragma once
#include "compat/jsoncpp_compat.h"

namespace dinero {

/**
 * Set mining payout address
 * @param params [address] - Bech32 address for mining payouts
 * @return true on success
 */
Json::Value mining_setaddress(const Json::Value& params);

/**
 * Get current mining payout address
 * @param params [] - no parameters
 * @return address string or null if none set
 */
Json::Value mining_getaddress(const Json::Value& params);

/**
 * Generate blocks to specified address (regtest only)
 * @param params [nblocks, address] - number of blocks and target address
 * @return array of block hashes
 */
Json::Value mining_generatetoaddress(const Json::Value& params);

} // namespace dinero
