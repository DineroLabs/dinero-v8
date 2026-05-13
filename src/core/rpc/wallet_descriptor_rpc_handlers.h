#pragma once
#include "compat/jsoncpp_compat.h"

namespace dinero {
    class WalletManager;  // Forward declaration

/**
 * List active descriptors for the current wallet
 * @param params {"private": bool} - include private keys (default: false)
 * @param wallet_manager Wallet manager instance
 * @return Descriptor list with metadata
 */
Json::Value rpc_wallet_listdescriptors(const Json::Value& params, dinero::WalletManager* wallet_manager);

/**
 * Parse and analyze a descriptor string
 * @param params {"descriptor": string} - descriptor to analyze
 * @param wallet_manager Wallet manager instance (can be null for descriptor-only operations)
 * @return Descriptor metadata and checksum
 */
Json::Value rpc_wallet_getdescriptorinfo(const Json::Value& params, dinero::WalletManager* wallet_manager);

/**
 * Derive addresses from a descriptor
 * @param params {"descriptor": string, "range": [start, end]} - descriptor and range
 * @param wallet_manager Wallet manager instance
 * @return List of derived addresses
 */
Json::Value rpc_wallet_deriveaddresses(const Json::Value& params, dinero::WalletManager* wallet_manager);

/**
 * Export wallet descriptors in backup-friendly format
 * @param params {} - no parameters required
 * @param wallet_manager Wallet manager instance
 * @return Descriptor export suitable for backup/watch-only wallets
 */
Json::Value rpc_wallet_exportdescriptors(const Json::Value& params, dinero::WalletManager* wallet_manager);

/**
 * Import descriptors for watch-only wallet support
 * @param params {"requests": [{descriptor, active, timestamp, range, internal, label}]} - array of descriptor import requests
 * @param wallet_manager Wallet manager instance
 * @return Array of import results with success/failure status
 */
Json::Value rpc_wallet_importdescriptors(const Json::Value& params, dinero::WalletManager* wallet_manager);

} // namespace dinero
