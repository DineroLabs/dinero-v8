#pragma once
#include "daemon_context.h"
#include "iservice.h"
#include <vector>
#include <memory>

// Forward declarations
namespace dinero {
namespace pool {
class PoolManager;
}
namespace grpc_server {
#ifndef DISABLE_GRPC
class GrpcServer;
#endif
class SocketWalletServer;  // Phase 5: Socket-based wallet service (always enabled)
}
namespace ipc {
class WatchRegistrationServer;  // Phase 9.3: Watch registration server
}
class ChainDB;
class TxOrphanPool;
}

namespace dinero {

/**
 * DaemonApp - Main application lifecycle manager
 *
 * Responsibilities:
 * - Construct all services in dependency order
 * - Initialize services (wire dependencies)
 * - Start services (open resources, spawn threads)
 * - Stop services in reverse order (graceful shutdown)
 *
 * Example usage:
 *   DaemonApp app;
 *   if (!app.Init()) return 1;
 *   if (!app.Start()) return 2;
 *   // ... run event loop ...
 *   app.Stop();
 */
class DaemonApp {
public:
    DaemonApp();
    ~DaemonApp();

    /**
     * Initialize all services
     * Creates service instances and calls Init() on each
     * @param argc Command line argument count (optional)
     * @param argv Command line arguments (optional)
     * @return true if all services initialized successfully
     */
    bool Init(int argc = 0, char** argv = nullptr);

    /**
     * Start all services
     * Calls Start() on each service in order
     * @return true if all services started successfully
     */
    bool Start();

    /**
     * Stop all services
     * Calls Stop() on each service in REVERSE order
     */
    void Stop();

    /**
     * Get daemon context (for testing/inspection)
     */
    DaemonContext& GetContext() { return ctx_; }

private:
    DaemonContext ctx_;
    std::vector<std::shared_ptr<IService>> services_;
    bool started_ = false;

    // Own ChainDB for the full DaemonApp lifetime. Services receive non-owning
    // pointers to this instance.
    std::unique_ptr<ChainDB> chain_db_;

    // Per-service JSON loggers (Step B: Separate log files)
    std::unique_ptr<class JsonLogger> wallet_logger_;
    std::unique_ptr<class JsonLogger> p2p_logger_;
    std::unique_ptr<class JsonLogger> mining_logger_;
    std::unique_ptr<class JsonLogger> mempool_logger_;

    // Unified log aggregator (Optional: Real-time log streaming)
    std::unique_ptr<class LoggerRouter> logger_router_;

    // Phase 3: gRPC server (infrastructure, not a service)
    // Provides wallet operations to lightningd via gRPC
    // Dev mode only: Disabled in release builds (DINERO_RELEASE=ON)
    #ifndef DISABLE_GRPC
    std::unique_ptr<grpc_server::GrpcServer> grpc_server_;
    #endif

    // Phase 5: Socket wallet server (infrastructure, not a service)
    // Provides wallet operations to lightningd via sockets
    // Always enabled: Works in both dev and release builds
    std::unique_ptr<grpc_server::SocketWalletServer> socket_wallet_server_;

    // Phase 9.3: Watch registration server (bidirectional oracle communication)
    // Allows lightningd to register transaction watches with dinerod
    // Always enabled: Part of oracle infrastructure
    std::unique_ptr<class dinero::ipc::WatchRegistrationServer> watch_registration_server_;

    // Transaction orphan pool (lifetime owned by DaemonApp)
    std::unique_ptr<TxOrphanPool> orphan_pool_owned_;

    // Pool accounting runtime (optional)
    std::shared_ptr<pool::PoolManager> pool_manager_runtime_;
};

} // namespace dinero
