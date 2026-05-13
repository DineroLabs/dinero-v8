/**
 * Phase 30: Taproot Asset Layer - RPC Methods
 *
 * RPC interface for asset operations:
 * - Asset issuance and management
 * - UTXO queries and balance lookups
 * - Transfer creation and broadcasting
 * - Mint/burn operations
 */

#pragma once

#include "rpc/method.h"
#include <memory>

namespace dinero {
namespace rpc {

// ============================================================================
// Asset Query Methods
// ============================================================================

/**
 * @brief assets.listassets - List all registered assets
 *
 * Parameters:
 *   [0] limit (optional): Max results (default 100)
 *   [1] offset (optional): Pagination offset (default 0)
 *
 * Returns:
 *   {
 *     "assets": [{
 *       "asset_id": "hex",
 *       "name": "string",
 *       "ticker": "string",
 *       "decimals": number,
 *       "circulating_supply": number,
 *       "max_supply": number,
 *       "creation_txid": "hex",
 *       "creation_height": number
 *     }],
 *     "total": number
 *   }
 */
class AssetsListAssets : public RPCMethod {
public:
    AssetsListAssets();
    nlohmann::json execute(const nlohmann::json& params) override;
};

/**
 * @brief assets.getasset - Get detailed asset information
 *
 * Parameters:
 *   [0] asset_id: 32-byte hex asset ID
 *
 * Returns:
 *   {
 *     "asset_id": "hex",
 *     "metadata": {
 *       "name": "string",
 *       "ticker": "string",
 *       "decimals": number,
 *       "description": "string",
 *       "icon_url": "string"
 *     },
 *     "supply": {
 *       "model": "fixed|capped|unlimited|algorithmic",
 *       "initial_supply": number,
 *       "max_supply": number,
 *       "circulating_supply": number,
 *       "total_minted": number,
 *       "total_burned": number,
 *       "burn_enabled": boolean
 *     },
 *     "authorities": {
 *       "mint_authority": "pubkey hex or null",
 *       "burn_authority": "pubkey hex or null"
 *     },
 *     "genesis": {
 *       "txid": "hex",
 *       "vout": number,
 *       "height": number,
 *       "timestamp": number,
 *       "issuer_pubkey": "hex"
 *     },
 *     "stats": {
 *       "utxo_count": number,
 *       "holder_count": number,
 *       "transfer_count": number
 *     }
 *   }
 */
class AssetsGetAsset : public RPCMethod {
public:
    AssetsGetAsset();
    nlohmann::json execute(const nlohmann::json& params) override;
};

/**
 * @brief assets.searchassets - Search assets by name/ticker
 *
 * Parameters:
 *   [0] query: Search string
 *   [1] limit (optional): Max results (default 20)
 *
 * Returns: Same as listassets
 */
class AssetsSearchAssets : public RPCMethod {
public:
    AssetsSearchAssets();
    nlohmann::json execute(const nlohmann::json& params) override;
};

// ============================================================================
// Asset Balance Methods
// ============================================================================

/**
 * @brief assets.getbalance - Get asset balances for address
 *
 * Parameters:
 *   [0] address: Dinero address
 *   [1] asset_id (optional): Filter by asset
 *
 * Returns:
 *   {
 *     "address": "string",
 *     "balances": [{
 *       "asset_id": "hex",
 *       "ticker": "string",
 *       "confirmed": number,
 *       "unconfirmed": number,
 *       "pending_spend": number,
 *       "available": number,
 *       "utxo_count": number
 *     }]
 *   }
 */
class AssetsGetBalance : public RPCMethod {
public:
    AssetsGetBalance();
    nlohmann::json execute(const nlohmann::json& params) override;
};

/**
 * @brief assets.listutxos - List asset UTXOs
 *
 * Parameters:
 *   [0] address (optional): Filter by owner
 *   [1] asset_id (optional): Filter by asset
 *   [2] options (optional): {
 *         "include_spent": boolean,
 *         "min_amount": number,
 *         "max_amount": number,
 *         "min_height": number,
 *         "max_height": number,
 *         "limit": number,
 *         "offset": number
 *       }
 *
 * Returns:
 *   {
 *     "utxos": [{
 *       "txid": "hex",
 *       "vout": number,
 *       "asset_id": "hex",
 *       "amount": number,
 *       "state_hash": "hex",
 *       "script_pubkey": "hex",
 *       "address": "string",
 *       "height": number,
 *       "timestamp": number,
 *       "is_spent": boolean,
 *       "spending_txid": "hex or null"
 *     }],
 *     "total": number
 *   }
 */
class AssetsListUTXOs : public RPCMethod {
public:
    AssetsListUTXOs();
    nlohmann::json execute(const nlohmann::json& params) override;
};

/**
 * @brief assets.getholders - Get top holders of an asset
 *
 * Parameters:
 *   [0] asset_id: Asset ID
 *   [1] limit (optional): Max results (default 100)
 *
 * Returns:
 *   {
 *     "asset_id": "hex",
 *     "holders": [{
 *       "address": "string",
 *       "balance": number,
 *       "percentage": number,
 *       "utxo_count": number
 *     }],
 *     "total_holders": number
 *   }
 */
class AssetsGetHolders : public RPCMethod {
public:
    AssetsGetHolders();
    nlohmann::json execute(const nlohmann::json& params) override;
};

// ============================================================================
// Asset Issuance Methods
// ============================================================================

/**
 * @brief assets.createasset - Create a new asset
 *
 * Parameters:
 *   [0] metadata: {
 *         "name": "string (required, max 64 chars)",
 *         "ticker": "string (required, max 8 chars)",
 *         "decimals": number (0-18, default 8),
 *         "description": "string (optional, max 256 chars)",
 *         "icon_url": "string (optional)"
 *       }
 *   [1] supply: {
 *         "model": "fixed|capped|unlimited" (default "fixed"),
 *         "initial_supply": number (required),
 *         "max_supply": number (optional, 0 = unlimited),
 *         "burn_enabled": boolean (default true)
 *       }
 *   [2] mint_authority (optional): Pubkey for mint control
 *
 * Returns:
 *   {
 *     "asset_id": "hex",
 *     "genesis_txid": "hex",
 *     "raw_tx": "hex (unsigned)",
 *     "fee": number
 *   }
 */
class AssetsCreateAsset : public RPCMethod {
public:
    AssetsCreateAsset();
    nlohmann::json execute(const nlohmann::json& params) override;
};

/**
 * @brief assets.mint - Mint additional supply (requires authority)
 *
 * Parameters:
 *   [0] asset_id: Asset to mint
 *   [1] amount: Amount to mint
 *   [2] to_address: Recipient address
 *   [3] authority_key (optional): Private key for signing
 *
 * Returns:
 *   {
 *     "txid": "hex",
 *     "raw_tx": "hex (unsigned if no key provided)",
 *     "amount_minted": number,
 *     "new_supply": number,
 *     "fee": number
 *   }
 */
class AssetsMint : public RPCMethod {
public:
    AssetsMint();
    nlohmann::json execute(const nlohmann::json& params) override;
};

/**
 * @brief assets.burn - Burn asset supply
 *
 * Parameters:
 *   [0] asset_id: Asset to burn
 *   [1] amount: Amount to burn
 *   [2] source_utxo (optional): Specific UTXO to burn from
 *
 * Returns:
 *   {
 *     "txid": "hex",
 *     "raw_tx": "hex",
 *     "amount_burned": number,
 *     "new_supply": number,
 *     "fee": number
 *   }
 */
class AssetsBurn : public RPCMethod {
public:
    AssetsBurn();
    nlohmann::json execute(const nlohmann::json& params) override;
};

// ============================================================================
// Asset Transfer Methods
// ============================================================================

/**
 * @brief assets.createtransfer - Create an asset transfer transaction
 *
 * Parameters:
 *   [0] sources: [{
 *         "txid": "hex",
 *         "vout": number
 *       }] - UTXOs to spend
 *   [1] destinations: [{
 *         "address": "string",
 *         "asset_id": "hex",
 *         "amount": number
 *       }]
 *   [2] options (optional): {
 *         "fee_rate": number (sat/vB, default 1),
 *         "change_address": "string"
 *       }
 *
 * Returns:
 *   {
 *     "raw_tx": "hex (unsigned)",
 *     "ctv_hash": "hex",
 *     "inputs": [{
 *       "txid": "hex",
 *       "vout": number,
 *       "asset_id": "hex",
 *       "amount": number
 *     }],
 *     "outputs": [{
 *       "address": "string",
 *       "asset_id": "hex",
 *       "amount": number
 *     }],
 *     "change": [{
 *       "address": "string",
 *       "asset_id": "hex",
 *       "amount": number
 *     }],
 *     "fee": number,
 *     "vsize": number
 *   }
 */
class AssetsCreateTransfer : public RPCMethod {
public:
    AssetsCreateTransfer();
    nlohmann::json execute(const nlohmann::json& params) override;
};

/**
 * @brief assets.signtransfer - Sign an asset transfer
 *
 * Parameters:
 *   [0] raw_tx: Unsigned transaction hex
 *   [1] private_keys: [{
 *         "input_index": number,
 *         "key": "hex (32 bytes)"
 *       }]
 *
 * Returns:
 *   {
 *     "signed_tx": "hex",
 *     "complete": boolean,
 *     "inputs_signed": number
 *   }
 */
class AssetsSignTransfer : public RPCMethod {
public:
    AssetsSignTransfer();
    nlohmann::json execute(const nlohmann::json& params) override;
};

/**
 * @brief assets.sendtransfer - Create, sign, and broadcast transfer
 *
 * Parameters:
 *   [0] asset_id: Asset to send
 *   [1] to_address: Recipient address
 *   [2] amount: Amount to send
 *   [3] options (optional): {
 *         "fee_rate": number,
 *         "from_address": "string (use specific address UTXOs)"
 *       }
 *
 * Returns:
 *   {
 *     "txid": "hex",
 *     "amount_sent": number,
 *     "fee": number,
 *     "change": number
 *   }
 */
class AssetsSendTransfer : public RPCMethod {
public:
    AssetsSendTransfer();
    nlohmann::json execute(const nlohmann::json& params) override;
};

// ============================================================================
// Asset History Methods
// ============================================================================

/**
 * @brief assets.gethistory - Get transfer history
 *
 * Parameters:
 *   [0] address: Address to query
 *   [1] asset_id (optional): Filter by asset
 *   [2] options (optional): {
 *         "limit": number (default 100),
 *         "offset": number (default 0),
 *         "direction": "sent|received|all" (default "all")
 *       }
 *
 * Returns:
 *   {
 *     "address": "string",
 *     "transfers": [{
 *       "txid": "hex",
 *       "asset_id": "hex",
 *       "ticker": "string",
 *       "amount": number,
 *       "direction": "sent|received",
 *       "counterparty": "address",
 *       "height": number,
 *       "timestamp": number,
 *       "confirmations": number
 *     }],
 *     "total": number
 *   }
 */
class AssetsGetHistory : public RPCMethod {
public:
    AssetsGetHistory();
    nlohmann::json execute(const nlohmann::json& params) override;
};

/**
 * @brief assets.gettransfer - Get transfer details
 *
 * Parameters:
 *   [0] txid: Transaction ID
 *
 * Returns:
 *   {
 *     "txid": "hex",
 *     "status": "confirmed|unconfirmed",
 *     "height": number,
 *     "timestamp": number,
 *     "confirmations": number,
 *     "inputs": [{
 *       "txid": "hex",
 *       "vout": number,
 *       "asset_id": "hex",
 *       "amount": number,
 *       "address": "string"
 *     }],
 *     "outputs": [{
 *       "vout": number,
 *       "asset_id": "hex",
 *       "amount": number,
 *       "address": "string"
 *     }],
 *     "fee": number
 *   }
 */
class AssetsGetTransfer : public RPCMethod {
public:
    AssetsGetTransfer();
    nlohmann::json execute(const nlohmann::json& params) override;
};

// ============================================================================
// Asset Validation Methods
// ============================================================================

/**
 * @brief assets.validatetransfer - Validate a transfer without broadcasting
 *
 * Parameters:
 *   [0] raw_tx: Transaction hex to validate
 *
 * Returns:
 *   {
 *     "valid": boolean,
 *     "error": "string (if invalid)",
 *     "error_code": number,
 *     "details": {
 *       "conservation_check": boolean,
 *       "signature_check": boolean,
 *       "ctv_check": boolean,
 *       "utxo_check": boolean
 *     }
 *   }
 */
class AssetsValidateTransfer : public RPCMethod {
public:
    AssetsValidateTransfer();
    nlohmann::json execute(const nlohmann::json& params) override;
};

/**
 * @brief assets.decodeassettx - Decode asset transaction
 *
 * Parameters:
 *   [0] raw_tx: Transaction hex
 *
 * Returns:
 *   {
 *     "version": number,
 *     "is_asset_tx": boolean,
 *     "transition_type": "transfer|mint|burn|contract",
 *     "inputs": [...],
 *     "outputs": [...],
 *     "proofs": [...],
 *     "scripts": [...]
 *   }
 */
class AssetsDecodeAssetTx : public RPCMethod {
public:
    AssetsDecodeAssetTx();
    nlohmann::json execute(const nlohmann::json& params) override;
};

// ============================================================================
// Asset Stats Methods
// ============================================================================

/**
 * @brief assets.getstats - Get asset registry statistics
 *
 * Returns:
 *   {
 *     "total_assets": number,
 *     "total_utxos": number,
 *     "spent_utxos": number,
 *     "total_transfers": number,
 *     "total_mints": number,
 *     "total_burns": number,
 *     "last_indexed_height": number
 *   }
 */
class AssetsGetStats : public RPCMethod {
public:
    AssetsGetStats();
    nlohmann::json execute(const nlohmann::json& params) override;
};

// ============================================================================
// Registration
// ============================================================================

/**
 * @brief Register all asset RPC methods
 * @param dispatcher RPC dispatcher to register with
 */
void RegisterAssetMethods(class RPCDispatcher& dispatcher);

} // namespace rpc
} // namespace dinero
