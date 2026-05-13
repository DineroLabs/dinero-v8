#pragma once

#include <string>

namespace dinero {
namespace rpc {

/**
 * WebSocket Client Registry
 *
 * These functions manage the mapping between client IDs and WebSocket file descriptors.
 * They should be called by the WebSocket session lifecycle management code.
 */

/**
 * Register a WebSocket client
 * Call this when a new WebSocket connection is established
 *
 * @param client_id Unique identifier for the client (can be based on authentication, IP, or FD)
 * @param fd File descriptor for the WebSocket connection
 */
void ws_adapter_register_client(const std::string& client_id, int fd);

/**
 * Unregister a WebSocket client
 * Call this when a WebSocket connection is closed
 *
 * @param fd File descriptor for the WebSocket connection
 */
void ws_adapter_unregister_client(int fd);

/**
 * Get the client ID for a file descriptor
 * Returns a generated ID if the client wasn't explicitly registered
 *
 * @param fd File descriptor for the WebSocket connection
 * @return Client ID (e.g., "ws_123" if not registered, or actual client ID if registered)
 */
std::string ws_adapter_get_client_id(int fd);

} // namespace rpc
} // namespace dinero
