#include "grpc/grpc_server.h"
#include "grpc/blockchain_service.h"
#include "grpc/mempool_service.h"
#include "grpc/wallet_service.h"
#include "storage/chain_db.h"
#include "storage/block_storage.h"
#include "daemon/mempool.h"
#include "daemon/daemon_context.h"
#include "policy/fee_estimator.h"
#include "common/logger.h"
#include <sstream>

namespace dinero {
namespace grpc_server {

GrpcServer::GrpcServer(ChainDB* chain_db,
                       BlockStorage* block_storage,
                       Mempool* mempool,
                       policy::FeeEstimator* fee_estimator,
                       DaemonContext* daemon_ctx,
                       int port)
    : m_running(false)
{
    // Build server address
    // Use 0.0.0.0:port format to bind to all interfaces
    std::ostringstream oss;
    oss << "0.0.0.0:" << port;
    m_server_address = oss.str();

    // Create service implementations
    m_blockchain_service = std::make_unique<BlockchainServiceImpl>(chain_db, block_storage);
    m_mempool_service = std::make_unique<MempoolServiceImpl>(mempool, fee_estimator);

    // Phase 2: Create wallet service if daemon context provided
    if (daemon_ctx) {
        m_wallet_service = std::make_unique<WalletServiceImpl>(daemon_ctx);
        g_logger.info("GrpcServer: Wallet service enabled");
    } else {
        g_logger.info("GrpcServer: Wallet service disabled (no daemon context)");
    }

    g_logger.info("GrpcServer initialized on " + m_server_address);
}

GrpcServer::~GrpcServer() {
    Stop();
}

bool GrpcServer::Start() {
    if (m_running) {
        g_logger.warning("GrpcServer already running");
        return true;
    }

    try {
        g_logger.info("GrpcServer::Start() - Creating ServerBuilder...");

        ::grpc::ServerBuilder builder;

        // Set reasonable message size limits
        builder.SetMaxReceiveMessageSize(100 * 1024 * 1024);  // 100MB
        builder.SetMaxSendMessageSize(100 * 1024 * 1024);     // 100MB

        // Add socket options for port reuse (helps with rapid restart scenarios)
        builder.AddChannelArgument("grpc.so_reuseport", 1);
        g_logger.info("GrpcServer::Start() - Enabled SO_REUSEPORT");

        g_logger.info("GrpcServer::Start() - Registering services...");
        // Register service implementations
        if (!m_blockchain_service) {
            g_logger.error("BlockchainService is null!");
            return false;
        }
        builder.RegisterService(m_blockchain_service.get());

        if (!m_mempool_service) {
            g_logger.error("MempoolService is null!");
            return false;
        }
        builder.RegisterService(m_mempool_service.get());

        // Phase 2: Register wallet service if available
        if (m_wallet_service) {
            g_logger.info("Registering WalletService...");
            builder.RegisterService(m_wallet_service.get());
        }

        g_logger.info("GrpcServer::Start() - Adding listening port: " + m_server_address);
        // Listen on the specified address without authentication
        // TODO: Add TLS/mTLS for production deployments
        int selected_port = 0;
        builder.AddListeningPort(m_server_address, ::grpc::InsecureServerCredentials(), &selected_port);

        g_logger.info("GrpcServer::Start() - Building and starting server...");
        // Build and start server
        m_server = builder.BuildAndStart();

        if (!m_server) {
            g_logger.error("Failed to start gRPC server (BuildAndStart returned null)");
            return false;
        }

        if (selected_port == 0) {
            g_logger.error("Failed to bind to gRPC port: " + m_server_address);
            return false;
        }

        g_logger.info("gRPC server bound to port: " + std::to_string(selected_port));

        m_running = true;
        g_logger.info("✅ gRPC server listening on " + m_server_address);
        if (m_wallet_service) {
            g_logger.info("   Services: dinerod.Blockchain, dinerod.Mempool, dinerod.Wallet");
        } else {
            g_logger.info("   Services: dinerod.Blockchain, dinerod.Mempool");
        }

        return true;

    } catch (const std::exception& e) {
        g_logger.error("GrpcServer::Start exception: " + std::string(e.what()));
        return false;
    }
}

void GrpcServer::Stop() {
    if (!m_running) {
        return;
    }

    g_logger.info("Stopping gRPC server...");

    if (m_server) {
        // Graceful shutdown with 5 second deadline
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        m_server->Shutdown(deadline);
        m_server.reset();
    }

    m_running = false;
    g_logger.info("gRPC server stopped");
}

} // namespace grpc_server
} // namespace dinero
