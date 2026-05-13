#pragma once

#include "grpc/mempool_client.h"
#include "primitives/uint256.h"
#include "wallet/transaction.h"
#include <memory>

namespace lightningd {

/**
 * MempoolAdapter - Adapter to make MempoolClient look like MempoolService
 *
 * This adapter allows Lightning components to use the same API they expect
 * (from MempoolService) but backed by gRPC calls to dinerod instead of
 * direct mempool access.
 *
 * Only implements the subset of MempoolService API that Lightning actually uses.
 */
class MempoolAdapter {
public:
    /**
     * Construct adapter wrapping a MempoolClient
     * @param mempool_client gRPC client for mempool operations
     */
    explicit MempoolAdapter(dinero::grpc_client::MempoolClient* mempool_client)
        : m_mempool_client(mempool_client)
    {
    }

    /**
     * Add transaction to mempool and optionally relay to network
     * @param tx Transaction to add
     * @param relay Whether to relay to peers (default: true)
     * @return true if transaction was added successfully, false otherwise
     *
     * Delegates to MempoolClient::BroadcastTransaction() via gRPC
     */
    bool addTransaction(const dinero::Transaction& tx, bool relay = true) {
        if (!m_mempool_client) {
            return false;
        }

        auto result = m_mempool_client->BroadcastTransaction(tx);
        return result.ok();
    }

    /**
     * Check if transaction exists in mempool
     * @param txid Transaction ID to check
     * @return true if transaction is in mempool, false otherwise
     *
     * Delegates to MempoolClient::IsInMempool() via gRPC
     */
    bool hasTransaction(const dinero::uint256& txid) const {
        if (!m_mempool_client) {
            return false;
        }

        auto result = m_mempool_client->IsInMempool(txid);
        return result.ok() && result.value();  // Returns true if in mempool
    }

private:
    dinero::grpc_client::MempoolClient* m_mempool_client;
};

} // namespace lightningd
