#pragma once

#include "dinerod.grpc.pb.h"
#include "common/status.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
namespace dinero {
    struct Block;
    struct Transaction;
    struct Coin;
    struct OutPoint;
    class uint256;
}

namespace dinero {
namespace grpc_client {

/**
 * BlockchainClient - gRPC client for blockchain queries
 *
 * Provides a C++ wrapper around the dinerod.Blockchain gRPC service.
 * This client allows lightningd to query blockchain state from dinerod
 * without direct access to ChainDB.
 *
 * Usage:
 *   BlockchainClient client("localhost:50051");
 *   auto height_result = client.GetBlockHeight();
 *   if (height_result.ok()) {
 *       std::cout << "Height: " << height_result.value() << std::endl;
 *   }
 *
 * Thread Safety: This class is NOT thread-safe. Create one instance per thread
 * or use external synchronization.
 */
class BlockchainClient {
public:
    /**
     * Construct BlockchainClient with server address
     *
     * @param server_address  gRPC server address (e.g., "localhost:50051")
     */
    explicit BlockchainClient(const std::string& server_address);

    /**
     * Get current blockchain height
     *
     * @return Current tip height, or error if unavailable
     */
    StatusOr<uint64_t> GetBlockHeight();

    /**
     * Get block hash by height
     *
     * @param height  Block height to query
     * @return Block hash, or error if not found
     */
    StatusOr<uint256> GetBlockHash(uint64_t height);

    /**
     * Get block by height
     *
     * @param height  Block height to query
     * @return Complete block, or error if not found
     */
    StatusOr<Block> GetBlockByHeight(uint64_t height);

    /**
     * Get block by hash
     *
     * @param hash  Block hash to query
     * @return Complete block, or error if not found
     */
    StatusOr<Block> GetBlockByHash(const uint256& hash);

    /**
     * Get confirmation count for a transaction
     *
     * Returns number of confirmations (blocks on top of tx).
     * 0 = in mempool, 1 = in latest block, etc.
     *
     * @param txid  Transaction ID to query
     * @return Confirmation count, or error if not found
     */
    StatusOr<uint32_t> GetConfirmationCount(const uint256& txid);

    /**
     * Get single UTXO
     *
     * @param txid  Transaction ID
     * @param vout  Output index
     * @return UTXO (Coin), or error if spent/not found
     */
    StatusOr<Coin> GetUTXO(const uint256& txid, uint32_t vout);

    /**
     * Get multiple UTXOs in a single request (batch query)
     *
     * This is more efficient than multiple GetUTXO calls.
     * Critical for Lightning which often needs to check many outputs.
     *
     * @param outpoints  List of outpoints to query
     * @return Vector of UTXOs (in same order as outpoints)
     *         Missing UTXOs will have a corresponding error in the result
     */
    StatusOr<std::vector<Coin>> GetUTXOs(const std::vector<OutPoint>& outpoints);

    /**
     * Get transaction by txid
     *
     * Searches both blockchain and mempool.
     *
     * @param txid  Transaction ID to query
     * @return Transaction, or error if not found
     */
    StatusOr<Transaction> GetTransaction(const uint256& txid);

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
    std::unique_ptr<dinerod::Blockchain::Stub> m_stub;
};

} // namespace grpc_client
} // namespace dinero
