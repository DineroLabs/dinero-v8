#pragma once
#include <string>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <optional>

namespace dinero {
namespace rpc {

/**
 * @brief Token information with security features
 *
 * Features:
 * - IP locking: Tokens bound to client IP
 * - Auto-refresh: Seamless session extension
 * - Limited scope: Damage control (wallet, mining, read, admin)
 * - Persistent tokens: Never expire (for GUI/CLI)
 */
struct TokenInfo {
    std::string token;           // 32-character random token
    std::string ip;              // Client IP (for IP lock)
    std::string scope;           // Access scope: "wallet", "mining", "read", "admin"
    bool auto_refresh = false;   // Auto-extend lifetime on use
    bool persistent = false;     // Never expires (for local CLI/GUI)
    std::chrono::system_clock::time_point expires_at;  // Expiration time
    std::chrono::system_clock::time_point created_at;  // Creation time
    std::chrono::system_clock::time_point last_used;   // Last validation time
};

/**
 * @brief Thread-safe token manager for RPC authentication
 *
 * Provides automatic token lifecycle management:
 * - Generation: Create tokens with configurable lifetime
 * - Validation: Check token validity with IP and scope checks
 * - Refresh: Extend token lifetime automatically
 * - Cleanup: Remove expired tokens periodically
 *
 * Usage:
 *   auto& tm = TokenManager::instance();
 *   std::string token = tm.GenerateToken("127.0.0.1", "wallet", true);
 *   bool valid = tm.ValidateToken(token, "127.0.0.1", "wallet");
 */
class TokenManager {
public:
    /**
     * @brief Get singleton instance
     */
    static TokenManager& instance();

    /**
     * @brief Generate a new access token
     *
     * @param ip Client IP address (for IP lock)
     * @param scope Access scope ("wallet", "mining", "read", "admin")
     * @param auto_refresh Auto-extend lifetime on use
     * @param persistent Never expires (for local CLI/GUI)
     * @param lifetime_sec Token lifetime in seconds (default: 3600 = 1 hour)
     * @return Generated token string
     */
    std::string GenerateToken(const std::string& ip,
                              const std::string& scope = "wallet",
                              bool auto_refresh = false,
                              bool persistent = false,
                              int lifetime_sec = 3600);

    /**
     * @brief Validate token with IP and scope checks
     *
     * Performs:
     * - Existence check: Token must exist
     * - Expiry check: Token must not be expired
     * - IP lock check: IP must match (if set)
     * - Scope check: Scope must match or be "admin"
     * - Auto-refresh: Extends lifetime if enabled
     *
     * @param token Token string to validate
     * @param current_ip Client's current IP
     * @param required_scope Required scope for this operation
     * @return true if valid, false otherwise
     */
    bool ValidateToken(const std::string& token,
                       const std::string& current_ip,
                       const std::string& required_scope);

    /**
     * @brief Get token information
     *
     * @param token Token string
     * @return TokenInfo if found, nullopt otherwise
     */
    std::optional<TokenInfo> GetTokenInfo(const std::string& token) const;

    /**
     * @brief Manually refresh a token (extend lifetime)
     *
     * @param token Token to refresh
     * @return Same token string if successful, empty string if failed
     */
    std::string RefreshToken(const std::string& token);

    /**
     * @brief Revoke (delete) a token
     *
     * @param token Token to revoke
     * @return true if revoked, false if not found
     */
    bool RevokeToken(const std::string& token);

    /**
     * @brief Remove expired tokens
     *
     * Should be called periodically (e.g., every minute)
     * @return Number of tokens removed
     */
    int CleanupExpired();

    /**
     * @brief Get statistics about active tokens
     *
     * @return JSON with token counts by scope
     */
    std::string GetStatistics() const;

private:
    TokenManager() = default;
    ~TokenManager() = default;

    TokenManager(const TokenManager&) = delete;
    TokenManager& operator=(const TokenManager&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, TokenInfo> tokens_;

    /**
     * @brief Generate cryptographically random token string
     *
     * @return 32-character alphanumeric token
     */
    static std::string GenerateRandomToken();
};

} // namespace rpc
} // namespace dinero
