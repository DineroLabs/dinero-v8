#pragma once

#include <string>
#include <functional>
#include "din_json.h"  // For din::Json type

namespace dinero {
namespace rpc {

/**
 * Abstract WebSocket Server Interface
 *
 * This interface allows the WebSocketEventBridge to send messages to clients
 * without depending on the concrete WsServer implementation.
 */
class WebSocketServer {
public:
    virtual ~WebSocketServer() = default;

    /**
     * Send a message to a specific client
     * @param client_id Client identifier (connection ID)
     * @param message JSON message to send
     * @return true if message was queued for sending, false otherwise
     */
    virtual bool send_to_client(const std::string& client_id, const din::Json& message) = 0;

    /**
     * Broadcast a message to all connected clients
     * @param message JSON message to broadcast
     */
    virtual void broadcast(const din::Json& message) = 0;

    /**
     * Check if a client is connected
     * @param client_id Client identifier
     * @return true if client is connected
     */
    virtual bool is_client_connected(const std::string& client_id) const = 0;

    /**
     * Get number of connected clients
     */
    virtual size_t get_client_count() const = 0;
};

} // namespace rpc
} // namespace dinero

// Forward declaration of WsServer from global dinero namespace
namespace dinero {
    class WsServer;
}

namespace dinero {
namespace rpc {

/**
 * WebSocket Server Adapter
 *
 * Adapts the concrete WsServer implementation to the WebSocketServer interface.
 * This allows the event bridge to work with the existing WebSocket implementation.
 */
class WsServerAdapter : public WebSocketServer {
public:
    explicit WsServerAdapter(dinero::WsServer* ws_server);
    ~WsServerAdapter() override = default;

    bool send_to_client(const std::string& client_id, const din::Json& message) override;
    void broadcast(const din::Json& message) override;
    bool is_client_connected(const std::string& client_id) const override;
    size_t get_client_count() const override;

private:
    dinero::WsServer* ws_server_;
};

} // namespace rpc
} // namespace dinero
