#pragma once
#include "compat/jsoncpp_compat.h"

namespace dinero {

// Forward declaration
class WalletManager;

/**
 * Generate new Bech32 P2WPKH address
 * @param params [] - no parameters
 * @param wallet_manager - wallet manager instance
 * @return new address string
 */
Json::Value wallet_getnewaddress(const Json::Value& params, dinero::WalletManager* wallet_manager);

/**
 * Validate address and check ownership
 * @param params [address] - address to validate
 * @param wallet_manager - wallet manager instance
 * @return validation result object with isvalid, type, ismine, etc.
 */
Json::Value wallet_validateaddress(const Json::Value& params, dinero::WalletManager* wallet_manager);

} // namespace dinero
