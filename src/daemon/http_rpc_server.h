#pragma once

#include <json/json.h>
#include <string>
#include <functional>
#include <unordered_map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

// Forward declarations
class RpcAuth;
class RpcRegistry;
struct DaemonContext;  // In global namespace, not dinero::

// Cross-platform HTTP RPC server (no Qt dependencies)
// Uses standard C++ and system APIs only

class HttpRpcServer {
public:
    using RpcHandler = std::function<Json::Value(const Json::Value&)>;

    HttpRpcServer(const std::string& bind_address, uint16_t port);
    ~HttpRpcServer();

    // Authentication
    void set_auth(std::shared_ptr<RpcAuth> auth) { auth_ = auth; }
    void set_dev_mode(bool dev_mode) { dev_mode_ = dev_mode; }
    void set_readonly_mode(bool readonly) { readonly_mode_ = readonly; }

    // RPC Registry (vNext unified system)
    void set_rpc_registry(RpcRegistry* registry) { rpc_registry_ = registry; }

    // Week 2: Dependency injection for service access
    void set_daemon_context(DaemonContext* ctx) { daemon_context_ = ctx; }

    // Server lifecycle
    void start();
    void stop();
    bool is_running() const { return running_; }

    // RPC method registration (legacy - prefer using RpcRegistry)
    void register_method(const std::string& method, RpcHandler handler);

    // Method introspection
    std::vector<std::string> get_registered_methods() const;

    // Built-in methods
    void register_builtin_methods();
    
private:
    static constexpr uint32_t kMaxConcurrentConnections = 128;
    static constexpr uint32_t kMaxConcurrentRpcHandlers = 64;
    static constexpr std::chrono::milliseconds kClientSocketTimeout{10000};

    std::string bind_address_;
    uint16_t port_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<uint32_t> active_connections_{0};
    std::atomic<uint32_t> active_rpc_handlers_{0};
    bool dev_mode_{false};
    bool readonly_mode_{false};

    // Authentication
    std::shared_ptr<RpcAuth> auth_;

    // RPC method registry (vNext unified - preferred)
    RpcRegistry* rpc_registry_{nullptr};

    // Week 2: DaemonContext for service dependency injection
    DaemonContext* daemon_context_{nullptr};

    // Legacy method registry (deprecated - kept for backwards compatibility)
    std::unordered_map<std::string, RpcHandler> methods_;

    // Server thread
    std::unique_ptr<std::thread> server_thread_;
    
    // Server implementation
    void server_loop();
    void handle_connection(int client_socket);
    std::string process_http_request(const std::string& request);
    Json::Value process_rpc_call(const Json::Value& request);
    
    // HTTP utilities
    std::string build_http_response(const std::string& content, 
                                   const std::string& content_type = "application/json",
                                   int status_code = 200);
    std::string extract_http_body(const std::string& request);
    std::string extract_authorization_header(const std::string& request);
    std::string extract_client_ip(int client_socket);
    void configure_client_socket(int client_socket);
    bool try_acquire_rpc_execution_slot();
    void release_rpc_execution_slot();
    
    // Built-in RPC methods
    Json::Value handle_getinfo(const Json::Value& params);
    Json::Value handle_help(const Json::Value& params);
    Json::Value handle_stop(const Json::Value& params);
    
    // RPC access control: admin-only methods require elevated auth
    // Default cookie has admin access. Can restrict via --rpc-readonly flag.
    static bool isAdminMethod(const std::string& method);

    // Platform-specific socket utilities
    int create_server_socket();
    void close_socket(int socket);

    // RPC rate limiting (per-IP token bucket)
    static constexpr uint32_t RPC_MAX_REQUESTS_PER_SEC = 50;
    static constexpr uint32_t RPC_BUCKET_CAPACITY = 100;
    struct RpcRateBucket {
        double tokens{100.0};
        int64_t last_refill{0};
    };
    mutable std::mutex rpc_rate_mutex_;
    std::unordered_map<std::string, RpcRateBucket> rpc_rate_buckets_;
    bool checkRpcRate(const std::string& client_ip);
};
