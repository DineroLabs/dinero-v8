#include "grpc/blockchain_client.h"
#include "primitives/block.h"
#include "wallet/transaction.h"
#include "storage/chain_db.h"  // For Coin struct
#include "consensus/outpoint.h"
#include "primitives/uint256.h"
#include "common/status.h"
#include "common/logger.h"
#include "common/serialization.h"
#include <grpcpp/create_channel.h>

namespace dinero {
namespace grpc_client {

BlockchainClient::BlockchainClient(const std::string& server_address)
    : m_server_address(server_address)
{
    // Create gRPC channel (insecure for local communication)
    // TODO: Add TLS for production deployments
    m_channel = grpc::CreateChannel(
        server_address,
        grpc::InsecureChannelCredentials()
    );

    // Create stub
    m_stub = dinerod::Blockchain::NewStub(m_channel);

    g_logger.info("BlockchainClient connected to " + server_address);
}

StatusOr<uint64_t> BlockchainClient::GetBlockHeight() {
    ::grpc::ClientContext context;
    ::dinerod::EmptyRequest request;
    ::dinerod::BlockHeightResponse response;

    ::grpc::Status status = m_stub->GetBlockHeight(&context, request, &response);

    if (!status.ok()) {
        g_logger.error("GetBlockHeight RPC failed: " + status.error_message());
        return StatusOr<uint64_t>(Status::Io);
    }

    return StatusOr<uint64_t>(response.height());
}

StatusOr<uint256> BlockchainClient::GetBlockHash(uint64_t height) {
    ::grpc::ClientContext context;
    ::dinerod::BlockHashRequest request;
    ::dinerod::BlockHashResponse response;

    request.set_height(height);

    ::grpc::Status status = m_stub->GetBlockHash(&context, request, &response);

    if (!status.ok()) {
        g_logger.error("GetBlockHash RPC failed: " + status.error_message());
        return StatusOr<uint256>(Status::Io);
    }

    if (!response.found()) {
        return StatusOr<uint256>(Status::NotFound);
    }

    // Convert bytes to uint256
    if (response.hash().size() != 32) {
        g_logger.error("GetBlockHash returned invalid hash size");
        return StatusOr<uint256>(Status::Corruption);
    }

    uint256 hash;
    std::memcpy(hash.begin(), response.hash().data(), 32);
    return StatusOr<uint256>(hash);
}

StatusOr<Block> BlockchainClient::GetBlockByHeight(uint64_t height) {
    ::grpc::ClientContext context;
    ::dinerod::GetBlockRequest request;
    ::dinerod::GetBlockResponse response;

    request.set_height(height);

    ::grpc::Status status = m_stub->GetBlock(&context, request, &response);

    if (!status.ok()) {
        g_logger.error("GetBlock RPC failed: " + status.error_message());
        return StatusOr<Block>(Status::Io);
    }

    if (!response.found()) {
        return StatusOr<Block>(Status::NotFound);
    }

    // Deserialize block from raw bytes
    std::vector<uint8_t> raw_block(response.raw_block().begin(), response.raw_block().end());
    auto block_opt = Block::Deserialize(raw_block);
    if (!block_opt.has_value()) {
        g_logger.error("Failed to deserialize block: Block::Deserialize returned null");
        return StatusOr<Block>(Status::Serialization);
    }

    return StatusOr<Block>(*block_opt);
}

StatusOr<Block> BlockchainClient::GetBlockByHash(const uint256& hash) {
    ::grpc::ClientContext context;
    ::dinerod::GetBlockRequest request;
    ::dinerod::GetBlockResponse response;

    request.set_hash(hash.begin(), 32);

    ::grpc::Status status = m_stub->GetBlock(&context, request, &response);

    if (!status.ok()) {
        g_logger.error("GetBlock RPC failed: " + status.error_message());
        return StatusOr<Block>(Status::Io);
    }

    if (!response.found()) {
        return StatusOr<Block>(Status::NotFound);
    }

    // Deserialize block from raw bytes
    std::vector<uint8_t> raw_block(response.raw_block().begin(), response.raw_block().end());
    auto block_opt = Block::Deserialize(raw_block);
    if (!block_opt.has_value()) {
        g_logger.error("Failed to deserialize block: Block::Deserialize returned null");
        return StatusOr<Block>(Status::Serialization);
    }

    return StatusOr<Block>(*block_opt);
}

StatusOr<uint32_t> BlockchainClient::GetConfirmationCount(const uint256& txid) {
    ::grpc::ClientContext context;
    ::dinerod::TxIdRequest request;
    ::dinerod::ConfirmationCountResponse response;

    request.set_txid(txid.begin(), 32);

    ::grpc::Status status = m_stub->GetConfirmationCount(&context, request, &response);

    if (!status.ok()) {
        g_logger.error("GetConfirmationCount RPC failed: " + status.error_message());
        return StatusOr<uint32_t>(Status::Io);
    }

    // ConfirmationCountResponse doesn't have 'found' field - it just returns confirmations
    // 0 confirmations means in mempool or not found
    return StatusOr<uint32_t>(response.confirmations());
}

StatusOr<Coin> BlockchainClient::GetUTXO(const uint256& txid, uint32_t vout) {
    ::grpc::ClientContext context;
    ::dinerod::OutPointRequest request;
    ::dinerod::UTXOResponse response;

    request.set_txid(txid.begin(), 32);
    request.set_vout(vout);

    ::grpc::Status status = m_stub->GetUTXO(&context, request, &response);

    if (!status.ok()) {
        g_logger.error("GetUTXO RPC failed: " + status.error_message());
        return StatusOr<Coin>(Status::Io);
    }

    // UTXOResponse uses 'exists' not 'found'
    if (!response.exists()) {
        return StatusOr<Coin>(Status::NotFound);
    }

    // Convert protobuf UTXO to Coin
    Coin coin;
    coin.amount = response.value();
    coin.script_pubkey = std::string(response.script_pubkey().begin(), response.script_pubkey().end());
    coin.height = 0;  // Height not provided in proto response
    coin.coinbase = false;

    return StatusOr<Coin>(coin);
}

StatusOr<std::vector<Coin>> BlockchainClient::GetUTXOs(const std::vector<OutPoint>& outpoints) {
    ::grpc::ClientContext context;
    ::dinerod::OutPointsRequest request;
    ::dinerod::UTXOsResponse response;

    // Convert OutPoints to protobuf format
    for (const auto& outpoint : outpoints) {
        auto* pb_outpoint = request.add_outpoints();
        pb_outpoint->set_txid(outpoint.txid.v.data, 32);  // Phase M.4: Access TxId internal data
        pb_outpoint->set_vout(outpoint.vout);
    }

    ::grpc::Status status = m_stub->GetUTXOs(&context, request, &response);

    if (!status.ok()) {
        g_logger.error("GetUTXOs RPC failed: " + status.error_message());
        return StatusOr<std::vector<Coin>>(Status::Io);
    }

    // Convert results to Coin vector
    std::vector<Coin> coins;
    coins.reserve(response.utxos_size());

    for (int i = 0; i < response.utxos_size(); ++i) {
        const auto& pb_utxo = response.utxos(i);

        // UTXOResponse uses 'exists' not 'found'
        if (!pb_utxo.exists()) {
            // UTXO not found - Lightning should handle missing UTXOs gracefully
            continue;
        }

        Coin coin;
        coin.amount = pb_utxo.value();
        coin.script_pubkey = std::string(pb_utxo.script_pubkey().begin(), pb_utxo.script_pubkey().end());
        coin.height = 0;
        coin.coinbase = false;
        coins.push_back(coin);
    }

    return StatusOr<std::vector<Coin>>(coins);
}

StatusOr<Transaction> BlockchainClient::GetTransaction(const uint256& txid) {
    ::grpc::ClientContext context;
    ::dinerod::TxIdRequest request;
    ::dinerod::TransactionResponse response;

    request.set_txid(txid.begin(), 32);

    ::grpc::Status status = m_stub->GetTransaction(&context, request, &response);

    if (!status.ok()) {
        g_logger.error("GetTransaction RPC failed: " + status.error_message());
        return StatusOr<Transaction>(Status::Io);
    }

    if (!response.found()) {
        return StatusOr<Transaction>(Status::NotFound);
    }

    // Deserialize transaction from raw bytes
    std::vector<uint8_t> raw_tx(response.raw_tx().begin(), response.raw_tx().end());
    Reader reader(raw_tx);

    Transaction tx;
    try {
        Deserialize(reader, tx);
    } catch (const std::exception& e) {
        g_logger.error("Failed to deserialize transaction: " + std::string(e.what()));
        return StatusOr<Transaction>(Status::Serialization);
    }

    return StatusOr<Transaction>(tx);
}

bool BlockchainClient::IsConnected() {
    // Try to get block height as a health check
    ::grpc::ClientContext context;
    ::dinerod::EmptyRequest request;
    ::dinerod::BlockHeightResponse response;

    // Set short timeout for health check (1 second)
    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(1);
    context.set_deadline(deadline);

    ::grpc::Status status = m_stub->GetBlockHeight(&context, request, &response);

    return status.ok();
}

} // namespace grpc_client
} // namespace dinero
