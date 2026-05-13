#pragma once
#include <string>
#include <memory>

namespace dinero::auth { class AuthStore; }

namespace dinero::rpc {

// Returns true if authorized (cookie or bearer). On failure, fills out
// status, headers, and JSON body for 401 response.
bool authorize_request(
    const std::string& auth_header,
    std::shared_ptr<dinero::auth::AuthStore> store,
    int& out_status,
    std::string& out_www_authenticate_header,
    std::string& out_json_body
);

} // namespace dinero::rpc
