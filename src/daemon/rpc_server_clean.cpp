#include <json/json.h>
// Clean RPC server implementation using portable rpc_http.hpp
/* daemon-only: rpc_http.hpp disabled */
#include "common/logger.h"
#include "daemon/cookie_auth.h"
#include "daemon/daemon_context.h"
#include <thread>
#include <atomic>
#include "compat/net_compat.h"
#include <csignal>

namespace dinero {

class CleanRPCServer {
private:
    std::atomic<bool> m_running{false};
    int m_server_socket{-1};
    int m_port{20998};
    std::thread m_server_thread;
    std::string m_cookie_creds;

public:
    CleanRPCServer() {
        // Ignore SIGPIPE globally (for macOS/Linux compatibility)
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
        
        // Load cookie credentials
        loadCookieCredentials();
    }

    ~CleanRPCServer() {
        shutdown();
    }

    bool initialize(int port = 20998) {
        m_port = port;
        
        // Create socket
        m_server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (m_server_socket < 0) {
            dinero::g_logger.error("Failed to create server socket");
            return false;
        }

        // #295: don't let spawned children (e.g. dinero-seeder) inherit the RPC
        // listen socket — an orphaned child would keep port 20998 bound after the
        // daemon exits and block the next daemon from starting.
        compat_set_cloexec(m_server_socket);

        // Set socket options
        int opt = 1;
        if (setsockopt(m_server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            g_logger.warning("Failed to set SO_REUSEADDR");
        }

        // Bind to all interfaces (0.0.0.0) for Docker compatibility
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;  // 0.0.0.0 - all interfaces
        server_addr.sin_port = htons(m_port);

        if (bind(m_server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            dinero::g_logger.error("Failed to bind server socket to port " + std::to_string(m_port));
            close(m_server_socket);
            return false;
        }

        // Listen for connections
        if (listen(m_server_socket, 100) < 0) {
            dinero::g_logger.error("Failed to listen on server socket");
            close(m_server_socket);
            return false;
        }

        g_logger.info("Clean RPC server initialized on 0.0.0.0:" + std::to_string(m_port));
        return true;
    }

    void start() {
        if (m_running.load()) {
            g_logger.warning("RPC server already running");
            return;
        }

        m_running.store(true);
        m_server_thread = std::thread(&CleanRPCServer::serverLoop, this);
        g_logger.info("Clean RPC server started");
    }

    void shutdown() {
        if (!m_running.load()) return;

        g_logger.info("Shutting down clean RPC server");
        m_running.store(false);

        if (m_server_socket >= 0) {
            close(m_server_socket);
            m_server_socket = -1;
        }

        if (m_server_thread.joinable()) {
            m_server_thread.join();
        }

        g_logger.info("Clean RPC server shutdown complete");
    }

private:
    void loadCookieCredentials() {
        // Resolve datadir: prefer DaemonContext config, fallback to $HOME/.dinero
        std::filesystem::path datadir;
        auto* ctx = DaemonContext::instance();
        if (ctx && ctx->config) {
            std::string dir_str = ctx->config->GetString("datadir", "");
            if (!dir_str.empty()) {
                if (dir_str[0] == '~') {
                    const char* h = getenv("HOME");
                    if (h) dir_str = std::string(h) + dir_str.substr(1);
                }
                datadir = dir_str;
            }
        }
        if (datadir.empty()) {
            const char* home = getenv("HOME");
            if (!home) {
                g_logger.error("HOME not set and no datadir configured, cannot locate cookie");
                return;
            }
            datadir = std::filesystem::path(home) / ".dinero";
        }
        auto cookie_path = dinero::rpc_auth::CookiePath(datadir);

        std::string user, pass;
        if (dinero::rpc_auth::ReadCookie(cookie_path, user, pass)) {
            m_cookie_creds = user + ":" + pass;
            g_logger.info("Loaded cookie from: " + cookie_path.string());
            return;
        }

        // No existing cookie — generate and write one
        if (dinero::rpc_auth::WriteCookie(cookie_path, user, pass)) {
            m_cookie_creds = user + ":" + pass;
            g_logger.info("Generated new cookie at: " + cookie_path.string());
        } else {
            g_logger.error("Failed to create cookie file at: " + cookie_path.string());
        }
    }

    void serverLoop() {
        g_logger.info("RPC server loop started");

        while (m_running.load()) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            int client_socket = accept(m_server_socket, (struct sockaddr*)&client_addr, &client_len);
            if (client_socket < 0) {
                if (m_running.load()) {
                    dinero::g_logger.error("Failed to accept client connection");
                }
                continue;
            }

            // Handle client in separate thread
            std::thread([this, client_socket, client_addr]() {
                try {
                    handleClient(client_socket, client_addr);
                } catch (const std::exception& e) {
                    g_logger.error("[CleanRPCServer] Exception in client handler: " + std::string(e.what()));
                } catch (...) {
                    g_logger.error("[CleanRPCServer] Unknown exception in client handler");
                }
            }).detach();
        }

        g_logger.info("RPC server loop stopped");
    }

    void handleClient(int client_socket, struct sockaddr_in client_addr) {
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        
        g_logger.info("New client connected from " + std::string(client_ip) + ":" + std::to_string(ntohs(client_addr.sin_port)));

        // Set up RPC context
        /* RpcContext disabled for daemon-only */
        /* ctx disabled for daemon-only */
        /* ctx.dispatch disabled for daemon-only */
                              const std::string& method,
                              const Json::Value& params,
                              const Json::Value& id) -> Json::Value {
            return dispatchRpcMethod(path, method, params, id);
        };

        // Use the robust RPC handler
        /* handle_rpc_connection disabled for daemon-only */

        g_logger.info("Client disconnected: " + std::string(client_ip));
    }

    Json::Value dispatchRpcMethod(const std::string& path, const std::string& method, const Json::Value& params, const Json::Value& id) {
        g_logger.info("RPC call: " + method + " (path: " + path + ")");

        // Handle wallet path routing
        std::string wallet;
        if (path.rfind("/wallet/", 0) == 0) {
            wallet = path.substr(8);
            g_logger.info("Wallet context: " + wallet);
        }

        // Implement basic RPC methods
        if (method == "getblockchaininfo") {
            return Json::Value(Json::objectValue);
                /* placeholder data */
                /* placeholder data */
                {"headers", 12345},
                /* placeholder data */,
                /* placeholder data */
                {"mediantime", 1640995200},
                /* placeholder data */
                /* placeholder data */
                {"chainwork", "0000000000000000000000000000000000000000000000000000000100010001"},
                {"size_on_disk", 1000000},
                {"pruned", false}
            };
        }
        else if (method == "getblockcount") {
            return Json::Value(12345);
        }
        else if (method == "getblockhash") {
            if (params.isArray() && params.size() > 0) {
                int height = params[0].asInt();
                return Json::Value("000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");
            }
            throw std::runtime_error("Invalid params for getblockhash");
        }
        else if (method == "getmininginfo") {
            return Json::Value(Json::objectValue);
                /* placeholder data */
                {"currentblockweight", 4000000},
                {"currentblocktx", 2500},
                /* placeholder data */
                {"networkhashps", 1000000000},
                {"pooledtx", 50},
                /* placeholder data */
                /* placeholder data */
            };
        }
        else if (method == "uptime") {
            return Json::Value(86400); // 1 day uptime
        }
        else if (method == "getnetworkinfo") {
            return Json::Value(Json::objectValue);
                {"version", 100000},
                {"subversion", "/Dinero:1.0.0/"},
                {"protocolversion", 70015},
                {"localservices", "0000000000000409"},
                {"localrelay", true},
                /* placeholder data */
                {"connections", 8},
                /* placeholder data */
                {"networks", Json::Value::array()},
                {"relayfee", 0.00001000},
                {"incrementalfee", 0.00001000},
                {"localaddresses", Json::Value::array()},
                /* placeholder data */
            };
        }
        else {
            throw std::runtime_error("Method not found: " + method);
        }
    }
};

// Global instance
static std::unique_ptr<dinero::CleanRPCServer> g_clean_rpc_server;

// C-style interface for compatibility
extern "C" {
    bool rpc_server_initialize(int port) {
        try {
            g_clean_rpc_server = std::make_unique<dinero::CleanRPCServer>();
            return g_clean_rpc_server->initialize(port);
        } catch (const std::exception& e) {
            dinero::g_logger.error("Failed to initialize clean RPC server: " + std::string(e.what()));
            return false;
        }
    }

    void rpc_server_start() {
        if (g_clean_rpc_server) {
            g_clean_rpc_server->start();
        }
    }

    void rpc_server_shutdown() {
        if (g_clean_rpc_server) {
            g_clean_rpc_server->shutdown();
            g_clean_rpc_server.reset();
        }
    }
}

} // namespace dinero
