#pragma once
#include <string>
#include <unordered_map>

namespace dinero {

// Load token once (or on-demand) from cookie file
std::string load_cookie_token(const std::string& cookie_path);

// Create Basic auth value from token
std::string make_basic_value(const std::string& token);

// Check Authorization header against expected Basic auth
bool check_basic_authorization(
    const std::unordered_map<std::string, std::string>& headers_lowercased,
    const std::string& cookie_path);

} // namespace dinero
