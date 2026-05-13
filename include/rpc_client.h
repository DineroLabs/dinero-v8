#pragma once

#include <json/json.h>
#include <string>
#include <optional>
#include <functional>

namespace dinero {
namespace rpc {

/**
 * Unified RPC client for CLI and GUI
 *
 * Features:
 * - Cookie authentication (reads .cookie file)
 * - JSON-RPC 2.0 formatting
 * - HTTP connection handling
 * - Error handling and reporting
 * - Connection status checking
 *
 * Usage:
 *   RpcClient client("127.0.0.1", 20998, "/path/to/datadir");
 *   if (client.connect()) {
 *       auto result = client.call("getblockcount");
 *       if (result) {
 *           int height = result.value()["result"].asInt();
 *       }
 *   }
 */
class RpcClient {
public:
    /**
     * Constructor
     * @param host RPC server host (e.g., "127.0.0.1")
     * @param port RPC server port (e.g., 20998)
     * @param datadir Path to data directory containing .cookie file
     */
    RpcClient(const std::string& host, uint16_t port, const std::string& datadir);

    /**
     * Constructor with explicit credentials (for non-cookie auth)
     * @param host RPC server host
     * @param port RPC server port
     * @param username RPC username
     * @param password RPC password
     */
    RpcClient(const std::string& host, uint16_t port,
              const std::string& username, const std::string& password);

    ~RpcClient() = default;

    // Connection management

    /**
     * Test connection to RPC server
     * @return true if server is reachable and responding
     */
    bool connect();

    /**
     * Check if connected to RPC server
     * @return true if last connection attempt succeeded
     */
    bool is_connected() const { return connected_; }

    /**
     * Get connection status message
     * @return Human-readable status (e.g., "Connected to 127.0.0.1:20998")
     */
    std::string get_status() const { return status_message_; }

    // RPC method calls

    /**
     * Call RPC method with no parameters
     * @param method RPC method name (e.g., "getblockcount")
     * @return JSON response if successful, std::nullopt on error
     */
    std::optional<Json::Value> call(const std::string& method);

    /**
     * Call RPC method with parameters
     * @param method RPC method name
     * @param params JSON array of parameters
     * @return JSON response if successful, std::nullopt on error
     */
    std::optional<Json::Value> call(const std::string& method, const Json::Value& params);

    /**
     * Call RPC method with single string parameter (convenience)
     * @param method RPC method name
     * @param param Single string parameter
     * @return JSON response if successful, std::nullopt on error
     */
    std::optional<Json::Value> call(const std::string& method, const std::string& param);

    /**
     * Call RPC method with single integer parameter (convenience)
     * @param method RPC method name
     * @param param Single integer parameter
     * @return JSON response if successful, std::nullopt on error
     */
    std::optional<Json::Value> call(const std::string& method, int param);

    // Error handling

    /**
     * Get last error message
     * @return Error message from last failed call, empty if no error
     */
    std::string get_last_error() const { return last_error_; }

    /**
     * Get last RPC error code (if any)
     * @return RPC error code from last failed call, 0 if no RPC error
     */
    int get_last_error_code() const { return last_error_code_; }

    /**
     * Check if last error was a connection error (vs RPC error)
     * @return true if last error was connection-related
     */
    bool last_error_was_connection() const { return last_error_was_connection_; }

    // Configuration

    /**
     * Set timeout for RPC calls
     * @param seconds Timeout in seconds (default: 30)
     */
    void set_timeout(int seconds) { timeout_seconds_ = seconds; }

    /**
     * Enable/disable debug output
     * @param enable true to print debug info to stdout
     */
    void set_debug(bool enable) { debug_ = enable; }

private:
    // Connection info
    std::string host_;
    uint16_t port_;
    std::string datadir_;
    std::string username_;
    std::string password_;
    bool use_cookie_auth_;

    // State
    bool connected_ = false;
    std::string status_message_;
    std::string last_error_;
    int last_error_code_ = 0;
    bool last_error_was_connection_ = false;
    int timeout_seconds_ = 30;
    bool debug_ = false;

    // Helper methods
    bool load_cookie();
    std::string build_auth_header() const;
    std::string build_json_rpc_request(const std::string& method, const Json::Value& params) const;
    std::optional<Json::Value> send_http_request(const std::string& request_body);
    void set_error(const std::string& message, int code = 0, bool connection_error = false);
    void debug_log(const std::string& message) const;
};

// Convenience functions for common RPC calls

/**
 * Get blockchain height
 * @param client RPC client
 * @return Block height, or -1 on error
 */
int get_block_count(RpcClient& client);

/**
 * Get daemon version info
 * @param client RPC client
 * @return Version info object, or std::nullopt on error
 */
std::optional<Json::Value> get_version(RpcClient& client);

/**
 * Check database health
 * @param client RPC client
 * @return Health status object, or std::nullopt on error
 */
std::optional<Json::Value> check_database(RpcClient& client);

/**
 * Get wallet balance
 * @param client RPC client
 * @return Balance in DIN, or 0.0 on error
 */
double get_balance(RpcClient& client);

/**
 * Get new address
 * @param client RPC client
 * @return New address string, or empty on error
 */
std::string get_new_address(RpcClient& client);

} // namespace rpc
} // namespace dinero
