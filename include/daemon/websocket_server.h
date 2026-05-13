#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <mutex>
#include "compat/jsoncpp_compat.h"
#include "compat/net_compat.h"

namespace dinero {

// Forward declarations
class Blockchain;
class Mining;
class Mempool;

namespace lightning {
    class LightningEventManager;
    struct LightningEvent;
}

// WebSocket connection state
enum class WebSocketState {
    CONNECTING,     // Initial connection, upgrading from HTTP
    OPEN,          // WebSocket connection established
    CLOSING,       // Connection is being closed
    CLOSED         // Connection fully closed
};

// WebSocket frame types
enum class WebSocketFrameType {
    CONTINUATION = 0x0,
    TEXT = 0x1,
    BINARY = 0x2,
    CLOSE = 0x8,
    PING = 0x9,
    PONG = 0xA
};

// Subscription channel types
enum class SubscriptionChannel {
    NEW_HEADS,     // New block headers
    NEW_BLOCKS,    // Full block information
    MEMPOOL_TX,    // New mempool transactions
    MINING_INFO,   // Mining status updates
    // Phase 14: Lightning Network Events
    LIGHTNING_ALL,         // All Lightning events
    LIGHTNING_WATCHTOWER,  // Watchtower events (breaches, justice txs)
    LIGHTNING_CHANNELS,    // Channel lifecycle events
    LIGHTNING_PAYMENTS,    // Payment events (future)
    LIGHTNING_ROUTING      // Routing events (future)
};

// Rate limiting configuration
struct RateLimitConfig {
    int http_requests_per_second = 50;
    int http_burst_limit = 100;
    int ws_subscriptions_per_second = 5;
    int ws_max_subscriptions = 8;
    int ws_max_connections = 100;
    int max_cpu_percent = 80;
    int max_event_backlog = 1000;
    double circuit_breaker_multiplier = 0.5;
};

// Token bucket for rate limiting
class TokenBucket {
public:
    TokenBucket(double rate, double burst);
    
    bool consume(double tokens_needed);
    void refill();
    double get_tokens() const { return tokens; }
    
private:
    double tokens;
    double max_tokens;
    double refill_rate;
    std::chrono::steady_clock::time_point last_refill;
};

// Circuit breaker for global rate limiting
class CircuitBreaker {
public:
    CircuitBreaker(int failure_threshold = 10, int recovery_timeout = 60);
    
    bool should_allow_request();
    void record_failure();
    void record_success();
    bool is_open() const { return is_open_.load(); }
    
private:
    std::atomic<bool> is_open_{false};
    std::atomic<int> failure_count{0};
    std::chrono::steady_clock::time_point last_failure;
    int failure_threshold;
    int recovery_timeout_seconds;
};

// WebSocket connection
class WebSocketConnection {
public:
    WebSocketConnection(int socket, const std::string& client_ip);
    ~WebSocketConnection();
    
    // Connection management
    bool upgrade_from_http(const std::string& request);
    bool send_raw(const std::string& data);
    bool send_message(const std::string& message);
    bool send_json(const Json::Value& json);
    void close();
    
    // Subscription management
    bool subscribe(SubscriptionChannel channel);
    bool unsubscribe(SubscriptionChannel channel);
    bool has_subscription(SubscriptionChannel channel) const;
    
    // Rate limiting
    bool check_rate_limit();
    void update_rate_limit();
    
    // Getters
    int get_socket() const { return socket_fd; }
    const std::string& get_client_ip() const { return client_ip; }
    WebSocketState get_state() const { return state; }
    std::chrono::steady_clock::time_point get_last_activity() const { return last_activity; }
    
    // Public frame reading for server use
    bool read_frame_public(std::string& payload, WebSocketFrameType& type) { return read_frame(payload, type); }
    
private:
    int socket_fd;
    std::string client_ip;
    WebSocketState state;
    std::chrono::steady_clock::time_point last_activity;
    
    // Subscriptions
    std::vector<SubscriptionChannel> subscriptions;
    
    // Rate limiting
    TokenBucket rate_limiter;
    
    // WebSocket frame handling
    bool send_frame(WebSocketFrameType type, const std::string& payload);
    std::string create_frame(WebSocketFrameType type, const std::string& payload);
    bool read_frame(std::string& payload, WebSocketFrameType& type);
    
    // HTTP upgrade
    bool parse_upgrade_request(const std::string& request);
    std::string create_upgrade_response(const std::string& key);
    std::string calculate_accept_key(const std::string& key);
};

// WebSocket server
class WebSocketServer {
public:
    WebSocketServer();
    ~WebSocketServer();
    
    // Server management
    bool initialize(int port, const std::string& path = "/rpc.ws", const std::string& bind_address = "127.0.0.1");
    void shutdown();
    void start();
    int get_listen_port() const { return listen_port_; }
    
    // Configuration
    void set_rate_limit_config(const RateLimitConfig& config);
    void set_mining(Mining* mining);
    void set_mempool(std::shared_ptr<Mempool> mempool);

    // Phase 14: Lightning event streaming - DISABLED: Lightning is standalone
    // void set_lightning_event_manager(lightning::LightningEventManager* event_mgr);
    // void broadcast_lightning_event(const lightning::LightningEvent& event);
    
    // Event broadcasting
    void broadcast_new_block(int height, const std::string& hash, int64_t time, double difficulty);
    void broadcast_mempool_tx(const std::string& txid, double fee, int size);
    void broadcast_mining_info(bool generating, int threads, double hashrate);
    
    // Generic event broadcasting
    void broadcast_event(const std::string& event_json);
    
    // Rate limiting
    bool check_global_rate_limit();
    void update_circuit_breaker();
    
    // Monitoring
    Json::Value get_rate_limit_info() const;
    int get_active_connections() const { return connections.size(); }
    int get_total_subscriptions() const;
    
private:
    // Server state
    int server_socket;
    std::string ws_path;
    std::atomic<bool> running{false};
    int listen_port_ = -1;
    
    // Connections
    std::unordered_map<int, std::unique_ptr<WebSocketConnection>> connections;
    mutable std::mutex connections_mutex;
    
    // Rate limiting
    RateLimitConfig rate_limit_config;
    std::unordered_map<std::string, TokenBucket> client_rate_limiters;
    CircuitBreaker circuit_breaker;
    
    // Component references
    Mining* m_mining;
    std::shared_ptr<Mempool> m_mempool;
    // lightning::LightningEventManager* m_lightning_event_mgr;  // Phase 14 - DISABLED: Lightning is standalone
    
    // Server loop
    void run_server_loop();
    void handle_new_connection();
    void handle_connection_data(WebSocketConnection* conn);
    void cleanup_dead_connections();
    
    // Event dispatching
    void dispatch_to_subscribers(SubscriptionChannel channel, const Json::Value& data);
    
    // HTTP request reading for WebSocket upgrade
    bool read_http_request(int socket_fd, std::string& request);
    
    // Rate limiting helpers
    std::string get_client_key(const std::string& client_ip, const std::string& auth);
    bool is_authenticated(const std::string& client_ip, const std::string& auth);
    
    // Utility functions
    static std::string base64_encode(const std::string& input);
    static std::string sha1_hash(const std::string& input);
};

} // namespace dinero
