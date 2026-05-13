#include "rpc_http.hpp"
#include "daemon/websocket_metrics.hpp"


namespace rpc {

// Define the global rate limiter instance here
RateLimitManager g_rate_limiter(RateLimitConfig{});

// Define the global WebSocket connections registry
std::map<int, WebSocketConnection> g_websocket_connections;
std::mutex g_websocket_mutex;

// Define the global WebSocket metrics instance
WebSocketMetrics g_websocket_metrics;

} // namespace rpc
