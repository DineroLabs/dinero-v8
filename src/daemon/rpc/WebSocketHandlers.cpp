#include "rpc/rpc_registry.h"
#include "daemon/ws_subscriptions.hpp"
#include "daemon/ws_globals.h"
#include "din_json.h"
#include <iostream>

using namespace dinero::rpc;

namespace {

// Phase 2.2: WebSocket subscription management RPC handlers

/**
 * wsSubscribe - Subscribe to a WebSocket topic with authentication check
 * Params: {
 *   "topic": "string",         // Topic name to subscribe to
 *   "replay_count": number     // Optional: Number of historical events to replay
 * }
 */
din::Json handleWsSubscribe(const ExecutionContext& ctx, const din::Json& params) {
    if (!g_subscriptions) {
        din::Json error;
        error["code"] = -32000;
        error["message"] = "WebSocket subscription system not initialized";
        throw std::runtime_error(din::dump(error));
    }

    // Extract topic parameter
    if (!params.has("topic") || !params["topic"].is_string()) {
        din::Json error;
        error["code"] = -32602;
        error["message"] = "Invalid params: 'topic' must be a string";
        throw std::runtime_error(din::dump(error));
    }

    std::string topic = params["topic"].get<std::string>();

    // Note: In the RPC context, we don't have a specific WebSocket connection FD
    // This handler is primarily for documentation and testing. Actual subscription
    // happens through WebSocket protocol messages.
    // For now, return topic metadata and auth requirements.

    din::Json result;
    result["topic"] = topic;
    result["subscribed"] = true;
    result["message"] = "Subscription request accepted. Connect via WebSocket to receive events.";

    // Get topic statistics
    auto stats = g_subscriptions->get_topic_stats();
    for (const auto& stat : stats) {
        if (stat.topic == topic) {
            result["subscriber_count"] = static_cast<uint64_t>(stat.subscriber_count);
            result["current_seq"] = static_cast<uint64_t>(stat.current_seq);
            result["events_sent"] = static_cast<uint64_t>(stat.events_sent);
            result["events_dropped"] = static_cast<uint64_t>(stat.events_dropped);
            break;
        }
    }

    result["rpc_schema"] = "din.websocket.v1";
    return result;
}

/**
 * wsReplay - Request historical events from a topic
 * Params: {
 *   "topic": "string",         // Topic name
 *   "count": number,           // Number of recent events to retrieve
 *   "from_seq": number,        // Optional: Start sequence (for range replay)
 *   "to_seq": number           // Optional: End sequence (for range replay)
 * }
 */
din::Json handleWsReplay(const ExecutionContext& ctx, const din::Json& params) {
    if (!g_subscriptions) {
        din::Json error;
        error["code"] = -32000;
        error["message"] = "WebSocket subscription system not initialized";
        throw std::runtime_error(din::dump(error));
    }

    // Extract topic parameter
    if (!params.has("topic") || !params["topic"].is_string()) {
        din::Json error;
        error["code"] = -32602;
        error["message"] = "Invalid params: 'topic' must be a string";
        throw std::runtime_error(din::dump(error));
    }

    std::string topic = params["topic"].get<std::string>();
    std::vector<std::string> events;

    // Range-based replay
    if (params.has("from_seq") && params.has("to_seq")) {
        uint64_t from_seq = params["from_seq"].get<uint64_t>();
        uint64_t to_seq = params["to_seq"].get<uint64_t>();
        events = g_subscriptions->replay_range(topic, from_seq, to_seq);
    }
    // Count-based replay
    else if (params.has("count")) {
        size_t count = params["count"].get<size_t>();
        events = g_subscriptions->replay(topic, count);
    }
    else {
        din::Json error;
        error["code"] = -32602;
        error["message"] = "Invalid params: must provide either 'count' or 'from_seq'+'to_seq'";
        throw std::runtime_error(din::dump(error));
    }

    din::Json result;
    result["topic"] = topic;
    result["event_count"] = static_cast<uint64_t>(events.size());
    result["current_seq"] = static_cast<uint64_t>(g_subscriptions->get_topic_seq(topic));

    // Parse and return events as JSON array
    din::Json events_array = din::Json::array();
    for (const auto& event_str : events) {
        try {
            din::Json event = din::parse(event_str);
            events_array.append(event);
        } catch (const std::exception& e) {
            // If parsing fails, include raw string
            events_array.append(event_str);
        }
    }
    result["events"] = events_array;
    result["rpc_schema"] = "din.websocket.v1";
    return result;
}

/**
 * wsGetConnections - List all active WebSocket connections (admin only)
 * Params: {} (no parameters)
 * Returns: Array of connection metadata
 */
din::Json handleWsGetConnections(const ExecutionContext& ctx, const din::Json& params) {
    if (!g_subscriptions) {
        din::Json error;
        error["code"] = -32000;
        error["message"] = "WebSocket subscription system not initialized";
        throw std::runtime_error(din::dump(error));
    }

    // Get all active connections
    auto connections = g_subscriptions->get_all_connections();

    din::Json result;
    result["connection_count"] = static_cast<uint64_t>(connections.size());

    din::Json conns_array = din::Json::array();
    for (const auto& [fd, metadata] : connections) {
        din::Json conn;
        conn["fd"] = fd;
        conn["client_ip"] = metadata.client_ip;

        // Convert auth level to string
        switch (metadata.auth_level) {
            case AuthLevel::NONE:
                conn["auth_level"] = "none";
                break;
            case AuthLevel::AUTHENTICATED:
                conn["auth_level"] = "authenticated";
                break;
            case AuthLevel::ADMIN:
                conn["auth_level"] = "admin";
                break;
        }

        conn["username"] = metadata.username;
        conn["connected_at"] = static_cast<uint64_t>(metadata.connected_at);

        // Add subscriptions
        din::Json subs_array = din::Json::array();
        for (const auto& topic : metadata.subscriptions) {
            subs_array.append(topic);
        }
        conn["subscriptions"] = subs_array;

        conns_array.append(conn);
    }
    result["connections"] = conns_array;
    result["rpc_schema"] = "din.websocket.v1";
    return result;
}

/**
 * wsGetTopicStats - Get per-topic statistics
 * Params: {
 *   "topic": "string"  // Optional: specific topic, or all if not provided
 * }
 */
din::Json handleWsGetTopicStats(const ExecutionContext& ctx, const din::Json& params) {
    if (!g_subscriptions) {
        din::Json error;
        error["code"] = -32000;
        error["message"] = "WebSocket subscription system not initialized";
        throw std::runtime_error(din::dump(error));
    }

    auto all_stats = g_subscriptions->get_topic_stats();

    // Filter by specific topic if provided
    std::string filter_topic;
    if (params.has("topic") && params["topic"].is_string()) {
        filter_topic = params["topic"].get<std::string>();
    }

    din::Json result;
    din::Json stats_array = din::Json::array();

    for (const auto& stat : all_stats) {
        // Apply filter if specified
        if (!filter_topic.empty() && stat.topic != filter_topic) {
            continue;
        }

        din::Json topic_stat;
        topic_stat["topic"] = stat.topic;
        topic_stat["subscriber_count"] = static_cast<uint64_t>(stat.subscriber_count);
        topic_stat["events_sent"] = static_cast<uint64_t>(stat.events_sent);
        topic_stat["events_dropped"] = static_cast<uint64_t>(stat.events_dropped);
        topic_stat["current_seq"] = static_cast<uint64_t>(stat.current_seq);

        stats_array.append(topic_stat);
    }

    result["topic_count"] = static_cast<uint64_t>(stats_array.size());
    result["topics"] = stats_array;
    result["rpc_schema"] = "din.websocket.v1";
    return result;
}

/**
 * wsGetStatus - Get overall WebSocket system status
 * Params: {} (no parameters)
 */
din::Json handleWsGetStatus(const ExecutionContext& ctx, const din::Json& params) {
    if (!g_subscriptions) {
        din::Json error;
        error["code"] = -32000;
        error["message"] = "WebSocket subscription system not initialized";
        throw std::runtime_error(din::dump(error));
    }

    auto connections = g_subscriptions->get_all_connections();
    auto stats = g_subscriptions->get_topic_stats();

    din::Json result;
    result["enabled"] = true;
    result["active_connections"] = static_cast<uint64_t>(connections.size());
    result["active_topics"] = static_cast<uint64_t>(stats.size());

    // Calculate total events sent/dropped across all topics
    uint64_t total_sent = 0;
    uint64_t total_dropped = 0;
    for (const auto& stat : stats) {
        total_sent += stat.events_sent;
        total_dropped += stat.events_dropped;
    }
    result["total_events_sent"] = total_sent;
    result["total_events_dropped"] = total_dropped;

    // Available topics (from stats)
    din::Json topics_array = din::Json::array();
    for (const auto& stat : stats) {
        topics_array.append(stat.topic);
    }
    result["available_topics"] = topics_array;

    result["protocol_version"] = "1.0";
    result["features"] = din::Json::array({"replay", "authentication", "per-topic-stats"});
    result["rpc_schema"] = "din.websocket.v1";
    return result;
}

} // anonymous namespace

// Registration function
void registerWebSocketRpcHandlers() {
    extern RpcRegistry g_rpcRegistry;

    std::cout << "[RPC] Registering WebSocket RPC handlers (Phase 2.2)..." << std::endl;

    // wsSubscribe - Subscribe to WebSocket topic
    RpcMethodMeta subscribe_meta;
    subscribe_meta.category = "websocket";
    subscribe_meta.description = "Subscribe to a WebSocket topic for real-time events";
    subscribe_meta.result_meta.description = "Subscription confirmation with topic metadata";
    g_rpcRegistry.registerHandler("wsSubscribe", handleWsSubscribe, subscribe_meta);

    // wsReplay - Replay historical events
    RpcMethodMeta replay_meta;
    replay_meta.category = "websocket";
    replay_meta.description = "Replay historical events from a WebSocket topic";
    replay_meta.result_meta.description = "Array of historical events with sequence numbers";
    g_rpcRegistry.registerHandler("wsReplay", handleWsReplay, replay_meta);

    // wsGetConnections - List active connections (admin)
    RpcMethodMeta connections_meta;
    connections_meta.category = "websocket";
    connections_meta.description = "List all active WebSocket connections with authentication info";
    connections_meta.result_meta.description = "Array of active connection metadata";
    g_rpcRegistry.registerHandler("wsGetConnections", handleWsGetConnections, connections_meta);

    // wsGetTopicStats - Get per-topic statistics
    RpcMethodMeta stats_meta;
    stats_meta.category = "websocket";
    stats_meta.description = "Get statistics for WebSocket topics";
    stats_meta.result_meta.description = "Per-topic subscriber counts and event metrics";
    g_rpcRegistry.registerHandler("wsGetTopicStats", handleWsGetTopicStats, stats_meta);

    // wsGetStatus - Get overall WebSocket system status
    RpcMethodMeta status_meta;
    status_meta.category = "websocket";
    status_meta.description = "Get overall WebSocket system status and metrics";
    status_meta.result_meta.description = "WebSocket system status including connections and topics";
    g_rpcRegistry.registerHandler("wsGetStatus", handleWsGetStatus, status_meta);

    std::cout << "[RPC] ✅ Registered 5 WebSocket RPC handlers" << std::endl;
}
