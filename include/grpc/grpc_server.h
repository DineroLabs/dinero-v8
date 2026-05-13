#pragma once

#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

namespace dinero {
    class ChainDB;
    class BlockStorage;
    class Mempool;
    namespace policy {
        class FeeEstimator;
    }
}

struct DaemonContext;  // Forward declaration

namespace dinero {
namespace grpc_server {

class BlockchainServiceImpl;
class MempoolServiceImpl;
class WalletServiceImpl;  // Phase 2: Lightning wallet operations

/**
 * GrpcServer - gRPC server for inter-daemon communication
 *
 * Manages the gRPC server lifecycle and service registration.
 * Provides blockchain and mempool APIs to external clients (e.g., lightningd).
 *
 * Server listens on localhost:50051 by default.
 */
class GrpcServer {
public:
    /**
     * Construct GrpcServer with service dependencies
     *
     * @param chain_db  ChainDB for blockchain queries
     * @param mempool  Mempool for transaction management
     * @param fee_estimator  FeeEstimator for fee calculations
     * @param daemon_ctx  DaemonContext for wallet service (optional)
     * @param port  Port to listen on (default: 50051)
     */
    explicit GrpcServer(ChainDB* chain_db,
                        BlockStorage* block_storage,
                        Mempool* mempool,
                        policy::FeeEstimator* fee_estimator,
                        DaemonContext* daemon_ctx = nullptr,
                        int port = 50051);
    ~GrpcServer();

    /**
     * Start the gRPC server
     *
     * Begins listening for requests. Blocks until server is ready.
     *
     * @return true if server started successfully, false otherwise
     */
    bool Start();

    /**
     * Stop the gRPC server
     *
     * Gracefully shuts down the server and waits for completion.
     */
    void Stop();

    /**
     * Check if server is running
     *
     * @return true if server is running, false otherwise
     */
    bool IsRunning() const { return m_running; }

    /**
     * Get server address
     *
     * @return Server address string (e.g., "127.0.0.1:50051")
     */
    std::string GetAddress() const { return m_server_address; }

private:
    std::string m_server_address;
    bool m_running;

    // Service implementations
    std::unique_ptr<BlockchainServiceImpl> m_blockchain_service;
    std::unique_ptr<MempoolServiceImpl> m_mempool_service;
    std::unique_ptr<WalletServiceImpl> m_wallet_service;  // Phase 2: Optional wallet service

    // gRPC server
    std::unique_ptr<::grpc::Server> m_server;
};

} // namespace grpc_server
} // namespace dinero
