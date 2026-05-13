#include "daemon/rpc/auth_middleware.h"
#include "auth/auth_store.h"
#include <json/json.h>
#include <string>
#include <algorithm>
#include <cctype>
#include <cstring>

static std::string trim(const std::string& s) {
    auto l = s.find_first_not_of(" \t\r\n"); if (l==std::string::npos) return "";
    auto r = s.find_last_not_of(" \t\r\n");  return s.substr(l, r-l+1);
}

namespace dinero::rpc {

// Check Bearer token auth. If header starts with "Bearer " we validate
// via AuthStore; otherwise fall back to existing cookie flow outside.
bool authorize_request(
    const std::string& auth_header,
    std::shared_ptr<dinero::auth::AuthStore> store,
    int& out_status,
    std::string& out_www_auth,
    std::string& out_json_body
) {
    const auto h = trim(auth_header);
    // Check if this is a Bearer token attempt (with or without space)
    if (h.size() >= 6 && std::strncmp(h.c_str(), "Bearer", 6) == 0) {
        // Must be followed by space or end of string
        if (h.size() == 6 || h[6] == ' ') {
            const std::string token = (h.size() > 7) ? trim(h.substr(7)) : "";
            if (token.empty()) {
            // Empty Bearer token - malformed
            out_status = 401;
            out_www_auth = R"(Bearer realm="Dinero RPC", error="invalid_request")";
            Json::Value root(Json::objectValue);
            Json::Value err(Json::objectValue);
            err["code"] = -32600;
            err["message"] = "Unauthorized: empty bearer token";
            root["error"] = err; 
            root["result"] = Json::nullValue;
            root["rpc_schema"] = "din.rpc.v1";
            root["schema_rev"] = 1;
            Json::StreamWriterBuilder w; w["indentation"] = "";
            out_json_body = Json::writeString(w, root);
            return false;
        }
        if (store->validateBearerAndTouch(token)) {
            return true; // OK via long-lived token
        }
        // unauthorized - invalid token
        out_status = 401;
        out_www_auth = R"(Bearer realm="Dinero RPC", error="invalid_token")";
        Json::Value root(Json::objectValue);
        Json::Value err(Json::objectValue);
        err["code"] = -32600;
        err["message"] = "Unauthorized: invalid or expired bearer token";
        root["error"] = err; 
        root["result"] = Json::nullValue;
        root["rpc_schema"] = "din.rpc.v1";
        root["schema_rev"] = 1;
        Json::StreamWriterBuilder w; w["indentation"] = "";
        out_json_body = Json::writeString(w, root);
        return false;
        }
    }

    // Otherwise, let upstream cookie-auth logic decide.
    // If upstream rejects, consider returning combined hint:
    // WWW-Authenticate: Basic realm="Dinero RPC", Bearer realm="Dinero RPC"
    return true;
}

} // namespace dinero::rpc
