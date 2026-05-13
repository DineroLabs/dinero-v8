#include "grpc/mempool_service.h"
#include "daemon/mempool.h"
#include "daemon/interfaces/tx_ingress.h"  // Step 5: ITxIngress interface
#include "daemon/interfaces/origin.h"      // Step 5: TxOrigin enum
#include "policy/fee_estimator.h"
#include "wallet/transaction.h"
#include "primitives/uint256.h"
#include "common/logger.h"

namespace dinero {
namespace grpc_server {

MempoolServiceImpl::MempoolServiceImpl(dinero::Mempool* mempool,
                                       dinero::policy::FeeEstimator* fee_estimator,
                                       dinero::ITxIngress* tx_ingress)
    : m_mempool(mempool)
    , m_fee_estimator(fee_estimator)
    , m_tx_ingress(tx_ingress)
{
    g_logger.info("MempoolService initialized");
}

::grpc::Status MempoolServiceImpl::BroadcastTransaction(
    ::grpc::ServerContext* context,
    const ::dinerod::RawTxRequest* request,
    ::dinerod::TxBroadcastResponse* response
) {
    try {
        // Step 5: Prefer ITxIngress interface, fall back to direct mempool
        if (!m_tx_ingress && !m_mempool) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "Transaction ingress not available");
        }

        // Deserialize transaction from raw bytes
        std::vector<uint8_t> raw_tx_bytes(request->raw_tx().begin(), request->raw_tx().end());
        Transaction tx;

        if (!TransactionSerializer::Deserialize(tx, raw_tx_bytes)) {
            response->set_success(false);
            response->set_error("Failed to deserialize transaction");
            response->set_reason(::dinerod::TxBroadcastResponse_RejectReason_INVALID);
            return ::grpc::Status::OK;
        }

        // Get transaction ID
        uint256 txid = tx.GetTxid().AsUint256();  // Phase M.4: Unwrap TxId to uint256

        // Step 5: Use ITxIngress for duplicate check and submission
        TxAcceptResult submit_result;

        if (m_tx_ingress) {
            // Check if already in mempool via interface
            if (m_tx_ingress->HasTransaction(txid)) {
                response->set_success(false);
                response->set_error("Transaction already in mempool");
                response->set_reason(::dinerod::TxBroadcastResponse_RejectReason_DUPLICATE);
                response->set_txid(txid.begin(), 32);
                return ::grpc::Status::OK;
            }

            // Submit via canonical interface (TxOrigin::GRPC auto-relays)
            submit_result = m_tx_ingress->Submit(tx, TxOrigin::GRPC);
        } else {
            // Fallback to direct mempool (legacy path)
            if (m_mempool->hasTransaction(txid)) {
                response->set_success(false);
                response->set_error("Transaction already in mempool");
                response->set_reason(::dinerod::TxBroadcastResponse_RejectReason_DUPLICATE);
                response->set_txid(txid.begin(), 32);
                return ::grpc::Status::OK;
            }

            submit_result = m_mempool->submitTransaction(tx, "grpc:broadcast", true);
        }

        if (submit_result.rejected()) {
            response->set_success(false);
            response->set_error(std::string(dinero::TxRejectCodeToString(submit_result.code)) +
                              ": " + submit_result.message);
            response->set_reason(::dinerod::TxBroadcastResponse_RejectReason_INVALID);
            return ::grpc::Status::OK;
        }

        // Success
        response->set_success(true);
        response->set_txid(txid.begin(), 32);
        response->set_reason(::dinerod::TxBroadcastResponse_RejectReason_ACCEPTED);

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("BroadcastTransaction exception: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status MempoolServiceImpl::EstimateFee(
    ::grpc::ServerContext* context,
    const ::dinerod::FeeEstimateRequest* request,
    ::dinerod::FeeEstimateResponse* response
) {
    try {
        if (!m_fee_estimator) {
            // No fee estimator available - return unavailable
            response->set_available(false);
            response->set_sat_per_vbyte(0);
            response->set_blocks(request->target_blocks());
            return ::grpc::Status::OK;
        }

        // Map target blocks to FeeTarget
        uint32_t target_blocks = request->target_blocks();
        if (target_blocks == 0) {
            target_blocks = 6;  // Default to NORMAL
        }
        if (target_blocks > 1000) {
            target_blocks = 1000;  // Cap at maximum
        }

        // Select appropriate fee target
        policy::FeeTarget fee_target;
        if (target_blocks == 1) {
            fee_target = policy::FeeTarget::IMMEDIATE;
        } else if (target_blocks <= 3) {
            fee_target = policy::FeeTarget::FAST;
        } else if (target_blocks <= 6) {
            fee_target = policy::FeeTarget::NORMAL;
        } else if (target_blocks <= 12) {
            fee_target = policy::FeeTarget::SLOW;
        } else {
            fee_target = policy::FeeTarget::ECONOMY;
        }

        // Get estimate from fee estimator
        policy::FeeEstimate estimate = m_fee_estimator->estimateFee(fee_target);

        if (!estimate.is_sufficient_data) {
            // Not enough data for reliable estimate
            response->set_available(false);
            response->set_sat_per_vbyte(0);
            response->set_blocks(target_blocks);
            return ::grpc::Status::OK;
        }

        // Convert from una/KB to una/vbyte
        // fee_rate is in sat/KB, so divide by 1000 to get sat/byte (vbyte)
        uint64_t sat_per_vbyte = estimate.fee_rate / 1000;
        if (sat_per_vbyte == 0) {
            sat_per_vbyte = 1;  // Minimum 1 sat/vbyte
        }

        response->set_available(true);
        response->set_sat_per_vbyte(sat_per_vbyte);
        response->set_blocks(target_blocks);

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("EstimateFee exception: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status MempoolServiceImpl::IsInMempool(
    ::grpc::ServerContext* context,
    const ::dinerod::TxIdRequest* request,
    ::dinerod::BoolResponse* response
) {
    try {
        if (!m_mempool) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "Mempool not available");
        }

        // Parse txid
        if (request->txid().size() != 32) {
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "txid must be 32 bytes");
        }

        uint256 txid;
        std::memcpy(txid.begin(), request->txid().data(), 32);

        // Check if in mempool
        bool in_mempool = m_mempool->hasTransaction(txid);
        response->set_value(in_mempool);

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("IsInMempool exception: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status MempoolServiceImpl::GetMempoolTransaction(
    ::grpc::ServerContext* context,
    const ::dinerod::TxIdRequest* request,
    ::dinerod::MempoolTxResponse* response
) {
    try {
        if (!m_mempool) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "Mempool not available");
        }

        // Parse txid
        if (request->txid().size() != 32) {
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "txid must be 32 bytes");
        }

        uint256 txid;
        std::memcpy(txid.begin(), request->txid().data(), 32);

        // Get mempool entry
        auto entry_opt = m_mempool->getMempoolEntry(txid);

        if (!entry_opt.has_value()) {
            response->set_found(false);
            return ::grpc::Status::OK;
        }

        const MempoolEntry& entry = entry_opt.value();

        // Serialize transaction
        std::vector<uint8_t> serialized_tx = entry.tx.Serialize();

        // Fill response
        response->set_found(true);
        response->set_raw_tx(std::string(serialized_tx.begin(), serialized_tx.end()));
        response->set_txid(txid.begin(), 32);
        response->set_fee(entry.fee);
        response->set_vsize(entry.tx_size);  // tx_size is in bytes

        // Time when entered mempool (convert to Unix timestamp)
        auto time_since_epoch = entry.time.time_since_epoch();
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch);
        response->set_time(seconds.count());

        response->set_height(entry.height);

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("GetMempoolTransaction exception: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status MempoolServiceImpl::GetMempoolInfo(
    ::grpc::ServerContext* context,
    const ::dinerod::EmptyRequest* request,
    ::dinerod::MempoolInfoResponse* response
) {
    try {
        if (!m_mempool) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "Mempool not available");
        }

        // Get mempool statistics
        auto stats = m_mempool->getStats();

        // Fill response
        response->set_size(stats.tx_count);
        response->set_bytes(stats.total_size);
        response->set_usage(stats.total_size);  // Memory usage ~ total size
        response->set_max_mempool(300000000);   // 300MB default (TODO: get from config)
        response->set_min_fee(stats.min_fee_rate);

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("GetMempoolInfo exception: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

} // namespace grpc_server
} // namespace dinero
