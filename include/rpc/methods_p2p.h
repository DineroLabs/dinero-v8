#pragma once

namespace din {
namespace rpc {

/**
 * P2P Marketplace RPC Methods
 *
 * Provides decentralized orderbook for buying/selling DIN with fiat
 * Uses time-locked escrow for trustless proof-of-ownership
 *
 * Methods:
 * - p2p.createoffer <type> <amount> <price_usd> <payment_methods> [duration_hours] [notes]
 * - p2p.listoffers [type] [max_count]
 * - p2p.acceptoffer <offer_id>
 * - p2p.verifyoffer <offer_id>
 * - p2p.canceloffer <offer_id>
 * - p2p.completeoffer <offer_id>
 * - p2p.getoffer <offer_id>
 * - p2p.bestoffers <type> [limit]
 * - p2p.escrowinfo <escrow_id>
 * - p2p.releaseescrow <offer_id>
 */

/**
 * Register all P2P RPC methods (legacy - not used)
 */
// void registerP2PRPC();

/**
 * Register all P2P RPC methods (vNext with metadata)
 */
void registerP2PMethodsVNext();

} // namespace rpc
} // namespace din
