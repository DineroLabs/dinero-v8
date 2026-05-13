#pragma once

#include "din_json.h"

namespace dinero {
class WalletManager;
}

namespace dinero::rpc {

/**
 * Basic wallet management RPC handlers
 * Provides create, load, and list functionality for wallets
 */

/**
 * Create a new wallet
 * 
 * @param params JSON object with:
 *   - name: string (wallet name, optional, defaults to "default")
 *   - encrypted: bool (optional, whether to encrypt wallet, default false)
 *   - passphrase: string (optional, encryption passphrase if encrypted=true)
 * 
 * @return JSON object with:
 *   - name: string (wallet name)
 *   - created: bool (always true on success)
 *   - encrypted: bool (whether wallet is encrypted)
 */
din::Json RpcWalletCreate(const din::Json& params, 
                          dinero::WalletManager* wallet_manager);

/**
 * Load/activate a wallet
 * 
 * @param params JSON object with:
 *   - name: string (wallet name, optional, defaults to "default")
 * 
 * @return JSON object with:
 *   - name: string (wallet name)
 *   - active: bool (always true on success)
 */
din::Json RpcWalletLoad(const din::Json& params, 
                        dinero::WalletManager* wallet_manager);

/**
 * List available wallets
 * 
 * @param params JSON object (can be empty)
 * 
 * @return JSON object with:
 *   - wallets: array of wallet names
 *   - current: string (currently active wallet name, or null)
 */
din::Json RpcWalletList(const din::Json& params, 
                        dinero::WalletManager* wallet_manager);

/**
 * Get wallet status/info
 * 
 * @param params JSON object (can be empty)
 * 
 * @return JSON object with:
 *   - name: string (current wallet name or null)
 *   - active: bool (whether a wallet is active)
 *   - encrypted: bool (whether wallet is encrypted)
 *   - locked: bool (whether wallet is locked, if encrypted)
 */
din::Json RpcWalletStatus(const din::Json& params, 
                          dinero::WalletManager* wallet_manager);

} // namespace dinero::rpc
