#pragma once

#include "dinerod.grpc.pb.h"
#include "common/status.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

// Forward declarations
namespace dinero {
    struct Transaction;
    struct MempoolEntry;
    class uint256;

    // Simple mempool stats struct for client (mirrors server-side struct)
    struct MempoolStats {
        size_t tx_count;
        size_t total_size;
        uint64_t min_fee_rate;
    };
}

namespace dinero {
namespace grpc_client {

/**
 * MempoolClient - gRPC client for mempool operations
 *
 * Provides a C++ wrapper around the dinerod.Mempool gRPC service.
 * This client allows lightningd to broadcast transactions and query
 * mempool state from dinerod without direct access to Mempool.
 *
 * Usage:
 *   MempoolClient client("localhost:50051");
 *   auto result = client.BroadcastTransaction(my_tx);
 *   if (result.ok()) {
 *       std::cout << "Txid: " << result.value().GetHex() << std::endl;
 *   }
 *
 * Thread Safety: This class is NOT thread-safe. Create one instance per thread
 * or use external synchronization.
 */
class MempoolClient {
public:
    /**
     * Construct MempoolClient with server address
     *
     * @param server_address  gRPC server address (e.g., "localhost:50051")
     */
    explicit MempoolClient(const std::string& server_address);

    /**
     * Broadcast transaction to network
     *
     * Validates and broadcasts the transaction. Returns txid on success.
     * Transaction must be valid according to consensus rules.
     *
     * @param tx  Transaction to broadcast
     * @return Transaction ID, or error if rejected
     */
    StatusOr<uint256> BroadcastTransaction(const Transaction& tx);

    /**
     * Estimate fee rate for target confirmation time
     *
     * Returns recommended fee rate in una per vbyte.
     *
     * @param target_blocks  Desired confirmation time in blocks
     *                       1 = next block (IMMEDIATE)
     *                       3 = ~30 min (FAST)
     *                       6 = ~1 hour (NORMAL)
     *                       12 = ~2 hours (SLOW)
     *                       20+ = economy
     * @return Fee rate in sat/vbyte, or error if not available
     */
    StatusOr<uint64_t> EstimateFee(uint32_t target_blocks);

    /**
     * Check if transaction is in mempool
     *
     * @param txid  Transaction ID to check
     * @return true if in mempool (pending), false if not found or confirmed
     */
    StatusOr<bool> IsInMempool(const uint256& txid);

    /**
     * Get mempool transaction details
     *
     * Retrieves full transaction and metadata from mempool.
     *
     * @param txid  Transaction ID to query
     * @return Mempool entry with tx, fee, vsize, time, etc.
     */
    StatusOr<MempoolEntry> GetMempoolTransaction(const uint256& txid);

    /**
     * Get current mempool statistics
     *
     * @return Mempool stats: size, bytes, usage, min_fee
     */
    StatusOr<MempoolStats> GetMempoolInfo();

    /**
     * Check if connection to server is healthy
     *
     * @return true if connected and server is responsive
     */
    bool IsConnected();

    /**
     * Get server address
     *
     * @return Server address string (e.g., "localhost:50051")
     */
    std::string GetServerAddress() const { return m_server_address; }

private:
    std::string m_server_address;
    std::shared_ptr<grpc::Channel> m_channel;
    std::unique_ptr<dinerod::Mempool::Stub> m_stub;
};

} // namespace grpc_client
} // namespace dinero
