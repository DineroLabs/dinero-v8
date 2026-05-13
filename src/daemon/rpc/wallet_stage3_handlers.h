#pragma once
#include "compat/jsoncpp_compat.h"

namespace dinero {

/**
 * Stage 3 Wallet RPC Handlers for Two-Node Relay Testing
 * These are minimal implementations focused on regtest functionality
 */

/**
 * Generate new Bech32 P2WPKH address for regtest
 * @param params [] - no parameters
 * @return new address string
 */
Json::Value wallet_getnewaddress_stage3(const Json::Value& params);

/**
 * Get wallet information and balance
 * @param params [] - no parameters  
 * @return wallet info object with balance, txcount, etc.
 */
Json::Value wallet_getwalletinfo_stage3(const Json::Value& params);

/**
 * Get address information and ownership status
 * @param params [address] - address to check
 * @return address info object with ismine, script type, etc.
 */
Json::Value wallet_getaddressinfo_stage3(const Json::Value& params);

/**
 * List unspent transaction outputs
 * @param params [minconf, maxconf, addresses] - optional parameters
 * @return array of unspent outputs
 */
Json::Value wallet_listunspent_stage3(const Json::Value& params);

/**
 * Create raw transaction from inputs and outputs
 * @param params [inputs, outputs, locktime] - transaction parameters
 * @return hex-encoded raw transaction
 */
Json::Value wallet_createrawtransaction_stage3(const Json::Value& params);

/**
 * Fund raw transaction with UTXOs and calculate fees
 * @param params [hex, options] - transaction hex and funding options
 * @return funded transaction with fee information
 */
Json::Value wallet_fundrawtransaction_stage3(const Json::Value& params);

/**
 * Sign raw transaction with wallet keys
 * @param params [hex, prevtxs, sigtype] - transaction and signing parameters
 * @return signed transaction hex
 */
Json::Value wallet_signrawtransactionwithwallet_stage3(const Json::Value& params);

/**
 * Generate blocks to specified address (regtest only)
 * @param params [nblocks, address, maxtries] - generation parameters
 * @return array of generated block hashes
 */
Json::Value wallet_generatetoaddress_stage3(const Json::Value& params);

} // namespace dinero
