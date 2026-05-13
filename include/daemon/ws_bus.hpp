#pragma once

#include <string>
#include <memory>
#include <functional>
#include "compat/jsoncpp_compat.h"

namespace wsbus {

/**
 * WebSocket Bus - Thread-safe event broadcasting system
 * 
 * Provides a clean interface for broadcasting blockchain events to WebSocket clients
 * without tight coupling to the main blockchain code.
 */

// Forward declarations
class WebSocketBusImpl;

/**
 * Start the WebSocket bus service
 * 
 * @param cookie_file Path to the daemon's cookie file for authentication
 * @param port Port to listen on (default: 18332)
 * @param max_connections Maximum concurrent connections (default: 1000)
 * @param max_queue_size Maximum message queue size per connection in bytes (default: 5MB)
 * @param bind_address Address to bind to (default: "127.0.0.1")
 * @param path WebSocket path (default: "/ws")
 */
void start(const std::string& cookie_file, 
           uint16_t port = 18332,
           size_t max_connections = 1000,
           size_t max_queue_size = 5 * 1024 * 1024,
           const std::string& bind_address = "127.0.0.1",
           const std::string& path = "/ws");

/**
 * Stop the WebSocket bus service
 * 
 * Gracefully shuts down all connections and stops the service
 */
void stop();

/**
 * Broadcast a message to all subscribers of a topic
 * 
 * @param topic The topic to broadcast to (e.g., "blocks", "txs", "mining")
 * @param json_payload JSON string payload to send
 * 
 * Thread-safe: Can be called from any thread
 */
void broadcast(const std::string& topic, const std::string& json_payload);

/**
 * Broadcast a message with automatic JSON serialization
 * 
 * @param topic The topic to broadcast to
 * @param payload JSON-serializable object (will be converted to string)
 * 
 * Thread-safe: Can be called from any thread
 */
template<typename T>
void broadcast_json(const std::string& topic, const T& payload);

/**
 * Get current WebSocket statistics
 * 
 * @return JSON string with connection counts, subscription counts, and dropped message counts
 */
std::string get_stats();

/**
 * Check if the WebSocket bus is running
 * 
 * @return true if the service is active and accepting connections
 */
bool is_running();

/**
 * Get the actual port the WebSocket server is listening on
 * 
 * @return The actual port number, or 0 if not running
 */
uint16_t get_actual_port();

/**
 * Set connection rate limiting
 * 
 * @param max_connections_per_ip Maximum connections per IP address
 * @param time_window_seconds Time window for rate limiting in seconds
 */
void set_rate_limits(size_t max_connections_per_ip = 5, 
                    size_t time_window_seconds = 10);

// Additional broadcasting functions (temporary - should be moved to proper namespace)
void broadcast_mining_info(const Json::Value& mining_data);
void broadcast_new_block(const Json::Value& block_data);
void broadcast_chain_tip(const Json::Value& tip_data);

} // namespace wsbus
