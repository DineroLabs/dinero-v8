#include "rpc/auth_manager.h"
#include "rpc/token_manager.h"
#include "daemon/rpc_auth.h"
#include "common/logger.h"
#include <algorithm>
#include <cctype>

namespace dinero {
namespace rpc {

AuthManager::AuthManager(RpcAuth* rpc_auth, const std::string& client_ip)
    : rpc_auth_(rpc_auth), client_ip_(client_ip) {
}

bool AuthManager::ValidateRequest(const std::string& authorization_header,
                                  const std::string& required_scope) {
    // Parse authorization header
    std::string auth_type, auth_value;
    if (!ParseAuthHeader(authorization_header, auth_type, auth_value)) {
        // No valid auth header - check if localhost bypass is enabled
        if (IsLocalhost() && rpc_auth_ && rpc_auth_->is_localhost_request(client_ip_)) {
            dinero::g_logger.info("[Auth] Localhost bypass for " + client_ip_);
            return true;
        }
        dinero::g_logger.warning("[Auth] No valid authorization header from " + client_ip_);
        return false;
    }

    // Handle Bearer token authentication
    if (auth_type == "bearer") {
        auto& tm = TokenManager::instance();
        bool valid = tm.ValidateToken(auth_value, client_ip_, required_scope);

        if (valid) {
            dinero::g_logger.info("[Auth] Token validation successful for " + client_ip_ +
                                  " scope=" + required_scope);
        } else {
            dinero::g_logger.warning("[Auth] Token validation failed for " + client_ip_);
        }

        return valid;
    }

    // Handle Basic (cookie) authentication
    if (auth_type == "basic") {
        if (!rpc_auth_) {
            dinero::g_logger.error("[Auth] RpcAuth not available for cookie validation");
            return false;
        }

        bool valid = rpc_auth_->validate_request(authorization_header);

        if (valid) {
            dinero::g_logger.info("[Auth] Cookie validation successful for " + client_ip_);
        } else {
            dinero::g_logger.warning("[Auth] Cookie validation failed for " + client_ip_);
        }

        return valid;
    }

    dinero::g_logger.warning("[Auth] Unknown auth type: " + auth_type);
    return false;
}

bool AuthManager::ParseAuthHeader(const std::string& header,
                                 std::string& auth_type,
                                 std::string& auth_value) {
    if (header.empty()) {
        return false;
    }

    // Find first space
    size_t space_pos = header.find(' ');
    if (space_pos == std::string::npos) {
        return false;
    }

    // Extract auth type (lowercase)
    auth_type = header.substr(0, space_pos);
    std::transform(auth_type.begin(), auth_type.end(), auth_type.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Extract auth value (everything after first space)
    auth_value = header.substr(space_pos + 1);

    // Trim whitespace from auth_value
    size_t start = auth_value.find_first_not_of(" \t\r\n");
    size_t end = auth_value.find_last_not_of(" \t\r\n");

    if (start == std::string::npos) {
        return false;
    }

    auth_value = auth_value.substr(start, end - start + 1);

    return !auth_type.empty() && !auth_value.empty();
}

bool AuthManager::IsLocalhost() const {
    return client_ip_ == "127.0.0.1" ||
           client_ip_ == "::1" ||
           client_ip_ == "localhost";
}

} // namespace rpc
} // namespace dinero
