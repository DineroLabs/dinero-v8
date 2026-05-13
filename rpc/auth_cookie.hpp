#pragma once
#include <string>
#include <unordered_map>
#include <utility>

namespace dinero {

std::pair<std::string,std::string> load_cookie_userpass();              // returns {user, pass}
std::string make_basic_value(const std::string& user, const std::string& pass); // "Basic <b64(user:pass)>"

// headers must be lowercased keys and trimmed values
bool check_basic_authorization(const std::unordered_map<std::string, std::string>& headers_lowercased);

} // namespace dinero