#include "rpc/websocket_event_bridge.h"
#include "rpc/rpc_registry.h"
#include "common/logger.h"
#include <sstream>
#include <iomanip>
#include <random>

// Global RPC registry (in global namespace)
extern RpcRegistry g_rpcRegistry;

namespace dinero {
namespace rpc {

WebSocketEventBridge::WebSocketEventBridge(WebSocketServer* ws_server, EventBus* event_bus)
    : ws_server_(ws_server)
    , event_bus_(event_bus)
    , events_sent_(0)
    , events_failed_(0)
    , initialized_(false) {
}

WebSocketEventBridge::~WebSocketEventBridge() {
    shutdown();
}

void WebSocketEventBridge::initialize() {
    if (initialized_) return;

    // Register WebSocket RPC methods for event subscription
    // g_rpcRegistry is declared at file scope in global namespace
    //
    // NOTE: ws.subscribe is registered in websocket_handlers.cpp to avoid duplication
    // This bridge only handles event-specific methods: unsubscribe, listsubscriptions, eventtypes

    g_rpcRegistry.registerHandler("ws.unsubscribe",
        [this](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return this->handle_unsubscribe(ctx, params);
        },
        "websocket");

    g_rpcRegistry.registerHandler("ws.listsubscriptions",
        [this](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return this->handle_list_subscriptions(ctx, params);
        },
        "websocket");

    g_rpcRegistry.registerHandler("ws.eventtypes",
        [this](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return this->handle_get_event_types(ctx, params);
        },
        "websocket");

    initialized_ = true;
}

void WebSocketEventBridge::shutdown() {
    std::lock_guard<std::mutex> lock(mtx_);

    // Unsubscribe all clients from event bus
    for (const auto& client_pair : client_subscriptions_) {
        for (const auto& sub_pair : client_pair.second) {
            event_bus_->unsubscribe(sub_pair.second.event_bus_sub_id);
        }
    }

    client_subscriptions_.clear();
    subscription_to_client_.clear();
    initialized_ = false;
}

std::string WebSocketEventBridge::subscribe_client(const std::string& client_id,
                                                   const EventFilter& filter) {
    std::lock_guard<std::mutex> lock(mtx_);

    std::string sub_id = generate_subscription_id();

    // Subscribe to event bus with callback that delivers to this client
    uint64_t event_bus_sub_id = event_bus_->subscribe(filter,
        [this, client_id, sub_id](const EventData& event) {
            this->deliver_event_to_client(client_id, sub_id, event);
        });

    // Store subscription info
    ClientSubscription sub;
    sub.client_id = client_id;
    sub.filter = filter;
    sub.event_bus_sub_id = event_bus_sub_id;
    sub.created_at = event_bus_->get_timestamp_ms();
    sub.events_received = 0;

    client_subscriptions_[client_id][sub_id] = sub;
    subscription_to_client_[sub_id] = client_id;

    return sub_id;
}

bool WebSocketEventBridge::unsubscribe_client(const std::string& client_id,
                                              const std::string& subscription_id) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto client_it = client_subscriptions_.find(client_id);
    if (client_it == client_subscriptions_.end()) {
        return false;
    }

    auto sub_it = client_it->second.find(subscription_id);
    if (sub_it == client_it->second.end()) {
        return false;
    }

    // Unsubscribe from event bus
    event_bus_->unsubscribe(sub_it->second.event_bus_sub_id);

    // Remove from maps
    client_it->second.erase(sub_it);
    subscription_to_client_.erase(subscription_id);

    // Remove client entry if no more subscriptions
    if (client_it->second.empty()) {
        client_subscriptions_.erase(client_it);
    }

    return true;
}

void WebSocketEventBridge::unsubscribe_all_client(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto client_it = client_subscriptions_.find(client_id);
    if (client_it == client_subscriptions_.end()) {
        return;
    }

    // Unsubscribe all from event bus
    for (const auto& sub_pair : client_it->second) {
        event_bus_->unsubscribe(sub_pair.second.event_bus_sub_id);
        subscription_to_client_.erase(sub_pair.first);
    }

    client_subscriptions_.erase(client_it);
}

void WebSocketEventBridge::handle_client_disconnect(const std::string& client_id) {
    unsubscribe_all_client(client_id);
}

std::vector<ClientSubscription> WebSocketEventBridge::get_client_subscriptions(const std::string& client_id) const {
    std::lock_guard<std::mutex> lock(mtx_);

    std::vector<ClientSubscription> subs;
    auto it = client_subscriptions_.find(client_id);
    if (it != client_subscriptions_.end()) {
        for (const auto& pair : it->second) {
            subs.push_back(pair.second);
        }
    }
    return subs;
}

WebSocketEventBridge::Stats WebSocketEventBridge::get_stats() const {
    std::lock_guard<std::mutex> lock(mtx_);

    Stats stats;
    stats.total_clients = client_subscriptions_.size();
    stats.total_subscriptions = subscription_to_client_.size();
    stats.events_sent = events_sent_;
    stats.events_failed = events_failed_;
    stats.events_per_client = events_per_client_;

    return stats;
}

// ============================================================================
// RPC Handler Implementations
// ============================================================================

din::Json WebSocketEventBridge::handle_subscribe(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        // Parse filter from params
        EventFilter filter;
        if (params.isMember("filter")) {
            filter = parse_filter(params["filter"]);
        }

        // Get client ID from context
        std::string client_id = ctx.client_id.empty() ? "default" : ctx.client_id;

        // Subscribe
        std::string sub_id = subscribe_client(client_id, filter);

        result["success"] = true;
        result["subscription_id"] = sub_id;
        result["message"] = "Subscribed to events successfully";

    } catch (const std::exception& e) {
        result["success"] = false;
        result["error"] = e.what();
    }

    return result;
}

din::Json WebSocketEventBridge::handle_unsubscribe(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        if (!params.isMember("subscription_id")) {
            throw std::invalid_argument("Missing subscription_id parameter");
        }

        std::string client_id = ctx.client_id.empty() ? "default" : ctx.client_id;
        std::string sub_id = params["subscription_id"].asString();

        bool success = unsubscribe_client(client_id, sub_id);

        result["success"] = success;
        result["message"] = success ? "Unsubscribed successfully" : "Subscription not found";

    } catch (const std::exception& e) {
        result["success"] = false;
        result["error"] = e.what();
    }

    return result;
}

din::Json WebSocketEventBridge::handle_list_subscriptions(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    try {
        std::string client_id = ctx.client_id.empty() ? "default" : ctx.client_id;
        auto subs = get_client_subscriptions(client_id);

        din::Json subs_array;
        for (const auto& sub : subs) {
            din::Json sub_json;
            sub_json["created_at"] = sub.created_at;
            sub_json["events_received"] = sub.events_received;

            // Add filter info
            if (!sub.filter.event_types.empty()) {
                din::Json types;
                for (auto type : sub.filter.event_types) {
                    types.append(event_type_to_string(type));
                }
                sub_json["event_types"] = types;
            }

            if (!sub.filter.addresses.empty()) {
                din::Json addrs;
                for (const auto& addr : sub.filter.addresses) {
                    addrs.append(addr);
                }
                sub_json["addresses"] = addrs;
            }

            if (sub.filter.min_amount > 0) {
                sub_json["min_amount"] = sub.filter.min_amount;
            }

            if (sub.filter.confirmed_only) {
                sub_json["confirmed_only"] = true;
            }

            subs_array.append(sub_json);
        }

        result["success"] = true;
        result["subscriptions"] = subs_array;
        result["count"] = static_cast<unsigned int>(subs.size());

    } catch (const std::exception& e) {
        result["success"] = false;
        result["error"] = e.what();
    }

    return result;
}

din::Json WebSocketEventBridge::handle_get_event_types(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    din::Json types;
    types.append(event_type_to_string(EventType::TransactionReceived));
    types.append(event_type_to_string(EventType::TransactionConfirmed));
    types.append(event_type_to_string(EventType::TransactionRejected));
    types.append(event_type_to_string(EventType::NewBlock));
    types.append(event_type_to_string(EventType::BlockOrphaned));
    types.append(event_type_to_string(EventType::WalletBalanceChanged));
    types.append(event_type_to_string(EventType::WalletNewAddress));
    types.append(event_type_to_string(EventType::WalletIncomingTx));
    types.append(event_type_to_string(EventType::WalletOutgoingTx));
    types.append(event_type_to_string(EventType::MempoolSizeChanged));
    types.append(event_type_to_string(EventType::MempoolFeeChanged));
    types.append(event_type_to_string(EventType::ChainReorg));
    types.append(event_type_to_string(EventType::ChainSyncing));
    types.append(event_type_to_string(EventType::ChainSynced));
    types.append(event_type_to_string(EventType::MiningStarted));
    types.append(event_type_to_string(EventType::MiningStopped));
    types.append(event_type_to_string(EventType::MiningBlockFound));

    result["success"] = true;
    result["event_types"] = types;

    return result;
}

// ============================================================================
// Private Helper Methods
// ============================================================================

void WebSocketEventBridge::deliver_event_to_client(const std::string& client_id,
                                                   const std::string& subscription_id,
                                                   const EventData& event) {
    if (!ws_server_) {
        events_failed_++;
        return;
    }

    try {
        // Build event notification message
        din::Json notification;
        notification["type"] = "event";
        notification["subscription_id"] = subscription_id;
        notification["data"] = event.toJson();

        // Send to client via WebSocket
        std::string message = notification.toStyledString();
        ws_server_->send_to_client(client_id, message);

        events_sent_++;
        events_per_client_[client_id]++;

        // Update subscription stats
        std::lock_guard<std::mutex> lock(mtx_);
        auto client_it = client_subscriptions_.find(client_id);
        if (client_it != client_subscriptions_.end()) {
            auto sub_it = client_it->second.find(subscription_id);
            if (sub_it != client_it->second.end()) {
                sub_it->second.events_received++;
            }
        }

    } catch (const std::exception& e) {
        events_failed_++;
        dinero::g_logger.warning("[WebSocketEventBridge] Failed to deliver event to client " +
                                 client_id + ": " + e.what());
    }
}

std::string WebSocketEventBridge::generate_subscription_id() const {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;

    std::ostringstream oss;
    oss << "sub_" << std::hex << std::setfill('0') << std::setw(16) << dis(gen);
    return oss.str();
}

EventFilter WebSocketEventBridge::parse_filter(const din::Json& filter_json) const {
    EventFilter filter;

    // Parse event types
    if (filter_json.isMember("event_types") && filter_json["event_types"].isArray()) {
        for (const auto& type_str : filter_json["event_types"]) {
            try {
                EventType type = event_type_from_string(type_str.asString());
                filter.event_types.push_back(type);
            } catch (const std::exception&) {
                // Skip invalid event types
            }
        }
    }

    // Parse addresses
    if (filter_json.isMember("addresses") && filter_json["addresses"].isArray()) {
        for (const auto& addr : filter_json["addresses"]) {
            filter.addresses.push_back(addr.asString());
        }
    }

    // Parse min_amount
    if (filter_json.isMember("min_amount")) {
        filter.min_amount = filter_json["min_amount"].asUInt64();
    }

    // Parse confirmed_only
    if (filter_json.isMember("confirmed_only")) {
        filter.confirmed_only = filter_json["confirmed_only"].asBool();
    }

    return filter;
}

} // namespace rpc
} // namespace dinero
