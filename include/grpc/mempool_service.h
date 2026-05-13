#pragma once

#include "dinerod.grpc.pb.h"
#include <grpcpp/grpcpp.h>

namespace dinero {
    class Mempool;
    struct ITxIngress;  // Step 5: Transaction ingress interface
    namespace policy {
        class FeeEstimator;
    }
}

namespace dinero {
namespace grpc_server {

/**
 * MempoolServiceImpl - gRPC service for mempool operations
 *
 * Implements the Mempool service defined in proto/dinerod.proto.
 * Provides Lightning Network (and other clients) with mempool functionality:
 * - Transaction broadcasting
 * - Fee estimation
 * - Mempool status queries
 * - Transaction lookup
 *
 * This service provides READ operations and transaction BROADCAST.
 * All queries are served from the daemon's mempool.
 */
class MempoolServiceImpl final : public dinerod::Mempool::Service {
public:
    /**
     * Construct MempoolService with mempool and fee estimator
     *
     * @param mempool  Mempool instance for stats/queries
     * @param fee_estimator  FeeEstimator for fee rate calculations
     * @param tx_ingress  Step 5: Transaction ingress interface for submission
     */
    explicit MempoolServiceImpl(dinero::Mempool* mempool,
                                 dinero::policy::FeeEstimator* fee_estimator,
                                 dinero::ITxIngress* tx_ingress = nullptr);
    ~MempoolServiceImpl() override = default;

    // Mempool service methods (from dinerod.proto)

    /**
     * Broadcast raw transaction to network
     *
     * Validates and broadcasts transaction. Returns txid on success.
     */
    ::grpc::Status BroadcastTransaction(
        ::grpc::ServerContext* context,
        const ::dinerod::RawTxRequest* request,
        ::dinerod::TxBroadcastResponse* response
    ) override;

    /**
     * Estimate fee rate for target confirmation time
     *
     * Returns sat/vbyte for desired number of blocks.
     */
    ::grpc::Status EstimateFee(
        ::grpc::ServerContext* context,
        const ::dinerod::FeeEstimateRequest* request,
        ::dinerod::FeeEstimateResponse* response
    ) override;

    /**
     * Check if transaction is in mempool
     *
     * Returns true if tx is pending (not yet confirmed).
     */
    ::grpc::Status IsInMempool(
        ::grpc::ServerContext* context,
        const ::dinerod::TxIdRequest* request,
        ::dinerod::BoolResponse* response
    ) override;

    /**
     * Get mempool transaction info
     *
     * Returns details about pending transaction.
     */
    ::grpc::Status GetMempoolTransaction(
        ::grpc::ServerContext* context,
        const ::dinerod::TxIdRequest* request,
        ::dinerod::MempoolTxResponse* response
    ) override;

    /**
     * Get current mempool size and fee statistics
     *
     * Useful for fee estimation and network health monitoring.
     */
    ::grpc::Status GetMempoolInfo(
        ::grpc::ServerContext* context,
        const ::dinerod::EmptyRequest* request,
        ::dinerod::MempoolInfoResponse* response
    ) override;

private:
    dinero::Mempool* m_mempool;
    dinero::policy::FeeEstimator* m_fee_estimator;
    dinero::ITxIngress* m_tx_ingress;  // Step 5: Transaction submission interface
};

} // namespace grpc_server
} // namespace dinero
