#include "grpc/blockchain_service.h"
#include "storage/chain_db.h"
#include "storage/archival_block_reader.h"
#include "consensus/block_index.h"
#include "primitives/block.h"
#include "primitives/uint256.h"
#include "wallet/transaction.h"
#include "common/logger.h"

namespace dinero {
namespace grpc_server {

BlockchainServiceImpl::BlockchainServiceImpl(ChainDB* db, BlockStorage* block_storage)
    : m_db(db)
    , m_block_storage(block_storage)
{
    g_logger.info("BlockchainService initialized");
}

::grpc::Status BlockchainServiceImpl::GetBlock(
    ::grpc::ServerContext* context,
    const ::dinerod::GetBlockRequest* request,
    ::dinerod::GetBlockResponse* response
) {
    try {
        ChainDB* db = m_db;
        if (!db) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "ChainDB not available");
        }

        uint256 block_hash;

        // Handle oneof: query by height OR hash
        if (request->has_height()) {
            // Query by height
            uint64_t height = request->height();
            auto hash_result = db->getBlockHashByHeight(static_cast<int>(height));
            if (!hash_result.ok()) {
                response->set_found(false);
                return ::grpc::Status::OK;
            }
            block_hash = hash_result.value();
        } else if (request->hash().size() == 32) {
            // Query by hash
            std::memcpy(block_hash.begin(), request->hash().data(), 32);
        } else {
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                  "Must specify either height or 32-byte hash");
        }

        // Get block from database
        auto block_result = storage::ReadArchivalBlock(*db, m_block_storage, block_hash);
        if (!block_result.ok()) {
            response->set_found(false);
            return ::grpc::Status::OK;
        }

        const Block& block = block_result.value();

        // Get block metadata
        auto height_result = db->getBlockHeight(block_hash);
        uint64_t height = height_result.ok() ? height_result.value() : 0;

        auto tip_result = db->getTip();
        uint32_t confirmations = 0;
        if (tip_result.ok()) {
            int tip_height = tip_result.value().height;
            if (height <= static_cast<uint64_t>(tip_height)) {
                confirmations = tip_height - height + 1;
            }
        }

        // Fill response
        response->set_found(true);
        response->set_raw_block(block.Serialize());
        response->set_height(height);
        response->set_hash(block_hash.begin(), 32);
        response->set_confirmations(confirmations);
        response->set_timestamp(block.header.timestamp);

        // Previous hash
        if (!block.header.prev_block_hash.IsNull()) {
            response->set_prev_hash(block.header.prev_block_hash.begin(), 32);
        }

        // Transaction hashes (if requested)
        if (request->include_txs()) {
            for (const auto& tx : block.vtx) {
                TxId txid = tx.GetTxid();  // Phase M.4: GetTxid() returns TxId
                response->add_tx_hashes(txid.v.data, 32);  // Phase M.4: Access internal uint256 data
            }
        }

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("GetBlock exception: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status BlockchainServiceImpl::GetBlockHeight(
    ::grpc::ServerContext* context,
    const ::dinerod::EmptyRequest* request,
    ::dinerod::BlockHeightResponse* response
) {
    try {
        ChainDB* db = m_db;
        if (!db) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "ChainDB not available");
        }

        auto tip_result = db->getTip();
        if (!tip_result.ok()) {
            return ::grpc::Status(::grpc::StatusCode::NOT_FOUND, "Chain tip not found");
        }

        const auto& tip = tip_result.value();
        response->set_height(tip.height);
        response->set_hash(tip.hash.begin(), 32);

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("GetBlockHeight exception: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status BlockchainServiceImpl::GetBlockHash(
    ::grpc::ServerContext* context,
    const ::dinerod::BlockHashRequest* request,
    ::dinerod::BlockHashResponse* response
) {
    try {
        ChainDB* db = m_db;
        if (!db) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "ChainDB not available");
        }

        uint64_t height = request->height();
        auto hash_result = db->getBlockHashByHeight(static_cast<int>(height));

        if (!hash_result.ok()) {
            response->set_found(false);
            return ::grpc::Status::OK;
        }

        const uint256& hash = hash_result.value();
        response->set_found(true);
        response->set_hash(hash.begin(), 32);

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("GetBlockHash exception: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status BlockchainServiceImpl::GetConfirmationCount(
    ::grpc::ServerContext* context,
    const ::dinerod::TxIdRequest* request,
    ::dinerod::ConfirmationCountResponse* response
) {
    try {
        ChainDB* db = m_db;
        if (!db) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "ChainDB not available");
        }

        // Parse txid
        if (request->txid().size() != 32) {
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "txid must be 32 bytes");
        }

        uint256 txid;
        std::memcpy(txid.begin(), request->txid().data(), 32);

        // Check if transaction is in blockchain (via tx index)
        auto tx_loc_result = db->getTxLocation(txid);

        if (!tx_loc_result.ok()) {
            // Not in blockchain - might be in mempool
            // TODO: Check mempool when MempoolService is integrated
            response->set_confirmations(0);
            response->set_in_mempool(false);
            response->set_confirmed(false);
            response->set_block_height(0);
            return ::grpc::Status::OK;
        }

        // Transaction is confirmed
        const auto& [block_hash, offset] = tx_loc_result.value();

        auto height_result = db->getBlockHeight(block_hash);
        if (!height_result.ok()) {
            return ::grpc::Status(::grpc::StatusCode::INTERNAL,
                                  "Found tx location but missing block height");
        }

        int tx_height = height_result.value();

        // Get current tip to calculate confirmations
        auto tip_result = db->getTip();
        if (!tip_result.ok()) {
            return ::grpc::Status(::grpc::StatusCode::INTERNAL, "Chain tip not found");
        }

        int tip_height = tip_result.value().height;
        uint32_t confirmations = (tip_height >= tx_height) ? (tip_height - tx_height + 1) : 0;

        response->set_confirmations(confirmations);
        response->set_in_mempool(false);
        response->set_confirmed(true);
        response->set_block_height(tx_height);
        response->set_block_hash(block_hash.begin(), 32);

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("GetConfirmationCount exception: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status BlockchainServiceImpl::GetUTXO(
    ::grpc::ServerContext* context,
    const ::dinerod::OutPointRequest* request,
    ::dinerod::UTXOResponse* response
) {
    try {
        ChainDB* db = m_db;
        if (!db) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "ChainDB not available");
        }

        // Parse outpoint
        if (request->txid().size() != 32) {
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "txid must be 32 bytes");
        }

        uint256 txid;
        std::memcpy(txid.begin(), request->txid().data(), 32);
        uint32_t vout = request->vout();

        // Query UTXO from database
        auto coin_result = db->getCoin(txid, vout);

        if (!coin_result.ok()) {
            // UTXO does not exist (spent or never existed)
            response->set_exists(false);
            return ::grpc::Status::OK;
        }

        const Coin& coin = coin_result.value();

        // Get current tip for confirmations
        auto tip_result = db->getTip();
        uint32_t confirmations = 0;
        if (tip_result.ok()) {
            int tip_height = tip_result.value().height;
            if (coin.height <= tip_height) {
                confirmations = tip_height - coin.height + 1;
            }
        }

        // Fill response
        response->set_exists(true);
        response->set_value(coin.amount);
        response->set_script_pubkey(coin.script_pubkey);
        response->set_height(coin.height);
        response->set_confirmations(confirmations);
        response->set_is_coinbase(coin.coinbase);

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("GetUTXO exception: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status BlockchainServiceImpl::GetUTXOs(
    ::grpc::ServerContext* context,
    const ::dinerod::OutPointsRequest* request,
    ::dinerod::UTXOsResponse* response
) {
    try {
        ChainDB* db = m_db;
        if (!db) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "ChainDB not available");
        }

        // Get tip once for all confirmations
        auto tip_result = db->getTip();
        int tip_height = tip_result.ok() ? tip_result.value().height : 0;

        // Query each outpoint
        for (const auto& outpoint_req : request->outpoints()) {
            if (outpoint_req.txid().size() != 32) {
                return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                      "All txids must be 32 bytes");
            }

            uint256 txid;
            std::memcpy(txid.begin(), outpoint_req.txid().data(), 32);
            uint32_t vout = outpoint_req.vout();

            auto* utxo_response = response->add_utxos();

            // Query UTXO
            auto coin_result = db->getCoin(txid, vout);

            if (!coin_result.ok()) {
                // UTXO does not exist
                utxo_response->set_exists(false);
                continue;
            }

            const Coin& coin = coin_result.value();

            uint32_t confirmations = 0;
            if (coin.height <= tip_height) {
                confirmations = tip_height - coin.height + 1;
            }

            // Fill response
            utxo_response->set_exists(true);
            utxo_response->set_value(coin.amount);
            utxo_response->set_script_pubkey(coin.script_pubkey);
            utxo_response->set_height(coin.height);
            utxo_response->set_confirmations(confirmations);
            utxo_response->set_is_coinbase(coin.coinbase);
        }

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("GetUTXOs exception: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status BlockchainServiceImpl::GetTransaction(
    ::grpc::ServerContext* context,
    const ::dinerod::TxIdRequest* request,
    ::dinerod::TransactionResponse* response
) {
    try {
        ChainDB* db = m_db;
        if (!db) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "ChainDB not available");
        }

        // Parse txid
        if (request->txid().size() != 32) {
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "txid must be 32 bytes");
        }

        uint256 txid;
        std::memcpy(txid.begin(), request->txid().data(), 32);

        // Try to find transaction in blockchain via tx index
        auto tx_loc_result = db->getTxLocation(txid);

        if (!tx_loc_result.ok()) {
            // Not in blockchain - might be in mempool
            // TODO: Check mempool when MempoolService is integrated
            response->set_found(false);
            return ::grpc::Status::OK;
        }

        const auto& [block_hash, offset] = tx_loc_result.value();

        // Get the block containing this transaction
        auto block_result = storage::ReadArchivalBlock(*db, m_block_storage, block_hash);
        if (!block_result.ok()) {
            return ::grpc::Status(::grpc::StatusCode::INTERNAL,
                                  "Found tx location but cannot read block");
        }

        const Block& block = block_result.value();

        // Find transaction in block
        const Transaction* found_tx = nullptr;
        for (const auto& tx : block.vtx) {
            if (tx.GetTxid().AsUint256() == txid) {  // Phase M.4: Unwrap TxId for comparison
                found_tx = &tx;
                break;
            }
        }

        if (!found_tx) {
            return ::grpc::Status(::grpc::StatusCode::INTERNAL,
                                  "Transaction not found in block despite tx index");
        }

        // Get block height and confirmations
        auto height_result = db->getBlockHeight(block_hash);
        uint64_t height = height_result.ok() ? height_result.value() : 0;

        auto tip_result = db->getTip();
        uint32_t confirmations = 0;
        if (tip_result.ok()) {
            int tip_height = tip_result.value().height;
            if (height <= static_cast<uint64_t>(tip_height)) {
                confirmations = tip_height - height + 1;
            }
        }

        // Fill response
        response->set_found(true);

        // Serialize transaction (returns vector<uint8_t>, convert to string for protobuf)
        std::vector<uint8_t> serialized_tx = found_tx->Serialize();
        response->set_raw_tx(std::string(serialized_tx.begin(), serialized_tx.end()));

        response->set_txid(txid.begin(), 32);
        response->set_confirmations(confirmations);
        response->set_block_height(height);
        response->set_block_hash(block_hash.begin(), 32);

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("GetTransaction exception: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

} // namespace grpc_server
} // namespace dinero
