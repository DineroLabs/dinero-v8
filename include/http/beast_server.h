#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/signal_set.hpp>
#include "din_json.h"
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <chrono>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Forward declarations
class RpcRegistry;

namespace dinero::auth {
    class AuthStore;
}

namespace dinero {

// HTTP request handler function type
using HttpHandler = std::function<http::response<http::string_body>(
    const http::request<http::string_body>&, 
    const std::string& target,
    const std::string& body
)>;

// Beast HTTP server for vNext integration
class BeastHttpServer {
public:
    explicit BeastHttpServer(
        const std::string& address = "127.0.0.1",
        unsigned short port = 20998,
        int threads = 1
    );
    
    ~BeastHttpServer();
    
    // Server lifecycle
    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }
    
    // Handler registration
    void registerHandler(const std::string& path, HttpHandler handler);
    void setRpcRegistry(RpcRegistry* registry) { rpc_registry_ = registry; }
    
    // Built-in endpoints
    void enableHealthEndpoint(bool enable = true) { health_enabled_ = enable; }
    void enableMetricsEndpoint(bool enable = true) { metrics_enabled_ = enable; }
    void enableRpcEndpoint(bool enable = true) { rpc_enabled_ = enable; }
    
    // Authentication
    void setAuthStore(std::shared_ptr<dinero::auth::AuthStore> store) { auth_store_ = store; }
    void setDevRpcOpen(bool enable = true) { dev_rpc_open_ = enable; }
    void setCookiePath(const std::string& path) { cookie_path_ = path; }
    
    // Configuration
    void setRequestTimeout(std::chrono::seconds timeout) { request_timeout_ = timeout; }
    void setMaxRequestSize(size_t max_size) { max_request_size_ = max_size; }
    
private:
    // Internal session handling
    class HttpSession;
    
    // Built-in handlers
    http::response<http::string_body> handleHealth(
        const http::request<http::string_body>& req,
        const std::string& target,
        const std::string& body
    );
    
    http::response<http::string_body> handleMetrics(
        const http::request<http::string_body>& req,
        const std::string& target, 
        const std::string& body
    );
    
    http::response<http::string_body> handleRpc(
        const http::request<http::string_body>& req,
        const std::string& target,
        const std::string& body
    );
    
    http::response<http::string_body> handleNotFound(
        const http::request<http::string_body>& req,
        const std::string& target,
        const std::string& body
    );
    
    // Request routing
    http::response<http::string_body> routeRequest(
        const http::request<http::string_body>& req
    );
    
    // JSON-RPC processing
    din::Json processJsonRpc(const std::string& body);
    
    // Authentication
    bool checkRpcAuthentication(const http::request<http::string_body>& req, 
                               http::response<http::string_body>& error_response);
    
    // Connection handling
    void doAccept();
    void onAccept(beast::error_code ec, tcp::socket socket);
    
    // Utility methods
    std::string generateTraceId() const;
    void logRequest(const std::string& method, const std::string& target, 
                   const std::string& trace_id, int status_code) const;
    
    // Error response helper
    http::response<http::string_body> createErrorResponse(
        http::status status, 
        const std::string& message,
        const http::request<http::string_body>& req
    );
    
    // Server configuration
    std::string address_;
    unsigned short port_;
    int thread_count_;
    
    // Network components
    std::unique_ptr<net::io_context> ioc_;
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::vector<std::thread> threads_;
    
    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    
    // Handlers
    std::map<std::string, HttpHandler> handlers_;
    RpcRegistry* rpc_registry_{nullptr};
    
    // Endpoint configuration
    bool health_enabled_{true};
    bool metrics_enabled_{true};
    bool rpc_enabled_{true};
    
    // Authentication configuration
    std::shared_ptr<dinero::auth::AuthStore> auth_store_;
    bool dev_rpc_open_{false};
    std::string cookie_path_;
    
    // Request limits
    std::chrono::seconds request_timeout_{30};
    size_t max_request_size_{1024 * 1024}; // 1MB
};

} // namespace dinero
