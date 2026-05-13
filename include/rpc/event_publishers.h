#pragma once

#include "rpc/event_bus.h"
#include "wallet/transaction.h"
#include <string>
#include <vector>

namespace dinero {
namespace rpc {

/**
 * Helper functions for publishing events from various components
 *
 * These are convenience wrappers around EventBus::publish_* methods
 * to make it easy to integrate event publishing into existing code.
 */

/**
 * Publish transaction received event (entered mempool)
 */
inline void publish_tx_received(const std::string& txid, uint64_t amount, uint64_t fee,
                                const std::vector<std::string>& addresses = {}) {
    EventBus::instance().publish_transaction(EventType::TransactionReceived,
                                            txid, amount, fee, addresses);
}

/**
 * Publish transaction confirmed event (included in block)
 */
inline void publish_tx_confirmed(const std::string& txid, uint64_t amount,
                                 const std::vector<std::string>& addresses = {}) {
    EventBus::instance().publish_transaction(EventType::TransactionConfirmed,
                                            txid, amount, 0, addresses);
}

/**
 * Publish transaction rejected event
 */
inline void publish_tx_rejected(const std::string& txid, const std::string& reason = "") {
    EventBus::instance().publish_transaction(EventType::TransactionRejected,
                                            txid, 0, 0);
}

/**
 * Publish new block event
 */
inline void publish_new_block(const std::string& block_hash, uint32_t height, uint32_t tx_count) {
    EventBus::instance().publish_block(EventType::NewBlock, block_hash, height, tx_count);
}

/**
 * Publish wallet balance change event
 */
inline void publish_balance_change(const std::string& address, uint64_t old_balance, uint64_t new_balance) {
    EventBus::instance().publish_balance_change(address, old_balance, new_balance);
}

/**
 * Publish wallet incoming transaction event
 */
inline void publish_wallet_incoming_tx(const std::string& txid, uint64_t amount,
                                       const std::vector<std::string>& addresses) {
    EventBus::instance().publish_transaction(EventType::WalletIncomingTx,
                                            txid, amount, 0, addresses);
}

/**
 * Publish wallet outgoing transaction event
 */
inline void publish_wallet_outgoing_tx(const std::string& txid, uint64_t amount, uint64_t fee,
                                       const std::vector<std::string>& addresses) {
    EventBus::instance().publish_transaction(EventType::WalletOutgoingTx,
                                            txid, amount, fee, addresses);
}

/**
 * Publish mempool update event
 */
inline void publish_mempool_update(uint32_t tx_count, uint64_t total_size,
                                  double min_fee, double median_fee, double max_fee) {
    EventBus::instance().publish_mempool_update(tx_count, total_size, min_fee, median_fee, max_fee);
}

/**
 * Example integration points for wallet components:
 *
 * 1. In wallet transaction processing:
 *    - After sending transaction: publish_wallet_outgoing_tx(...)
 *    - After receiving transaction: publish_wallet_incoming_tx(...)
 *    - After balance update: publish_balance_change(...)
 *
 * 2. In mempool processing:
 *    - After accepting transaction: publish_tx_received(...)
 *    - After rejecting transaction: publish_tx_rejected(...)
 *    - Periodically: publish_mempool_update(...)
 *
 * 3. In block processing:
 *    - After new block added: publish_new_block(...)
 *    - For each transaction in block: publish_tx_confirmed(...)
 *
 * Example usage:
 *
 *   // In sendtoaddress_impl after transaction sent
 *   publish_wallet_outgoing_tx(txid, amount, fee, {to_address});
 *
 *   // In mempool after accepting transaction
 *   publish_tx_received(txid, total_out, fee, affected_addresses);
 *
 *   // In block processor after new block
 *   publish_new_block(block.GetHash(), block.nHeight, block.vtx.size());
 *   for (const auto& tx : block.vtx) {
 *       publish_tx_confirmed(tx.GetTxid(), tx.GetTotalOut());
 *   }
 */

} // namespace rpc
} // namespace dinero
