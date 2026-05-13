#pragma once

#include "din_json.h"
#include <memory>

namespace dinero {
namespace auth {
class AuthStore;
}
class WalletManager;
}

namespace dinero::rpc {

/**
 * Wallet import/export RPC handlers
 * Provides secure key import and export functionality
 */

/**
 * Import a private key into the wallet
 * 
 * @param params JSON object with:
 *   - privkey: string (WIF format private key)
 *   - label: string (optional, label for the imported key)
 *   - rescan: bool (optional, whether to rescan blockchain, default false)
 * 
 * @return JSON object with:
 *   - success: bool
 *   - address: string (the address corresponding to the imported key)
 *   - message: string (success/error message)
 */
din::Json RpcImportPrivateKey(const din::Json& params, 
                              dinero::WalletManager* wallet_manager);

/**
 * Export a private key from the wallet (requires unlocked wallet)
 * 
 * @param params JSON object with:
 *   - address: string (address to export private key for)
 * 
 * @return JSON object with:
 *   - privkey: string (hex format private key)
 *   - address: string (the address)
 */
din::Json RpcExportPrivateKey(const din::Json& params, 
                              dinero::WalletManager* wallet_manager);

/**
 * Import a mnemonic seed phrase to restore wallet
 * 
 * @param params JSON object with:
 *   - mnemonic: string (BIP39 mnemonic phrase)
 *   - passphrase: string (optional, BIP39 passphrase)
 *   - account: number (optional, account index, default 0)
 *   - rescan: bool (optional, whether to rescan blockchain, default true)
 *   - initial_address_count: number (optional, pre-derived receive addresses for restore window)
 * 
 * @return JSON object with:
 *   - success: bool
 *   - addresses_imported: number (number of addresses restored)
 *   - message: string
 */
din::Json RpcImportMnemonic(const din::Json& params, 
                            dinero::WalletManager* wallet_manager);

/**
 * Export wallet's mnemonic seed phrase (requires unlocked wallet)
 * 
 * @return JSON object with:
 *   - mnemonic: string (BIP39 mnemonic phrase)
 *   - warning: string (security warning)
 */
din::Json RpcExportMnemonic(dinero::WalletManager* wallet_manager);

/**
 * Import an encrypted private key (PBKDF2 + AES)
 * 
 * @param params JSON object with:
 *   - enc: string (encryption method, e.g., "pbkdf2-hmac-sha256")
 *   - iter: number (PBKDF2 iterations, e.g., 100000)
 *   - salt: string (base64-encoded salt)
 *   - cipher: string (cipher method, e.g., "aes-256-gcm", "aes-256-cbc")
 *   - iv: string (base64-encoded initialization vector)
 *   - ct: string (base64-encoded ciphertext)
 *   - tag: string (base64-encoded auth tag, for GCM only)
 *   - passphrase: string (decryption passphrase)
 *   - label: string (optional, label for imported key)
 *   - rescan: bool (optional, whether to rescan blockchain)
 * 
 * @return JSON object with:
 *   - success: bool
 *   - address: string (derived address)
 *   - compressed: bool (whether key is compressed)
 *   - message: string
 */
din::Json RpcImportEncryptedKey(const din::Json& params,
                                dinero::WalletManager* wallet_manager);

/**
 * Import a Taproot descriptor with private key
 *
 * Descriptor-based Taproot key import with mandatory rescan.
 * This is the ONLY recovery-safe way to import single Taproot keys.
 *
 * The descriptor format is: tr(<hex-privkey>)
 *
 * This function:
 * 1. Parses the tr() descriptor
 * 2. Derives x-only pubkey from privkey
 * 3. Applies BIP341 TapTweak to get output key
 * 4. Creates P2TR scriptPubKey from tweaked key
 * 5. Registers address with UTXOIndex for scanning
 * 6. Stores internal key for signing
 * 7. ALWAYS triggers blockchain rescan (mandatory for recovery safety)
 *
 * @param params JSON object with:
 *   - descriptor: string (tr(<64-char-hex-privkey>) format)
 *   - label: string (optional, label for the imported key)
 *
 * @return JSON object with:
 *   - success: bool
 *   - address: string (P2TR address from tweaked output key)
 *   - internal_pubkey: string (hex, x-only internal pubkey)
 *   - output_pubkey: string (hex, tweaked output pubkey)
 *   - scriptPubKey: string (hex, P2TR scriptPubKey)
 *   - rescan_triggered: bool (always true)
 *   - message: string
 */
din::Json RpcImportTaprootDescriptor(const din::Json& params,
                                      dinero::WalletManager* wallet_manager);

/**
 * Explicit one-time migration of legacy HD wallet sidecar.
 *
 * Imports `<datadir>/wallet.conf` into `<datadir>/wallet_state.db` and creates
 * a backup copy at `<datadir>/backups/wallet.conf.<timestamp>.bak`.
 *
 * @param params JSON object with:
 *   - datadir: string (required, legacy wallet directory)
 *   - coin_type: number (optional, defaults to DINERO_COIN_TYPE)
 *   - backup: bool (optional, defaults true)
 *   - overwrite_existing: bool (optional, defaults false)
 *
 * @return JSON object with:
 *   - success: bool
 *   - migrated: bool
 *   - already_migrated: bool
 *   - message: string
 *   - wallet_state_path: string
 *   - legacy_wallet_conf_path: string
 *   - backup_path: string (if backup created)
 */
din::Json RpcMigrateLegacySidecar(const din::Json& params,
                                  dinero::WalletManager* wallet_manager);

} // namespace dinero::rpc
