#include "daemon/ws_globals.h"
#include "daemon/websocket_metrics.hpp"
#include <string>

// Global Subscriptions instance implementation (nullable pointer pattern)
// Initialized to nullptr - will be set when WebSocket server starts
Subscriptions* g_subscriptions = nullptr;

// Global WebSocket metrics instance
WebSocketMetrics g_websocket_metrics;

// Stub implementation of ws_send_text
// Returns false (message not sent) since WebSocket server not fully implemented
bool ws_send_text(int fd, const std::string& message) {
    // For now, just return false
    // When WebSocket server is ready, this will actually send via WebSocket frame
    (void)fd;
    (void)message;
    return false;  // Indicate message not sent
}
