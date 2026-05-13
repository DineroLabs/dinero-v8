#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <optional>
#include <vector>

namespace dinero {
namespace rpc {

/**
 * @brief Session information for device tracking
 *
 * Each session represents a unique device/application accessing the daemon.
 * Sessions are tied to tokens and provide visibility into what's connected.
 */
struct SessionInfo {
    std::string session_id;        // Unique session ID (UUID format)
    std::string token;              // Associated token
    std::string device_name;        // User-friendly name ("Desktop Wallet", "Trading Bot")
    std::string ip_address;         // Client IP address
    std::string user_agent;         // HTTP User-Agent header (if available)
    std::string platform;           // Platform info ("Windows", "macOS", "Linux", "Mobile")
    std::string scope;              // Token scope (wallet, mining, read, admin)
    bool persistent;                // Whether token is persistent
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_used;
    std::chrono::system_clock::time_point expires_at;  // When session/token expires
};

/**
 * @brief Session Manager - Track and manage active client sessions
 *
 * Provides visibility into all connected devices and applications.
 * Users can list active sessions, rename them, and revoke specific devices.
 *
 * Features:
 * - Device tracking with user-friendly names
 * - Session listing with metadata (IP, platform, last used)
 * - Selective revocation (disconnect specific devices)
 * - Session limits (prevent too many concurrent sessions)
 * - Integration with TokenManager
 *
 * Usage:
 *   auto& sm = SessionManager::instance();
 *
 *   // Create session
 *   std::string session_id = sm.CreateSession(token, device_name, ip, user_agent);
 *
 *   // List sessions
 *   auto sessions = sm.ListSessions();
 *
 *   // Revoke session
 *   sm.RevokeSession(session_id);
 */
class SessionManager {
public:
    /**
     * @brief Get singleton instance
     */
    static SessionManager& instance();

    /**
     * @brief Create a new session for a token
     *
     * @param token Token string
     * @param device_name User-friendly device name (e.g., "Desktop Wallet")
     * @param ip_address Client IP address
     * @param user_agent HTTP User-Agent header (optional)
     * @param scope Token scope
     * @param persistent Whether token is persistent
     * @param lifetime_sec Token lifetime in seconds
     * @return session_id Unique session ID (UUID format)
     */
    std::string CreateSession(
        const std::string& token,
        const std::string& device_name,
        const std::string& ip_address,
        const std::string& user_agent,
        const std::string& scope,
        bool persistent,
        int lifetime_sec
    );

    /**
     * @brief Update session last_used timestamp
     *
     * @param token Token to update
     */
    void UpdateSessionActivity(const std::string& token);

    /**
     * @brief List all active sessions
     *
     * @return Vector of SessionInfo objects
     */
    std::vector<SessionInfo> ListSessions() const;

    /**
     * @brief Get session info by session ID
     *
     * @param session_id Session ID
     * @return Optional SessionInfo (nullopt if not found)
     */
    std::optional<SessionInfo> GetSession(const std::string& session_id) const;

    /**
     * @brief Get session info by token
     *
     * @param token Token string
     * @return Optional SessionInfo (nullopt if not found)
     */
    std::optional<SessionInfo> GetSessionByToken(const std::string& token) const;

    /**
     * @brief Revoke session by session ID
     *
     * This removes the session and revokes the associated token.
     *
     * @param session_id Session ID to revoke
     * @return true if session was found and revoked
     */
    bool RevokeSession(const std::string& session_id);

    /**
     * @brief Rename session (change device_name)
     *
     * @param session_id Session ID
     * @param new_name New device name
     * @return true if session was found and renamed
     */
    bool RenameSession(const std::string& session_id, const std::string& new_name);

    /**
     * @brief Remove session when token is revoked externally
     *
     * @param token Token that was revoked
     */
    void RemoveSessionByToken(const std::string& token);

    /**
     * @brief Get total number of active sessions
     *
     * @return Number of active sessions
     */
    int GetSessionCount() const;

    /**
     * @brief Get number of sessions for specific scope
     *
     * @param scope Scope name (wallet, mining, read, admin)
     * @return Number of sessions with that scope
     */
    int GetSessionCountByScope(const std::string& scope) const;

    /**
     * @brief Check if session limit exceeded
     *
     * @param scope Scope to check
     * @return true if adding another session would exceed limit
     */
    bool IsSessionLimitExceeded(const std::string& scope) const;

    /**
     * @brief Set maximum sessions per scope (0 = unlimited)
     *
     * @param scope Scope name
     * @param max_sessions Maximum allowed sessions
     */
    void SetSessionLimit(const std::string& scope, int max_sessions);

private:
    SessionManager() = default;
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    /**
     * @brief Generate unique session ID (UUID v4)
     *
     * @return UUID string (e.g., "550e8400-e29b-41d4-a716-446655440000")
     */
    std::string GenerateSessionId() const;

    /**
     * @brief Extract platform from user agent
     *
     * @param user_agent User-Agent header
     * @return Platform string ("Windows", "macOS", "Linux", "Mobile", "Unknown")
     */
    std::string ExtractPlatform(const std::string& user_agent) const;

    mutable std::mutex mutex_;

    // Session storage: session_id -> SessionInfo
    std::unordered_map<std::string, SessionInfo> sessions_;

    // Token to session ID mapping for fast lookup
    std::unordered_map<std::string, std::string> token_to_session_;

    // Session limits per scope (0 = unlimited)
    std::unordered_map<std::string, int> session_limits_;
};

} // namespace rpc
} // namespace dinero
