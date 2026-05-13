#pragma once

#include "din_json.h"

namespace dinero {
    class WalletManager;
}

namespace dinero::rpc {

/**
 * GUI-compatible wallet RPC handlers
 * These match the interface expected by the Qt GUI
 */

// Create HD wallet with BIP39 mnemonic (GUI-compatible)
// Params: word_count (12/24), passphrase (optional), password (required),
//         policy (optional, must be "bip86")
// Returns: {mnemonic, fingerprint, first_address}
din::Json RpcCreateHDWallet(const din::Json& params, dinero::WalletManager* wallet_manager);

// Restore HD wallet from BIP39 mnemonic (GUI-compatible)
// Params: mnemonic, passphrase (optional), password (optional),
//         policy (optional, must be "bip86"),
//         expected_first_address (optional safety guard)
// Returns: {success, fingerprint, addresses_restored}
din::Json RpcRestoreWallet(const din::Json& params, dinero::WalletManager* wallet_manager);

// Export mnemonic from existing wallet (for "Backup Seed" button)
// Params: wallet_name (optional, defaults to "default")
// Returns: {mnemonic, fingerprint, first_address, word_count}
din::Json RpcExportMnemonic(const din::Json& params, dinero::WalletManager* wallet_manager);

} // namespace dinero::rpc
