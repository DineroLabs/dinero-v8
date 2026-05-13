#pragma once

#include "dinerod.grpc.pb.h"
#include <grpcpp/grpcpp.h>

namespace dinero {
    class ChainDB;  // Forward declaration
    class BlockStorage;
}

namespace dinero {
namespace grpc_server {

/**
 * BlockchainServiceImpl - gRPC service for blockchain queries
 *
 * Implements the Blockchain service defined in proto/dinerod.proto.
 * Provides Lightning Network (and other clients) with blockchain data:
 * - Block queries (by height or hash)
 * - Transaction lookups
 * - UTXO queries (single and batch)
 * - Confirmation counts
 *
 * This service is READ-ONLY and does not modify blockchain state.
 * All queries are served from the consensus ChainDB.
 */
class BlockchainServiceImpl final : public dinerod::Blockchain::Service {
public:
    /**
     * Construct BlockchainService with ChainDB
     *
     * @param db  ChainDB instance for blockchain metadata queries
     * @param block_storage BlockStorage instance for flatfile block bodies
     */
    explicit BlockchainServiceImpl(ChainDB* db, BlockStorage* block_storage = nullptr);
    ~BlockchainServiceImpl() override = default;

    // Blockchain service methods (from dinerod.proto)

    /**
     * Get block by height or hash
     *
     * Returns serialized block data with metadata.
     * Supports querying by either height OR hash (using oneof).
     */
    ::grpc::Status GetBlock(
        ::grpc::ServerContext* context,
        const ::dinerod::GetBlockRequest* request,
        ::dinerod::GetBlockResponse* response
    ) override;

    /**
     * Get current blockchain height
     *
     * Returns the height of the most recent block in the active chain.
     */
    ::grpc::Status GetBlockHeight(
        ::grpc::ServerContext* context,
        const ::dinerod::EmptyRequest* request,
        ::dinerod::BlockHeightResponse* response
    ) override;

    /**
     * Get block hash at specific height
     *
     * Used for reorg detection and chain synchronization.
     */
    ::grpc::Status GetBlockHash(
        ::grpc::ServerContext* context,
        const ::dinerod::BlockHashRequest* request,
        ::dinerod::BlockHashResponse* response
    ) override;

    /**
     * Check transaction confirmation status
     *
     * Returns number of confirmations (0 if unconfirmed/not found).
     */
    ::grpc::Status GetConfirmationCount(
        ::grpc::ServerContext* context,
        const ::dinerod::TxIdRequest* request,
        ::dinerod::ConfirmationCountResponse* response
    ) override;

    /**
     * Get UTXO for given outpoint
     *
     * Returns UTXO data if it exists and is unspent.
     */
    ::grpc::Status GetUTXO(
        ::grpc::ServerContext* context,
        const ::dinerod::OutPointRequest* request,
        ::dinerod::UTXOResponse* response
    ) override;

    /**
     * Get multiple UTXOs in batch
     *
     * More efficient than multiple GetUTXO calls.
     * Returns results in same order as request.
     */
    ::grpc::Status GetUTXOs(
        ::grpc::ServerContext* context,
        const ::dinerod::OutPointsRequest* request,
        ::dinerod::UTXOsResponse* response
    ) override;

    /**
     * Get raw transaction by txid
     *
     * Returns serialized transaction if found in blockchain or mempool.
     */
    ::grpc::Status GetTransaction(
        ::grpc::ServerContext* context,
        const ::dinerod::TxIdRequest* request,
        ::dinerod::TransactionResponse* response
    ) override;

private:
    ChainDB* m_db;
    BlockStorage* m_block_storage;
};

} // namespace grpc_server
} // namespace dinero
