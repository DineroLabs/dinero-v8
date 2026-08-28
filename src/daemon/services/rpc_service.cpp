#include "daemon/services/rpc_service.h"
#include "daemon/services/logger_service.h"
#include "daemon/services/config_service.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/p2p_service.h"
#include "daemon/services/mining_service.h"
#include "daemon/services/metrics_service.h"
#include "daemon/daemon_context.h"
#include "daemon/http_rpc_server.h"  // Week 2: Real HttpRpcServer
#include "daemon/rpc_auth.h"         // Week 2: Cookie authentication
#include "daemon/rpc_context_wiring.h"  // Week 2: Context wiring function
#include "rpc/rpc_registry.h"        // Week 2: Global RPC registry
#include <stdexcept>
#include <fstream>

// Global RPC registry (defined in src/rpc/rpc_registry.cpp)
extern RpcRegistry g_rpcRegistry;

namespace dinero {

// Constructor and destructor must be defined here where HttpRpcServer is complete
RPCService::RPCService() = default;
RPCService::~RPCService() = default;

bool RPCService::Init(DaemonContext& ctx) {
    // Store context reference for Start()
    ctx_ = &ctx;

    // Wire ALL dependencies from context (RPC needs everything)
    logger_ = ctx.logger;
    config_ = ctx.config;
    chainstate_ = ctx.chainstate;
    mempool_ = ctx.mempool;
    wallet_ = ctx.wallet;
    p2p_ = ctx.p2p;
    mining_ = ctx.mining;
    metrics_ = ctx.metrics;

    if (!logger_) {
        throw std::runtime_error("[RPCService] Logger dependency missing");
    }
    if (!config_) {
        logger_->error("[RPCService] Config dependency missing");
        return false;
    }

    // Get RPC configuration from config
    rpc_port_ = config_->RPCPort();

    // Parse rpcbind (may include port like "0.0.0.0:20998")
    std::string rpcbind_value = config_->GetString("rpcbind", "127.0.0.1");
    size_t colon_pos = rpcbind_value.find(':');
    if (colon_pos != std::string::npos) {
        // Extract IP address before colon
        rpc_bind_ = rpcbind_value.substr(0, colon_pos);
        // Extract port after colon (override config port if specified in rpcbind)
        std::string port_str = rpcbind_value.substr(colon_pos + 1);
        try {
            rpc_port_ = std::stoi(port_str);
        } catch (...) {
            // Invalid port in rpcbind, use default from config
        }
    } else {
        rpc_bind_ = rpcbind_value;
    }

    datadir_ = config_->DataDir();
    cookie_path_ = datadir_ + "/.cookie";

    logger_->info("[RPCService] Initializing RPC server...");
    logger_->info("[RPCService]   RPC port: " + std::to_string(rpc_port_));
    logger_->info("[RPCService]   RPC bind: " + rpc_bind_);
    logger_->info("[RPCService]   Datadir: " + datadir_);
    logger_->info("[RPCService]   Cookie auth: " + cookie_path_);

    // Log available services
    if (chainstate_) logger_->info("[RPCService] Chainstate available for RPC");
    if (mempool_) logger_->info("[RPCService] Mempool available for RPC");
    if (wallet_) logger_->info("[RPCService] Wallet available for RPC");
    if (p2p_) logger_->info("[RPCService] P2P available for RPC");
    if (mining_) logger_->info("[RPCService] Mining available for RPC");

    logger_->info("[RPCService] RPC server initialized successfully");
    return true;
}

bool RPCService::Start() {
    logger_->info("[RPCService] Starting RPC server...");

    try {
        // Week 2: Create RPC authentication
        rpc_auth_ = std::make_shared<RpcAuth>(datadir_);
        if (!rpc_auth_->load_cookie()) {
            logger_->warning("[RPCService] Could not load existing cookie, generating new one");
            logger_->info("[RPCService] Cookie path: " + cookie_path_);
            if (!rpc_auth_->generate_cookie()) {
                logger_->error("[RPCService] Failed to generate RPC cookie");
                logger_->error("[RPCService] RPC authentication will not work!");
                return false;
            }
            // Verify cookie was created
            if (!std::filesystem::exists(cookie_path_)) {
                logger_->error("[RPCService] CRITICAL: Cookie file was not created at: " + cookie_path_);
                logger_->error("[RPCService] RPC authentication will fail!");
                return false;
            }
        }
        logger_->info("[RPCService] Cookie authentication ready: " + rpc_auth_->get_cookie_path());

        // Static credentials for mobile/iOS apps (rpcuser + rpcpassword)
        std::string rpcuser = config_->GetString("rpcuser", "");
        std::string rpcpassword = config_->GetString("rpcpassword", "");
        if (!rpcuser.empty() && !rpcpassword.empty()) {
            rpc_auth_->set_static_credentials(rpcuser, rpcpassword);
            logger_->info("[RPCService] Static RPC credentials configured for user: " + rpcuser);
        }

        // Week 2: Create HttpRpcServer
        http_server_ = std::make_unique<HttpRpcServer>(rpc_bind_, static_cast<uint16_t>(rpc_port_));
        http_server_->set_auth(rpc_auth_);
        http_server_->set_dev_mode(false);  // Production mode
        http_server_->set_readonly_mode(config_->GetBool("rpc.readonly", false));
        logger_->info("[RPCService] HttpRpcServer created");
        if (config_->GetBool("rpc.readonly", false)) {
            logger_->warning("[RPCService] RPC server running in read-only mode");
        }

        // Week 2: Connect to global RPC registry (contains all method registrations)
        http_server_->set_rpc_registry(&::g_rpcRegistry);
        logger_->info("[RPCService] Connected to global RPC registry");

        // Context-aware handlers capture DaemonContext pointers. A mobile
        // embedded node can stop and start repeatedly inside one host process,
        // unlike the ordinary daemon process lifetime. Restore the static RPC
        // baseline before each wiring pass so no handler from the prior
        // DaemonApp survives with a dangling context.
        ::g_rpcRegistry.beginRuntimeRegistrationCycle();

        // Week 2: Wire DaemonContext to RPC system (CRITICAL STEP)
        // This enables context-aware handlers to access services
        if (!ctx_) {
            logger_->error("[RPCService] DaemonContext not available for wiring");
            return false;
        }

        if (!WireRpcContext(*ctx_, http_server_.get())) {
            logger_->error("[RPCService] Failed to wire RPC context");
            logger_->error("[RPCService] Context-aware handlers will not work correctly");
            return false;
        }

        logger_->info("[RPCService] ✅ RPC context wired successfully");
        logger_->info("[RPCService] Context-aware handlers are now active");

        // Persist daemon_id to <datadir>/.daemon_id for monitoring/automation
        PersistDaemonId(*ctx_);

        // Start HTTP server (spawns listener threads)
        http_server_->start();
        logger_->info("[RPCService] HTTP RPC server started");

        logger_->info("[RPCService] RPC server ready at http://" + rpc_bind_ + ":" +
                     std::to_string(rpc_port_));
        logger_->info("[RPCService] Cookie auth: " + cookie_path_);

        return true;

    } catch (const std::exception& e) {
        logger_->error("[RPCService] Failed to start: " + std::string(e.what()));
        return false;
    }
}

void RPCService::Stop() {
    if (!http_server_) {
        logger_->info("[RPCService] Already stopped");
        return;
    }

    logger_->info("[RPCService] Stopping RPC server...");

    try {
        // Stop HTTP server (stops threads, closes sockets)
        http_server_->stop();

        logger_->info("[RPCService] RPC server stopped cleanly");

        // Reset the unique_ptr
        http_server_.reset();
        rpc_auth_.reset();

    } catch (const std::exception& e) {
        logger_->error("[RPCService] Error during shutdown: " + std::string(e.what()));
        // Still reset to avoid dangling pointer
        http_server_.reset();
        rpc_auth_.reset();
    }
}

int RPCService::GetActualPort() const {
    return rpc_port_;  // Return configured port
}

} // namespace dinero
