#pragma once
#include <string>

// Forward declarations
class RpcAuth;

namespace dinero {
namespace rpc {

class TokenManager;

/**
 * @brief Unified authentication manager for HTTP and WebSocket RPC
 *
 * Supports dual authentication modes:
 * 1. Cookie-based auth (existing .cookie file system)
 * 2. Token-based auth (new Bearer token system)
 *
 * Authentication flow:
 * 1. Check for Bearer token in Authorization header
 * 2. If token present, validate with TokenManager
 * 3. If no token, fall back to cookie-based auth
 * 4. If both fail, reject request
 *
 * Usage:
 *   AuthManager auth(rpc_auth, "127.0.0.1");
 *   if (auth.ValidateRequest(headers)) {
 *       // Request authorized
 *   }
 */
class AuthManager {
public:
    /**
     * @brief Construct auth manager
     * @param rpc_auth Existing cookie-based auth system
     * @param client_ip Client IP address (for IP lock and localhost checks)
     */
    AuthManager(RpcAuth* rpc_auth, const std::string& client_ip);

    /**
     * @brief Validate authentication for RPC request
     *
     * Checks in order:
     * 1. Bearer token (if present)
     * 2. Cookie-based auth (fallback)
     * 3. Localhost bypass (if enabled)
     *
     * @param authorization_header Full Authorization header value
     * @param required_scope Required token scope (default: "wallet")
     * @return true if authenticated, false otherwise
     */
    bool ValidateRequest(const std::string& authorization_header,
                        const std::string& required_scope = "wallet");

    /**
     * @brief Extract and parse authorization header
     *
     * Supports:
     * - "Bearer <token>" for token auth
     * - "Basic <base64>" for cookie auth
     *
     * @param header Authorization header value
     * @param auth_type Output: "bearer" or "basic"
     * @param auth_value Output: token or base64 credentials
     * @return true if parsed successfully
     */
    static bool ParseAuthHeader(const std::string& header,
                               std::string& auth_type,
                               std::string& auth_value);

    /**
     * @brief Check if client is localhost
     * @return true if IP is 127.0.0.1 or ::1
     */
    bool IsLocalhost() const;

private:
    RpcAuth* rpc_auth_;
    std::string client_ip_;
};

} // namespace rpc
} // namespace dinero
