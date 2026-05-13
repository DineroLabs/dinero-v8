#pragma once

#include "din_json.h"

namespace dinero {
namespace rpc {

/**
 * RPC Methods for Multi-Asset Escrow
 *
 * Provides JSON-RPC interface for creating and managing escrow contracts
 * with multiple asset types and automatic conversion support.
 */

/**
 * multiasset.createescrow - Create multi-asset escrow contract
 *
 * Parameters:
 * {
 *   "buyer_pubkey": "hex",        // Buyer's public key
 *   "seller_pubkey": "hex",       // Seller's public key
 *   "mediator_pubkey": "hex",     // Mediator's public key
 *   "asset_id": "DIN|BTC|USDT|EUR|USD|...",  // Asset to lock
 *   "amount": 10.5,               // Amount to lock
 *   "refund_blocks": 2880,        // Blocks until refund (~6 days)
 *   "release_asset": "EUR"        // Optional: convert to this asset on release
 * }
 *
 * Returns:
 * {
 *   "contract_id": "contract_...",
 *   "asset_id": "USDT",
 *   "amount": 10.5,
 *   "p2sh_address": "din1q...",
 *   "redeem_script": "hex",
 *   "release_asset": "EUR",       // If conversion enabled
 *   "conversion_route": {...}     // Route details if converting
 * }
 */
din::Json multiasset_createescrow(const din::Json& params);

/**
 * multiasset.releaseescrow - Release escrow with optional conversion
 *
 * Parameters:
 * {
 *   "contract_id": "contract_...",
 *   "to_address": "din1q...",
 *   "sig_buyer": "hex",
 *   "sig_seller": "hex"
 * }
 *
 * Returns:
 * {
 *   "txid": "hex",
 *   "conversion_executed": true/false,
 *   "route_description": "USDT→EUR via SimpleSwap"
 * }
 */
din::Json multiasset_releaseescrow(const din::Json& params);

/**
 * multiasset.refundescrow - Refund escrow (original asset)
 *
 * Parameters:
 * {
 *   "contract_id": "contract_...",
 *   "refund_address": "din1q...",
 *   "sig_buyer": "hex"
 * }
 *
 * Returns:
 * {
 *   "txid": "hex"
 * }
 */
din::Json multiasset_refundescrow(const din::Json& params);

/**
 * multiasset.getcontract - Get contract details
 *
 * Parameters:
 * {
 *   "contract_id": "contract_..."
 * }
 *
 * Returns: Full contract details as JSON
 */
din::Json multiasset_getcontract(const din::Json& params);

/**
 * multiasset.listcontracts - List all contracts (optional filter by asset)
 *
 * Parameters:
 * {
 *   "asset_id": "USDT"  // Optional filter
 * }
 *
 * Returns: Array of contracts
 */
din::Json multiasset_listcontracts(const din::Json& params);

/**
 * multiasset.getconversionroutes - Get available conversion routes
 *
 * Parameters:
 * {
 *   "from_asset": "USDT",
 *   "to_asset": "EUR",
 *   "amount": 100.0
 * }
 *
 * Returns:
 * {
 *   "routes": [
 *     {
 *       "hops": [...],
 *       "total_rate": 0.92,
 *       "total_fee_bps": 50,
 *       "description": "USDT→EUR via SimpleSwap"
 *     }
 *   ]
 * }
 */
din::Json multiasset_getconversionroutes(const din::Json& params);

/**
 * multiasset.estimateconversion - Estimate conversion output
 *
 * Parameters:
 * {
 *   "from_asset": "USDT",
 *   "to_asset": "EUR",
 *   "amount": 100.0
 * }
 *
 * Returns:
 * {
 *   "input_amount": 100.0,
 *   "output_amount": 91.8,
 *   "effective_rate": 0.918,
 *   "route": "USDT→EUR via SimpleSwap"
 * }
 */
din::Json multiasset_estimateconversion(const din::Json& params);

/**
 * multiasset.stats - Get multi-asset escrow statistics
 *
 * Returns:
 * {
 *   "total_contracts": 42,
 *   "active_contracts": 15,
 *   "by_asset": {
 *     "DIN": 10,
 *     "USDT": 20,
 *     "EUR": 12
 *   }
 * }
 */
din::Json multiasset_stats(const din::Json& params);

/**
 * multiasset.supportedassets - List all supported assets
 *
 * Returns:
 * {
 *   "assets": [
 *     {"id": "DIN", "decimals": 8, "name": "Dinero"},
 *     {"id": "BTC", "decimals": 8, "name": "Bitcoin"},
 *     {"id": "USDT", "decimals": 6, "name": "Tether"},
 *     ...
 *   ]
 * }
 */
din::Json multiasset_supportedassets(const din::Json& params);

// Register all multi-asset RPC methods
void register_multiasset_methods();

} // namespace rpc
} // namespace dinero
