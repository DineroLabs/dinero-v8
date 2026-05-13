#pragma once

namespace dinero {
namespace rpc {

/**
 * Smart Contract RPC Methods
 *
 * Provides on-chain escrow smart contracts using Bitcoin Script
 *
 * Methods:
 * - contract.createescrow <buyer_pub> <seller_pub> <mediator_pub> <amount> <refund_blocks>
 * - contract.status <contract_id>
 * - contract.release <contract_id> <to_address> <sig_buyer> <sig_seller>
 * - contract.refund <contract_id> <refund_address> <sig_buyer>
 * - contract.list [address]
 */

/**
 * Register all contract RPC methods
 */
void registerContractRPC();

} // namespace rpc
} // namespace dinero
