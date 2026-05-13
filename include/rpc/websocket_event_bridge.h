#pragma once

#include "rpc/event_bus.h"
#include "rpc/websocket_server.h"
#include <unordered_map>
#include <unordered_set>
#include <mutex>

// Forward declaration
struct ExecutionContext;

namespace dinero {
namespace rpc {

/**
 * Client subscription information
 */
struct ClientSubscription {
    std::string client_id;
    EventFilter filter;
    uint64_t event_bus_sub_id;
    int64_t created_at;
    uint64_t events_received;

    ClientSubscription()
        : event_bus_sub_id(0), created_at(0), events_received(0) {}
};

/**
 * WebSocket Event Bridge
 *
 * Connects the EventBus to WebSocket clients, managing subscriptions
 * and pushing events in real-time.
 *
 * Features:
 * - Per-client event subscriptions with filters
 * - Automatic cleanup on client disconnect
 * - Subscription management RPC methods
 * - Event delivery statistics
 */
class WebSocketEventBridge {
public:
    WebSocketEventBridge(WebSocketServer* ws_server, EventBus* event_bus);
    ~WebSocketEventBridge();

    /**
     * Initialize the bridge (register RPC handlers)
     */
    void initialize();

    /**
     * Shutdown the bridge (cleanup subscriptions)
     */
    void shutdown();

    /**
     * Subscribe client to events
     * Returns subscription ID
     */
    std::string subscribe_client(const std::string& client_id,
                                 const EventFilter& filter);

    /**
     * Unsubscribe client from specific subscription
     */
    bool unsubscribe_client(const std::string& client_id,
                           const std::string& subscription_id);

    /**
     * Unsubscribe client from all events
     */
    void unsubscribe_all_client(const std::string& client_id);

    /**
     * Handle client disconnect (cleanup all subscriptions)
     */
    void handle_client_disconnect(const std::string& client_id);

    /**
     * Get client subscriptions
     */
    std::vector<ClientSubscription> get_client_subscriptions(const std::string& client_id) const;

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t total_clients = 0;
        uint64_t total_subscriptions = 0;
        uint64_t events_sent = 0;
        uint64_t events_failed = 0;
        std::unordered_map<std::string, uint64_t> events_per_client;
    };

    Stats get_stats() const;

private:
    // RPC handler implementations
    din::Json handle_subscribe(const ExecutionContext& ctx, const din::Json& params);
    din::Json handle_unsubscribe(const ExecutionContext& ctx, const din::Json& params);
    din::Json handle_list_subscriptions(const ExecutionContext& ctx, const din::Json& params);
    din::Json handle_get_event_types(const ExecutionContext& ctx, const din::Json& params);

    // Event delivery callback
    void deliver_event_to_client(const std::string& client_id,
                                const std::string& subscription_id,
                                const EventData& event);

    // Helper to generate subscription ID
    std::string generate_subscription_id() const;

    // Helper to parse event filter from JSON
    EventFilter parse_filter(const din::Json& filter_json) const;

    WebSocketServer* ws_server_;
    EventBus* event_bus_;

    mutable std::mutex mtx_;

    // client_id -> {subscription_id -> ClientSubscription}
    std::unordered_map<std::string, std::unordered_map<std::string, ClientSubscription>> client_subscriptions_;

    // subscription_id -> client_id (reverse lookup)
    std::unordered_map<std::string, std::string> subscription_to_client_;

    // Statistics
    mutable uint64_t events_sent_;
    mutable uint64_t events_failed_;
    mutable std::unordered_map<std::string, uint64_t> events_per_client_;

    bool initialized_;
};

} // namespace rpc
} // namespace dinero
