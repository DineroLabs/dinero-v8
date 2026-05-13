#include "daemon/rpc/websocket_handlers.h"
#include "http_rpc_server.h"
#include "daemon/ws_subscriptions.hpp"
#include "daemon/ws_globals.h"
#include "rpc/rpc_registry.h"
#include <json/json.h>
#include <iostream>

/**
 * Phase 2.2: WebSocket RPC Handlers for Subscription Management and Metrics (vNext)
 *
 * Provides RPC methods for:
 * - wsSubscribe: Subscribe to WebSocket topics
 * - wsReplay: Replay historical events
 * - wsGetConnections: List active WebSocket connections (admin)
 * - wsGetTopicStats: Get per-topic statistics
 * - wsGetStatus: Get overall WebSocket system status
 *
 * Migration Status: Migrated to vNext RpcRegistry pattern
 */

extern RpcRegistry g_rpcRegistry;

namespace din {
namespace rpc {

// ═══════════════════════════════════════════════════════════════
// Implementation Functions (vNext Pattern)
// ═══════════════════════════════════════════════════════════════

din::Json ws_subscribe_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!g_subscriptions) {
        result["error"] = "WebSocket subscription system not initialized";
        result["code"] = -32000;
        return result;
    }

    // Extract topic parameter
    if (!params.isMember("topic") || !params["topic"].isString()) {
        result["error"] = "Invalid params: 'topic' must be a string";
        result["code"] = -32602;
        return result;
    }

    std::string topic = params["topic"].asString();

    // Get topic statistics
    auto stats = g_subscriptions->get_topic_stats();

    result["topic"] = topic;
    result["subscribed"] = true;
    result["message"] = "Subscription request acknowledged. Connect via WebSocket to receive real-time events.";

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

din::Json ws_replay_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!g_subscriptions) {
        result["error"] = "WebSocket subscription system not initialized";
        result["code"] = -32000;
        return result;
    }

    // Extract topic parameter
    if (!params.isMember("topic") || !params["topic"].isString()) {
        result["error"] = "Invalid params: 'topic' must be a string";
        result["code"] = -32602;
        return result;
    }

    std::string topic = params["topic"].asString();
    std::vector<std::string> events;

    // Range-based replay
    if (params.isMember("from_seq") && params.isMember("to_seq")) {
        uint64_t from_seq = params["from_seq"].asUInt64();
        uint64_t to_seq = params["to_seq"].asUInt64();
        events = g_subscriptions->replay_range(topic, from_seq, to_seq);
    }
    // Count-based replay
    else if (params.isMember("count")) {
        size_t count = params["count"].asUInt64();
        events = g_subscriptions->replay(topic, count);
    }
    else {
        result["error"] = "Invalid params: must provide either 'count' or 'from_seq'+'to_seq'";
        result["code"] = -32602;
        return result;
    }

    result["topic"] = topic;
    result["event_count"] = static_cast<uint64_t>(events.size());
    result["current_seq"] = static_cast<uint64_t>(g_subscriptions->get_topic_seq(topic));

    // Parse and return events as JSON array
    din::Json events_array = din::arr();
    for (const auto& event_str : events) {
        din::Json event;
        if (din::parse(event_str, event)) {
            events_array.append(event);
        } else {
            // If parsing fails, include raw string
            events_array.append(event_str);
        }
    }
    result["events"] = events_array;
    result["rpc_schema"] = "din.websocket.v1";
    return result;
}

din::Json ws_get_connections_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!g_subscriptions) {
        result["error"] = "WebSocket subscription system not initialized";
        result["code"] = -32000;
        return result;
    }

    // Get all active connections
    auto connections = g_subscriptions->get_all_connections();

    result["connection_count"] = static_cast<uint64_t>(connections.size());

    din::Json conns_array = din::arr();
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
        din::Json subs_array = din::arr();
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

din::Json ws_get_topic_stats_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!g_subscriptions) {
        result["error"] = "WebSocket subscription system not initialized";
        result["code"] = -32000;
        return result;
    }

    auto all_stats = g_subscriptions->get_topic_stats();

    // Filter by specific topic if provided
    std::string filter_topic;
    if (params.isMember("topic") && params["topic"].isString()) {
        filter_topic = params["topic"].asString();
    }

    din::Json stats_array = din::arr();

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

din::Json ws_get_status_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!g_subscriptions) {
        result["error"] = "WebSocket subscription system not initialized";
        result["code"] = -32000;
        return result;
    }

    auto connections = g_subscriptions->get_all_connections();
    auto stats = g_subscriptions->get_topic_stats();

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
    result["total_events_sent"] = static_cast<uint64_t>(total_sent);
    result["total_events_dropped"] = static_cast<uint64_t>(total_dropped);

    // Available topics (from stats)
    din::Json topics_array = din::arr();
    for (const auto& stat : stats) {
        topics_array.append(stat.topic);
    }
    result["available_topics"] = topics_array;

    result["protocol_version"] = "1.0";

    din::Json features = din::arr();
    features.append("replay");
    features.append("authentication");
    features.append("per-topic-stats");
    result["features"] = features;

    result["rpc_schema"] = "din.websocket.v1";
    return result;
}

} // namespace rpc
} // namespace din

// ═══════════════════════════════════════════════════════════════
// vNext RpcRegistry Registration
// ═══════════════════════════════════════════════════════════════

void registerWebSocketManagementRPC() {
    std::cout << "[WebSocket RPC] Registering methods in vNext RpcRegistry..." << std::endl;

    g_rpcRegistry.registerHandler("ws.subscribe",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return din::rpc::ws_subscribe_impl(ctx, params);
        },
        "websocket");

    g_rpcRegistry.registerHandler("ws.replay",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return din::rpc::ws_replay_impl(ctx, params);
        },
        "websocket");

    g_rpcRegistry.registerHandler("ws.getconnections",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return din::rpc::ws_get_connections_impl(ctx, params);
        },
        "websocket");

    g_rpcRegistry.registerHandler("ws.gettopicstats",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return din::rpc::ws_get_topic_stats_impl(ctx, params);
        },
        "websocket");

    g_rpcRegistry.registerHandler("ws.getstatus",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return din::rpc::ws_get_status_impl(ctx, params);
        },
        "websocket");

    std::cout << "[WebSocket RPC] ✅ Registered 5 WebSocket RPC methods:" << std::endl;
    std::cout << "[WebSocket RPC]    - ws.subscribe" << std::endl;
    std::cout << "[WebSocket RPC]    - ws.replay" << std::endl;
    std::cout << "[WebSocket RPC]    - ws.getconnections" << std::endl;
    std::cout << "[WebSocket RPC]    - ws.gettopicstats" << std::endl;
    std::cout << "[WebSocket RPC]    - ws.getstatus" << std::endl;
}
