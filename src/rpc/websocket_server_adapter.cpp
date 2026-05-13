#include <json/json.h>
#include <json/writer.h>
#include "din_json.h"
#include "rpc/websocket_server.h"
#include "daemon/ws_subscriptions.hpp"
#include "daemon/ws_globals.h"
#include <sstream>
#include <mutex>
#include <unordered_map>

namespace dinero {
namespace rpc {

// Client ID to file descriptor mapping
// This allows the event bridge to route events to specific connections
static std::mutex g_client_registry_mutex;
static std::unordered_map<std::string, int> g_client_id_to_fd;
static std::unordered_map<int, std::string> g_fd_to_client_id;

WsServerAdapter::WsServerAdapter(dinero::WsServer* ws_server)
    : ws_server_(ws_server) {
}

bool WsServerAdapter::send_to_client(const std::string& client_id, const din::Json& message) {
    if (!g_subscriptions) {
        return false;
    }

    // Convert Json::Value to string
    Json::FastWriter writer;
    std::string json_str = writer.write(message);

    // Look up the file descriptor for this client
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(g_client_registry_mutex);
        auto it = g_client_id_to_fd.find(client_id);
        if (it != g_client_id_to_fd.end()) {
            fd = it->second;
        }
    }

    if (fd == -1) {
        // Client not found - might have disconnected
        return false;
    }

    // Create a per-connection channel name
    std::string channel = "events:" + client_id;

    // Enqueue message to the specific connection's channel
    g_subscriptions->enqueue(channel, json_str);
    return true;
}

void WsServerAdapter::broadcast(const din::Json& message) {
    if (!g_subscriptions) {
        return;
    }

    // Convert Json::Value to string
    Json::FastWriter writer;
    std::string json_str = writer.write(message);

    // Broadcast to all clients via the general "events" channel
    g_subscriptions->enqueue("events", json_str);
}

bool WsServerAdapter::is_client_connected(const std::string& client_id) const {
    std::lock_guard<std::mutex> lock(g_client_registry_mutex);
    return g_client_id_to_fd.find(client_id) != g_client_id_to_fd.end();
}

size_t WsServerAdapter::get_client_count() const {
    if (!g_subscriptions) {
        return 0;
    }

    // Get all connections from the subscription system
    auto connections = g_subscriptions->get_all_connections();
    return connections.size();
}

// Helper functions for managing client registration
namespace {

/**
 * Register a client ID with its file descriptor
 * This should be called when a WebSocket connection is established
 */
void register_client(const std::string& client_id, int fd) {
    std::lock_guard<std::mutex> lock(g_client_registry_mutex);
    g_client_id_to_fd[client_id] = fd;
    g_fd_to_client_id[fd] = client_id;

    // Subscribe the connection to its dedicated events channel
    if (g_subscriptions) {
        std::string channel = "events:" + client_id;
        g_subscriptions->subscribe(fd, channel);
    }
}

/**
 * Unregister a client when it disconnects
 * This should be called from the WebSocket session destructor
 */
void unregister_client(int fd) {
    std::lock_guard<std::mutex> lock(g_client_registry_mutex);

    auto it = g_fd_to_client_id.find(fd);
    if (it != g_fd_to_client_id.end()) {
        std::string client_id = it->second;

        // Unsubscribe from dedicated channel
        if (g_subscriptions) {
            std::string channel = "events:" + client_id;
            g_subscriptions->unsubscribe(fd, channel);
        }

        g_client_id_to_fd.erase(client_id);
        g_fd_to_client_id.erase(fd);
    }
}

/**
 * Get client ID for a file descriptor
 */
std::string get_client_id_for_fd(int fd) {
    std::lock_guard<std::mutex> lock(g_client_registry_mutex);
    auto it = g_fd_to_client_id.find(fd);
    if (it != g_fd_to_client_id.end()) {
        return it->second;
    }

    // Generate a client ID based on FD if not registered
    std::ostringstream oss;
    oss << "ws_" << fd;
    return oss.str();
}

} // anonymous namespace

// Export these functions for use by WebSocket session management
void ws_adapter_register_client(const std::string& client_id, int fd) {
    register_client(client_id, fd);
}

void ws_adapter_unregister_client(int fd) {
    unregister_client(fd);
}

std::string ws_adapter_get_client_id(int fd) {
    return get_client_id_for_fd(fd);
}

} // namespace rpc
} // namespace dinero
