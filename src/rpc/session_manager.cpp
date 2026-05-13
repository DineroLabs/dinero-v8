#include "rpc/session_manager.h"
#include "rpc/token_manager.h"
#include "common/logger.h"
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dinero {
namespace rpc {

using std::chrono::system_clock;
using std::chrono::seconds;

SessionManager& SessionManager::instance() {
    static SessionManager instance;
    return instance;
}

std::string SessionManager::GenerateSessionId() const {
    // Generate UUID v4 format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t random1 = dis(gen);
    uint64_t random2 = dis(gen);

    // Format as UUID
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');

    // Time low (8 hex digits)
    oss << std::setw(8) << (random1 & 0xFFFFFFFF) << "-";

    // Time mid (4 hex digits)
    oss << std::setw(4) << ((random1 >> 32) & 0xFFFF) << "-";

    // Time high and version (4 hex digits, version 4)
    oss << "4" << std::setw(3) << ((random1 >> 48) & 0x0FFF) << "-";

    // Clock seq (4 hex digits, variant bits)
    oss << std::setw(1) << ((random2 & 0x3) | 0x8)
        << std::setw(3) << ((random2 >> 2) & 0xFFF) << "-";

    // Node (12 hex digits)
    oss << std::setw(12) << ((random2 >> 16) & 0xFFFFFFFFFFFF);

    return oss.str();
}

std::string SessionManager::ExtractPlatform(const std::string& user_agent) const {
    if (user_agent.empty()) {
        return "Unknown";
    }

    std::string ua_lower = user_agent;
    std::transform(ua_lower.begin(), ua_lower.end(), ua_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Check for mobile first
    if (ua_lower.find("mobile") != std::string::npos ||
        ua_lower.find("android") != std::string::npos ||
        ua_lower.find("iphone") != std::string::npos ||
        ua_lower.find("ipad") != std::string::npos) {
        return "Mobile";
    }

    // Desktop platforms
    if (ua_lower.find("windows") != std::string::npos) {
        return "Windows";
    }
    if (ua_lower.find("mac os") != std::string::npos ||
        ua_lower.find("macos") != std::string::npos ||
        ua_lower.find("darwin") != std::string::npos) {
        return "macOS";
    }
    if (ua_lower.find("linux") != std::string::npos) {
        return "Linux";
    }

    return "Unknown";
}

std::string SessionManager::CreateSession(
    const std::string& token,
    const std::string& device_name,
    const std::string& ip_address,
    const std::string& user_agent,
    const std::string& scope,
    bool persistent,
    int lifetime_sec)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Check session limit
    if (IsSessionLimitExceeded(scope)) {
        dinero::g_logger.warning("[SessionManager] Session limit exceeded for scope: " + scope);
        throw std::runtime_error("Session limit exceeded for scope: " + scope);
    }

    // Check if session already exists for this token
    auto it = token_to_session_.find(token);
    if (it != token_to_session_.end()) {
        // Update existing session
        auto& session = sessions_[it->second];
        session.last_used = system_clock::now();
        session.device_name = device_name.empty() ? session.device_name : device_name;
        dinero::g_logger.info("[SessionManager] Updated existing session: " + it->second);
        return it->second;
    }

    // Generate new session ID
    std::string session_id = GenerateSessionId();

    // Create session info
    SessionInfo info;
    info.session_id = session_id;
    info.token = token;
    info.device_name = device_name.empty() ? "Unnamed Device" : device_name;
    info.ip_address = ip_address;
    info.user_agent = user_agent;
    info.platform = ExtractPlatform(user_agent);
    info.scope = scope;
    info.persistent = persistent;
    info.created_at = system_clock::now();
    info.last_used = system_clock::now();
    info.expires_at = persistent ?
        system_clock::time_point::max() :
        system_clock::now() + seconds(lifetime_sec);

    // Store session
    sessions_[session_id] = info;
    token_to_session_[token] = session_id;

    dinero::g_logger.info("[SessionManager] Created session: " + session_id +
                          " device=" + info.device_name +
                          " ip=" + ip_address +
                          " scope=" + scope);

    return session_id;
}

void SessionManager::UpdateSessionActivity(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = token_to_session_.find(token);
    if (it != token_to_session_.end()) {
        auto& session = sessions_[it->second];
        session.last_used = system_clock::now();
    }
}

std::vector<SessionInfo> SessionManager::ListSessions() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<SessionInfo> result;
    result.reserve(sessions_.size());

    for (const auto& pair : sessions_) {
        result.push_back(pair.second);
    }

    // Sort by last_used (most recent first)
    std::sort(result.begin(), result.end(),
              [](const SessionInfo& a, const SessionInfo& b) {
                  return a.last_used > b.last_used;
              });

    return result;
}

std::optional<SessionInfo> SessionManager::GetSession(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::optional<SessionInfo> SessionManager::GetSessionByToken(const std::string& token) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = token_to_session_.find(token);
    if (it != token_to_session_.end()) {
        return sessions_.at(it->second);
    }

    return std::nullopt;
}

bool SessionManager::RevokeSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }

    std::string token = it->second.token;
    std::string device_name = it->second.device_name;

    // Remove from token mapping
    token_to_session_.erase(token);

    // Remove session
    sessions_.erase(it);

    dinero::g_logger.info("[SessionManager] Revoked session: " + session_id +
                          " device=" + device_name);

    // Revoke the associated token
    auto& tm = TokenManager::instance();
    tm.RevokeToken(token);

    return true;
}

bool SessionManager::RenameSession(const std::string& session_id, const std::string& new_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }

    std::string old_name = it->second.device_name;
    it->second.device_name = new_name;

    dinero::g_logger.info("[SessionManager] Renamed session: " + session_id +
                          " from '" + old_name + "' to '" + new_name + "'");

    return true;
}

void SessionManager::RemoveSessionByToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = token_to_session_.find(token);
    if (it != token_to_session_.end()) {
        std::string session_id = it->second;
        sessions_.erase(session_id);
        token_to_session_.erase(it);

        dinero::g_logger.info("[SessionManager] Removed session for revoked token: " +
                              token.substr(0, 8) + "...");
    }
}

int SessionManager::GetSessionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(sessions_.size());
}

int SessionManager::GetSessionCountByScope(const std::string& scope) const {
    std::lock_guard<std::mutex> lock(mutex_);

    int count = 0;
    for (const auto& pair : sessions_) {
        if (pair.second.scope == scope) {
            count++;
        }
    }

    return count;
}

bool SessionManager::IsSessionLimitExceeded(const std::string& scope) const {
    // Must be called with mutex already locked

    auto it = session_limits_.find(scope);
    if (it == session_limits_.end() || it->second == 0) {
        return false;  // No limit set or unlimited
    }

    int current_count = 0;
    for (const auto& pair : sessions_) {
        if (pair.second.scope == scope) {
            current_count++;
        }
    }

    return current_count >= it->second;
}

void SessionManager::SetSessionLimit(const std::string& scope, int max_sessions) {
    std::lock_guard<std::mutex> lock(mutex_);
    session_limits_[scope] = max_sessions;

    dinero::g_logger.info("[SessionManager] Set session limit for scope '" + scope +
                          "': " + std::to_string(max_sessions));
}

} // namespace rpc
} // namespace dinero
