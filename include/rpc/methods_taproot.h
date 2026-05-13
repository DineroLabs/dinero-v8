#pragma once
#include <string>
#include "compat/jsoncpp_compat.h"

namespace dinero {

// Forward declarations
class WalletManager;

/**
 * Generate a Taproot P2TR address (key path only)
 *
 * Usage: taproot.getnewaddress [label]
 * Returns: { "address": "din1p...", "pubkey": "hex" }
 */
Json::Value taproot_getnewaddress(const Json::Value& params, dinero::WalletManager* wallet_manager);

/**
 * Validate a Taproot P2TR address
 *
 * Usage: taproot.validateaddress "din1p..."
 * Returns: { "isvalid": true/false, "address": "...", "type": "p2tr" }
 */
Json::Value taproot_validateaddress(const Json::Value& params, dinero::WalletManager* wallet_manager);

/**
 * Get Taproot address info (internal key, output key, etc.)
 *
 * Usage: taproot.getaddressinfo "din1p..."
 * Returns: { "address": "...", "output_key": "hex", "type": "p2tr" }
 */
Json::Value taproot_getaddressinfo(const Json::Value& params, dinero::WalletManager* wallet_manager);

} // namespace dinero
