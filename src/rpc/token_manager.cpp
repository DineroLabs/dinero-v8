#include "rpc/token_manager.h"
#include "common/logger.h"
#include "din_json.h"
#include <random>
#include <sstream>
#include <iomanip>

using namespace dinero::rpc;
using namespace std::chrono;

TokenManager& TokenManager::instance() {
    static TokenManager mgr;
    return mgr;
}

std::string TokenManager::GenerateRandomToken() {
    static const char chars[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string token;
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(0, sizeof(chars) - 2);
    for (int i = 0; i < 32; ++i) {
        token += chars[dist(rng)];
    }
    return token;
}

std::string TokenManager::GenerateToken(const std::string& ip,
                                        const std::string& scope,
                                        bool auto_refresh,
                                        bool persistent,
                                        int lifetime_sec) {
    std::lock_guard<std::mutex> lock(mutex_);

    TokenInfo info;
    info.token = GenerateRandomToken();
    info.ip = ip;
    info.scope = scope;
    info.auto_refresh = auto_refresh;
    info.persistent = persistent;
    info.created_at = system_clock::now();
    info.last_used = system_clock::now();

    if (persistent) {
        info.expires_at = system_clock::time_point::max();
    } else {
        info.expires_at = system_clock::now() + seconds(lifetime_sec);
    }

    tokens_[info.token] = info;

    dinero::g_logger.info("[Auth] Token issued: " + info.token.substr(0, 8) + "... "
                          "scope=" + scope + " ip=" + ip +
                          " auto_refresh=" + (auto_refresh ? "true" : "false") +
                          " persistent=" + (persistent ? "true" : "false"));

    return info.token;
}

bool TokenManager::ValidateToken(const std::string& token,
                                 const std::string& current_ip,
                                 const std::string& required_scope) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tokens_.find(token);
    if (it == tokens_.end()) {
        dinero::g_logger.warning("[Auth] Token not found: " + token.substr(0, 8) + "...");
        return false;
    }

    TokenInfo& info = it->second;

    // Expiry check
    if (!info.persistent && system_clock::now() > info.expires_at) {
        dinero::g_logger.warning("[Auth] Token expired: " + token.substr(0, 8) + "...");
        return false;
    }

    // IP lock check
    if (!info.ip.empty() && info.ip != current_ip) {
        dinero::g_logger.warning("[Auth] IP mismatch for token " + token.substr(0, 8) + "... "
                                 "expected=" + info.ip + " got=" + current_ip);
        return false;
    }

    // Scope check (admin scope has access to everything)
    if (info.scope != "admin" && required_scope != info.scope) {
        dinero::g_logger.warning("[Auth] Scope mismatch for token " + token.substr(0, 8) + "... "
                                 "has=" + info.scope + " needs=" + required_scope);
        return false;
    }

    // Update last used time
    info.last_used = system_clock::now();

    // Auto-refresh if enabled and close to expiry
    if (info.auto_refresh && !info.persistent) {
        auto remaining = duration_cast<minutes>(info.expires_at - system_clock::now()).count();
        if (remaining < 5) {
            info.expires_at = system_clock::now() + seconds(3600);
            dinero::g_logger.info("[Auth] Auto-refreshed token for " + current_ip);
        }
    }

    return true;
}

std::optional<TokenInfo> TokenManager::GetTokenInfo(const std::string& token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tokens_.find(token);
    if (it == tokens_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::string TokenManager::RefreshToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tokens_.find(token);
    if (it == tokens_.end()) {
        return "";
    }

    TokenInfo& info = it->second;
    if (info.persistent) {
        return info.token; // No need to refresh persistent tokens
    }

    // Extend expiry by 1 hour
    info.expires_at = system_clock::now() + seconds(3600);

    dinero::g_logger.info("[Auth] Token manually refreshed for " + info.ip);
    return info.token;
}

bool TokenManager::RevokeToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tokens_.find(token);
    if (it == tokens_.end()) {
        return false;
    }

    dinero::g_logger.info("[Auth] Token revoked: " + token.substr(0, 8) + "... ip=" + it->second.ip);
    tokens_.erase(it);
    return true;
}

int TokenManager::CleanupExpired() {
    std::lock_guard<std::mutex> lock(mutex_);
    int removed = 0;

    for (auto it = tokens_.begin(); it != tokens_.end();) {
        if (!it->second.persistent && system_clock::now() > it->second.expires_at) {
            dinero::g_logger.info("[Auth] Removing expired token: " + it->first.substr(0, 8) + "...");
            it = tokens_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }

    if (removed > 0) {
        dinero::g_logger.info("[Auth] Cleanup removed " + std::to_string(removed) + " expired tokens");
    }

    return removed;
}

std::string TokenManager::GetStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Count tokens by scope
    std::unordered_map<std::string, int> by_scope;
    int persistent_count = 0;
    int auto_refresh_count = 0;

    for (const auto& pair : tokens_) {
        const TokenInfo& info = pair.second;
        by_scope[info.scope]++;
        if (info.persistent) persistent_count++;
        if (info.auto_refresh) auto_refresh_count++;
    }

    // Build JSON
    din::Json result = din::obj();
    result["total_tokens"] = static_cast<int>(tokens_.size());
    result["persistent_tokens"] = persistent_count;
    result["auto_refresh_tokens"] = auto_refresh_count;

    din::Json scopes = din::obj();
    for (const auto& pair : by_scope) {
        scopes[pair.first] = pair.second;
    }
    result["by_scope"] = scopes;

    // Convert to string
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    return Json::writeString(builder, result);
}
