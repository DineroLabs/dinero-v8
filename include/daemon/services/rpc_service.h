#pragma once
#include "daemon/iservice.h"
#include <memory>
#include <string>

// Forward declare HttpRpcServer (in global namespace)
class HttpRpcServer;
class RpcAuth;

namespace dinero {

/**
 * RPCService - IService wrapper for HttpRpcServer
 *
 * Wraps the RPC/HTTP/WebSocket server into IService lifecycle:
 * - Init() wires all dependencies from DaemonContext
 * - Start() initializes RPC server and starts listening
 * - Stop() cleanly shuts down HTTP/WS threads
 *
 * Dependencies: Logger, Config, Chainstate, Mempool, Wallet, P2P, Mining, Metrics
 *
 * Week 2: Enhanced with context-aware RPC handler support
 * - Injects DaemonContext into HttpRpcServer
 * - Calls WireRpcContext() to enable service access in handlers
 *
 * The HttpRpcServer provides:
 * - JSON-RPC 2.0 API (blockchain, wallet, mining, network)
 * - Cookie-based authentication
 * - HTTP request routing
 * - Method registry and dispatch (via RpcRegistry)
 * - Context-aware handler support (Week 2)
 *
 * Note: RPCService is the top-level service - it depends on everything else
 *       and nothing depends on it (one-way dependency).
 */
class RPCService : public IService {
public:
    RPCService();  // Must be in .cpp due to unique_ptr<HttpRpcServer>
    ~RPCService() override;  // Must be in .cpp due to unique_ptr<HttpRpcServer>

    std::string Name() const override { return "RPCServer"; }

    /**
     * Initialize RPC service with dependencies from context
     * - Stores references to all subsystems
     * - Configures RPC port, bind address, authentication
     * - Creates RPCServer instance
     * - Wires subsystem pointers to RPC server
     */
    bool Init(DaemonContext& ctx) override;

    /**
     * Start RPC service
     * - Initializes cookie authentication
     * - Registers all RPC method handlers
     * - Starts HTTP/WebSocket listener on configured port
     * - Spawns RPC worker threads
     */
    bool Start() override;

    /**
     * Stop RPC service
     * - Stops accepting new connections
     * - Waits for pending requests to complete
     * - Closes all WebSocket connections
     * - Stops HTTP listener threads
     */
    void Stop() override;

    /**
     * Get reference to wrapped HttpRpcServer
     * Use this to access RPC functionality
     */
    HttpRpcServer* GetHttpServer() { return http_server_.get(); }
    const HttpRpcServer* GetHttpServer() const { return http_server_.get(); }

    // Forward commonly used methods for convenience
    int GetActualPort() const;
    std::string GetHealth() const { return "{\"status\":\"ok\"}"; }

private:
    std::unique_ptr<HttpRpcServer> http_server_;
    std::shared_ptr<RpcAuth> rpc_auth_;

    // Store DaemonContext reference for wiring
    DaemonContext* ctx_ = nullptr;

    // Dependencies (all of them!)
    std::shared_ptr<class LoggerService> logger_;
    std::shared_ptr<class ConfigService> config_;
    std::shared_ptr<class ChainstateService> chainstate_;
    std::shared_ptr<class MempoolService> mempool_;
    std::shared_ptr<class WalletService> wallet_;
    std::shared_ptr<class P2PService> p2p_;
    std::shared_ptr<class MiningService> mining_;
    std::shared_ptr<class MetricsService> metrics_;

    // RPC configuration
    int rpc_port_ = 20998;
    std::string rpc_bind_ = "127.0.0.1";
    std::string datadir_;
    std::string cookie_path_;
};

} // namespace dinero
