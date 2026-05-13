#include "grpc/mempool_client.h"
#include "wallet/transaction.h"
#include "daemon/mempool.h"
#include "primitives/uint256.h"
#include "common/status.h"
#include "common/logger.h"
#include "common/serialization.h"
#include <grpcpp/create_channel.h>

namespace dinero {
namespace grpc_client {

MempoolClient::MempoolClient(const std::string& server_address)
    : m_server_address(server_address)
{
    // Create gRPC channel (insecure for local communication)
    // TODO: Add TLS for production deployments
    m_channel = grpc::CreateChannel(
        server_address,
        grpc::InsecureChannelCredentials()
    );

    // Create stub
    m_stub = dinerod::Mempool::NewStub(m_channel);

    g_logger.info("MempoolClient connected to " + server_address);
}

StatusOr<uint256> MempoolClient::BroadcastTransaction(const Transaction& tx) {
    ::grpc::ClientContext context;
    ::dinerod::RawTxRequest request;
    ::dinerod::TxBroadcastResponse response;

    // Serialize transaction
    std::vector<uint8_t> raw_tx = tx.Serialize();
    request.set_raw_tx(std::string(raw_tx.begin(), raw_tx.end()));

    ::grpc::Status status = m_stub->BroadcastTransaction(&context, request, &response);

    if (!status.ok()) {
        g_logger.error("BroadcastTransaction RPC failed: " + status.error_message());
        return StatusOr<uint256>(Status::Io);
    }

    if (!response.success()) {
        g_logger.error("Transaction rejected: " + response.error());

        // Map rejection reason to Status
        switch (response.reason()) {
            case ::dinerod::TxBroadcastResponse_RejectReason_INVALID:
                return StatusOr<uint256>(Status::Invalid);
            case ::dinerod::TxBroadcastResponse_RejectReason_DUPLICATE:
                return StatusOr<uint256>(Status::AlreadyExists);
            default:
                return StatusOr<uint256>(Status::Invalid);
        }
    }

    // Extract txid from response
    if (response.txid().size() != 32) {
        g_logger.error("BroadcastTransaction returned invalid txid");
        return StatusOr<uint256>(Status::Corruption);
    }

    uint256 txid;
    std::memcpy(txid.begin(), response.txid().data(), 32);

    g_logger.info("Transaction broadcast successfully: " + txid.GetHex());

    return StatusOr<uint256>(txid);
}

StatusOr<uint64_t> MempoolClient::EstimateFee(uint32_t target_blocks) {
    ::grpc::ClientContext context;
    ::dinerod::FeeEstimateRequest request;
    ::dinerod::FeeEstimateResponse response;

    request.set_target_blocks(target_blocks);

    ::grpc::Status status = m_stub->EstimateFee(&context, request, &response);

    if (!status.ok()) {
        g_logger.error("EstimateFee RPC failed: " + status.error_message());
        return StatusOr<uint64_t>(Status::Io);
    }

    if (!response.available()) {
        g_logger.warning("Fee estimation not available (insufficient data)");
        // Return a default minimum fee rate instead of error
        return StatusOr<uint64_t>(1);  // 1 sat/vbyte minimum
    }

    return StatusOr<uint64_t>(response.sat_per_vbyte());
}

StatusOr<bool> MempoolClient::IsInMempool(const uint256& txid) {
    ::grpc::ClientContext context;
    ::dinerod::TxIdRequest request;
    ::dinerod::BoolResponse response;

    request.set_txid(txid.begin(), 32);

    ::grpc::Status status = m_stub->IsInMempool(&context, request, &response);

    if (!status.ok()) {
        g_logger.error("IsInMempool RPC failed: " + status.error_message());
        return StatusOr<bool>(Status::Io);
    }

    return StatusOr<bool>(response.value());
}

StatusOr<MempoolEntry> MempoolClient::GetMempoolTransaction(const uint256& txid) {
    ::grpc::ClientContext context;
    ::dinerod::TxIdRequest request;
    ::dinerod::MempoolTxResponse response;

    request.set_txid(txid.begin(), 32);

    ::grpc::Status status = m_stub->GetMempoolTransaction(&context, request, &response);

    if (!status.ok()) {
        g_logger.error("GetMempoolTransaction RPC failed: " + status.error_message());
        return StatusOr<MempoolEntry>(Status::Io);
    }

    if (!response.found()) {
        return StatusOr<MempoolEntry>(Status::NotFound);
    }

    // Deserialize transaction
    std::vector<uint8_t> raw_tx(response.raw_tx().begin(), response.raw_tx().end());
    Reader reader(raw_tx);

    Transaction tx;
    try {
        Deserialize(reader, tx);
    } catch (const std::exception& e) {
        g_logger.error("Failed to deserialize mempool transaction: " + std::string(e.what()));
        return StatusOr<MempoolEntry>(Status::Serialization);
    }

    // Create MempoolEntry
    MempoolEntry entry;
    entry.tx = tx;
    entry.fee = response.fee();
    entry.tx_size = response.vsize();
    // Convert Unix timestamp to steady_clock
    // Note: This is an approximation since steady_clock doesn't track wall time
    // For mempool purposes, we just need relative time tracking
    entry.time = std::chrono::steady_clock::now();  // Use current time as approximate
    entry.height = response.height();

    return StatusOr<MempoolEntry>(entry);
}

StatusOr<MempoolStats> MempoolClient::GetMempoolInfo() {
    ::grpc::ClientContext context;
    ::dinerod::EmptyRequest request;
    ::dinerod::MempoolInfoResponse response;

    ::grpc::Status status = m_stub->GetMempoolInfo(&context, request, &response);

    if (!status.ok()) {
        g_logger.error("GetMempoolInfo RPC failed: " + status.error_message());
        return StatusOr<MempoolStats>(Status::Io);
    }

    // Convert to MempoolStats
    MempoolStats stats;
    stats.tx_count = response.size();
    stats.total_size = response.bytes();
    stats.min_fee_rate = response.min_fee();

    return StatusOr<MempoolStats>(stats);
}

bool MempoolClient::IsConnected() {
    // Try to get mempool info as a health check
    ::grpc::ClientContext context;
    ::dinerod::EmptyRequest request;
    ::dinerod::MempoolInfoResponse response;

    // Set short timeout for health check (1 second)
    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(1);
    context.set_deadline(deadline);

    ::grpc::Status status = m_stub->GetMempoolInfo(&context, request, &response);

    return status.ok();
}

} // namespace grpc_client
} // namespace dinero
