/**
 * Logs Aggregation RPC Methods - Context-Aware
 *
 * Unified Log Aggregator - Real-time log viewing across all services
 *
 * Methods:
 *   - logs.recent: Get recent logs from all services
 *   - logs.stream: Stream logs in real-time (returns recent + continues streaming)
 *   - logs.services: List available log services
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "daemon/daemon_context.h"
#include "common/logger_router.h"
#include "common/logger.h"
#include <stdexcept>
#include <string>

/**
 * logs.recent - Get recent logs from aggregated log buffer
 *
 * Parameters:
 *   {
 *     "service": "wallet|p2p|mining|mempool",  // Optional: filter by service
 *     "level": "debug|info|warning|error",     // Optional: minimum log level (default: info)
 *     "limit": 100                               // Optional: max logs to return (default: 100)
 *   }
 *
 * Returns:
 *   {
 *     "logs": [
 *       {
 *         "timestamp": "2025-11-15T10:30:45Z",
 *         "level": "info",
 *         "service": "wallet",
 *         "message": "Wallet loaded successfully"
 *       },
 *       ...
 *     ],
 *     "count": 42
 *   }
 *
 * Example:
 *   {"method": "logs.recent", "params": {"service": "wallet", "limit": 50}}
 */
din::Json rpc_context_logs_recent(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Access LoggerRouter from DaemonContext
    auto daemonCtx = DaemonContext::instance();
    if (!daemonCtx || !daemonCtx->logger_router) {
        throw std::runtime_error("LoggerRouter not available");
    }

    // Parse parameters
    std::string service = params.get("service", "").asString();
    std::string level_str = params.get("level", "info").asString();
    int limit = params.get("limit", 100).asInt();

    // Convert level string to LogLevel enum
    dinero::LogLevel min_level = dinero::LogLevel::INFO;
    if (level_str == "debug") min_level = dinero::LogLevel::DEBUG;
    else if (level_str == "info") min_level = dinero::LogLevel::INFO;
    else if (level_str == "warning") min_level = dinero::LogLevel::WARNING;
    else if (level_str == "error") min_level = dinero::LogLevel::ERROR;

    // Get recent logs
    auto logs = daemonCtx->logger_router->getRecentLogs(service, min_level, limit);

    // Build JSON response
    din::Json logs_array(Json::arrayValue);
    for (const auto& entry : logs) {
        din::Json log_entry;
        log_entry["timestamp"] = entry.timestamp;
        log_entry["level"] = entry.level;
        log_entry["service"] = entry.service;
        log_entry["message"] = entry.message;
        log_entry["thread_id"] = entry.thread_id;
        logs_array.append(log_entry);
    }

    result["logs"] = logs_array;
    result["count"] = static_cast<int>(logs.size());

    return result;
}

/**
 * logs.services - List available log services
 *
 * Parameters: None
 *
 * Returns:
 *   {
 *     "services": ["wallet", "p2p", "mining", "mempool", "global"]
 *   }
 *
 * Example:
 *   {"method": "logs.services"}
 */
din::Json rpc_context_logs_services(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    din::Json services(Json::arrayValue);
    services.append("wallet");
    services.append("p2p");
    services.append("mining");
    services.append("mempool");
    services.append("global");

    result["services"] = services;

    return result;
}

/**
 * logs.filter - Advanced log filtering with thread_id support
 *
 * Parameters:
 *   {
 *     "service": "wallet|p2p|mining|mempool",  // Optional: filter by service
 *     "level": "debug|info|warning|error",     // Optional: minimum log level (default: info)
 *     "thread_id": "12345",                     // Optional: filter by thread ID
 *     "limit": 100                               // Optional: max logs to return (default: 100)
 *   }
 *
 * Returns:
 *   {
 *     "logs": [
 *       {
 *         "timestamp": "2025-11-15T10:30:45Z",
 *         "level": "info",
 *         "service": "wallet",
 *         "message": "Wallet loaded successfully",
 *         "thread_id": "12345"
 *       },
 *       ...
 *     ],
 *     "count": 42
 *   }
 *
 * Example:
 *   {"method": "logs.filter", "params": {"service": "wallet", "thread_id": "12345", "limit": 50}}
 */
din::Json rpc_context_logs_filter(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Access LoggerRouter from DaemonContext
    auto daemonCtx = DaemonContext::instance();
    if (!daemonCtx || !daemonCtx->logger_router) {
        throw std::runtime_error("LoggerRouter not available");
    }

    // Parse parameters
    std::string service = params.get("service", "").asString();
    std::string level_str = params.get("level", "info").asString();
    std::string thread_id = params.get("thread_id", "").asString();
    int limit = params.get("limit", 100).asInt();

    // Convert level string to LogLevel enum
    dinero::LogLevel min_level = dinero::LogLevel::INFO;
    if (level_str == "debug") min_level = dinero::LogLevel::DEBUG;
    else if (level_str == "info") min_level = dinero::LogLevel::INFO;
    else if (level_str == "warning") min_level = dinero::LogLevel::WARNING;
    else if (level_str == "error") min_level = dinero::LogLevel::ERROR;

    // Get filtered logs
    auto logs = daemonCtx->logger_router->filterLogs(service, min_level, thread_id, limit);

    // Build JSON response
    din::Json logs_array(Json::arrayValue);
    for (const auto& entry : logs) {
        din::Json log_entry;
        log_entry["timestamp"] = entry.timestamp;
        log_entry["level"] = entry.level;
        log_entry["service"] = entry.service;
        log_entry["message"] = entry.message;
        log_entry["thread_id"] = entry.thread_id;
        logs_array.append(log_entry);
    }

    result["logs"] = logs_array;
    result["count"] = static_cast<int>(logs.size());

    return result;
}

/**
 * logs.tail - Tail logs in real-time (long-polling endpoint)
 *
 * Parameters:
 *   {
 *     "since": "2025-11-15T10:30:45Z",  // Optional: only return logs after this timestamp
 *     "service": "wallet",               // Optional: filter by service
 *     "level": "info",                   // Optional: minimum log level
 *     "timeout": 30                      // Optional: long-polling timeout in seconds (default: 30)
 *   }
 *
 * Returns:
 *   {
 *     "logs": [...],  // New logs since timestamp
 *     "count": 5
 *   }
 *
 * Note: This uses long-polling. For true streaming, use WebSocket (future enhancement).
 *
 * Example:
 *   {"method": "logs.tail", "params": {"since": "2025-11-15T10:30:45Z", "service": "wallet"}}
 */
din::Json rpc_context_logs_tail(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Access LoggerRouter from DaemonContext
    auto daemonCtx = DaemonContext::instance();
    if (!daemonCtx || !daemonCtx->logger_router) {
        throw std::runtime_error("LoggerRouter not available");
    }

    // Parse parameters
    std::string since = params.get("since", "").asString();
    std::string service = params.get("service", "").asString();
    std::string level_str = params.get("level", "info").asString();

    // Get logs as JSON
    dinero::LogLevel min_level = dinero::LogLevel::INFO;
    if (level_str == "debug") min_level = dinero::LogLevel::DEBUG;
    else if (level_str == "info") min_level = dinero::LogLevel::INFO;
    else if (level_str == "warning") min_level = dinero::LogLevel::WARNING;
    else if (level_str == "error") min_level = dinero::LogLevel::ERROR;

    std::string logs_json = daemonCtx->logger_router->getLogsJson(since, service, min_level);

    // Parse JSON string back into JSON value (for consistent API)
    Json::CharReaderBuilder builder;
    Json::Value logs_array;
    std::istringstream ss(logs_json);
    std::string errors;

    if (!Json::parseFromStream(builder, ss, &logs_array, &errors)) {
        throw std::runtime_error("Failed to parse logs JSON: " + errors);
    }

    result["logs"] = logs_array;
    result["count"] = static_cast<int>(logs_array.size());

    return result;
}

/**
 * logs.follow - Stream logs in real-time (subscription-based)
 *
 * This creates a subscription that captures all new log entries.
 * The subscription uses the LoggerRouter's built-in subscription mechanism.
 *
 * Parameters:
 *   {
 *     "service": "wallet",      // Optional: filter by service
 *     "level": "info",          // Optional: minimum log level
 *     "thread_id": "12345"      // Optional: filter by thread ID
 *   }
 *
 * Returns:
 *   {
 *     "subscription_id": 42,
 *     "message": "Subscription created successfully. Use logs.follow.poll to retrieve logs."
 *   }
 *
 * Note: This method creates a subscription. Use logs.follow.poll to retrieve new logs,
 *       and logs.follow.unsubscribe to clean up when done.
 *
 * Example:
 *   {"method": "logs.follow", "params": {"service": "wallet", "level": "info"}}
 */
din::Json rpc_context_logs_follow(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Access LoggerRouter from DaemonContext
    auto daemonCtx = DaemonContext::instance();
    if (!daemonCtx || !daemonCtx->logger_router) {
        throw std::runtime_error("LoggerRouter not available");
    }

    // Parse filter parameters
    std::string service = params.get("service", "").asString();
    std::string level_str = params.get("level", "info").asString();
    std::string thread_id = params.get("thread_id", "").asString();

    // Convert level string to LogLevel enum
    dinero::LogLevel min_level = dinero::LogLevel::INFO;
    if (level_str == "debug") min_level = dinero::LogLevel::DEBUG;
    else if (level_str == "info") min_level = dinero::LogLevel::INFO;
    else if (level_str == "warning") min_level = dinero::LogLevel::WARNING;
    else if (level_str == "error") min_level = dinero::LogLevel::ERROR;

    // Create a subscription with filter callback
    // NOTE: This is a simplified implementation. For production, you'd want to store
    // subscription filters and manage a per-subscription buffer.
    int subscription_id = daemonCtx->logger_router->subscribe([=](const dinero::LogEntry& entry) {
        // Filter by service
        if (!service.empty() && entry.service != service) {
            return;
        }

        // Filter by log level
        dinero::LogLevel entry_level = dinero::LogLevel::INFO;
        if (entry.level == "debug") entry_level = dinero::LogLevel::DEBUG;
        else if (entry.level == "info") entry_level = dinero::LogLevel::INFO;
        else if (entry.level == "warning") entry_level = dinero::LogLevel::WARNING;
        else if (entry.level == "error") entry_level = dinero::LogLevel::ERROR;

        if (entry_level < min_level) {
            return;
        }

        // Filter by thread_id
        if (!thread_id.empty() && entry.thread_id != thread_id) {
            return;
        }

        // Entry matches filters - in a full implementation, this would be buffered
        // for retrieval via logs.follow.poll
    });

    result["subscription_id"] = subscription_id;
    result["message"] = "Subscription created successfully. Note: Full streaming support requires WebSocket implementation.";
    result["note"] = "Use logs.tail for long-polling, or connect via WebSocket for true real-time streaming.";

    return result;
}

/**
 * logs.follow.unsubscribe - Cancel a log subscription
 *
 * Parameters:
 *   {
 *     "subscription_id": 42
 *   }
 *
 * Returns:
 *   {
 *     "success": true,
 *     "message": "Subscription cancelled successfully"
 *   }
 *
 * Example:
 *   {"method": "logs.follow.unsubscribe", "params": {"subscription_id": 42}}
 */
din::Json rpc_context_logs_follow_unsubscribe(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Access LoggerRouter from DaemonContext
    auto daemonCtx = DaemonContext::instance();
    if (!daemonCtx || !daemonCtx->logger_router) {
        throw std::runtime_error("LoggerRouter not available");
    }

    // Parse parameters
    int subscription_id = params.get("subscription_id", 0).asInt();

    if (subscription_id <= 0) {
        throw std::runtime_error("Invalid subscription_id");
    }

    // Unsubscribe
    daemonCtx->logger_router->unsubscribe(subscription_id);

    result["success"] = true;
    result["message"] = "Subscription cancelled successfully";

    return result;
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION
// ═══════════════════════════════════════════════════════════════

// Global RPC registry (defined in rpc_registry.cpp)
extern RpcRegistry g_rpcRegistry;

void WireLogsRpcContext() {
    // Register context-aware logs aggregation methods
    g_rpcRegistry.registerHandler("logs.recent",
                                 rpc_context_logs_recent,
                                 RegisterMode::Overwrite,
                                 "Get recent logs from all services");

    g_rpcRegistry.registerHandler("logs.services",
                                 rpc_context_logs_services,
                                 RegisterMode::Overwrite,
                                 "List available log services");

    g_rpcRegistry.registerHandler("logs.filter",
                                 rpc_context_logs_filter,
                                 RegisterMode::Overwrite,
                                 "Filter logs by service, level, and thread_id");

    g_rpcRegistry.registerHandler("logs.tail",
                                 rpc_context_logs_tail,
                                 RegisterMode::Overwrite,
                                 "Tail logs in real-time (long-polling)");

    g_rpcRegistry.registerHandler("logs.follow",
                                 rpc_context_logs_follow,
                                 RegisterMode::Overwrite,
                                 "Subscribe to real-time log stream");

    g_rpcRegistry.registerHandler("logs.follow.unsubscribe",
                                 rpc_context_logs_follow_unsubscribe,
                                 RegisterMode::Overwrite,
                                 "Unsubscribe from log stream");

    // Log registration (note: using g_logger here is acceptable for system-level RPC registration)
    auto daemonCtx = DaemonContext::instance();
    if (daemonCtx && daemonCtx->logger_interface) {
        daemonCtx->logger_interface->info("[RPC] Registered context-aware logs aggregation methods (recent, services, filter, tail, follow)");
    }
}
