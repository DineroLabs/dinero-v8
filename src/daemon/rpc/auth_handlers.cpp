#include "daemon/rpc/auth_handlers.h"
#include "auth/auth_store.h"
#include "din_json.h"
#include <chrono>

using dinero::auth::AuthStore;
using sysclock = std::chrono::system_clock;

static Json::Value err(int code, const std::string& msg) {
    Json::Value e(Json::objectValue);
    e["code"] = code; e["message"] = msg;
    Json::Value root(Json::objectValue);
    root["error"] = e; root["result"] = Json::nullValue;
    root["rpc_schema"] = "din.rpc.v1";
    root["schema_rev"] = 1;
    return root;
}

namespace dinero::rpc {

Json::Value RpcCreateAuth(Json::Value params, std::shared_ptr<AuthStore> store) {
    const auto label = params.get("label","").asString();
    bool has_ttl = params.isMember("ttl_days") && !params["ttl_days"].isNull();
    int ttl_days = has_ttl ? params.get("ttl_days", 365).asInt() : -1; // -1 = no expiry

    if (label.empty()) return err(-32602, "Missing 'label'");

    // Server-side guardrails
    const int MAX_TTL_DAYS = 730;  // 2 years max (configurable later)
    const int MIN_TTL_DAYS = 1;    // Minimum 1 day
    
    if (has_ttl && ttl_days > 0) {
        if (ttl_days > MAX_TTL_DAYS) {
            return err(-32602, "ttl_days exceeds maximum allowed (" + std::to_string(MAX_TTL_DAYS) + " days)");
        }
        if (ttl_days < MIN_TTL_DAYS) {
            return err(-32602, "ttl_days must be >= " + std::to_string(MIN_TTL_DAYS) + " or null for no expiry");
        }
    }

    std::optional<sysclock::time_point> expires;
    if (has_ttl && ttl_days > 0) {
        expires = sysclock::now() + std::chrono::hours(24*ttl_days);
    }
    // If has_ttl=false or ttl_days=-1, expires remains nullopt (no expiry)

    std::string token_plain;
    auto hash_hex = store->createToken(label, expires, token_plain);

    Json::Value r(Json::objectValue);
    r["token"]   = token_plain; // plaintext token returned ONCE
    r["token_hash"] = hash_hex; // for management
    if (expires.has_value()) {
        std::time_t t = sysclock::to_time_t(*expires);
        std::tm tm{}; 
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[32]; std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        r["expires"] = buf;
    } else {
        r["expires"] = Json::nullValue;
    }

    Json::Value root(Json::objectValue);
    root["result"] = r; 
    root["error"] = Json::nullValue;
    root["rpc_schema"] = "din.rpc.v1";
    root["schema_rev"] = 1;
    return root;
}

Json::Value RpcListAuth(Json::Value, std::shared_ptr<AuthStore> store) {
    Json::Value arr(Json::arrayValue);
    for (const auto& t : store->listTokens()) {
        Json::Value o(Json::objectValue);
        o["label"]       = t.label;
        o["token_hash"]  = t.token_hash_hex;
        o["created_at"]  = t.created_at;
        o["last_used"]   = t.last_used;
        if (t.expires.has_value()) o["expires"] = *t.expires; else o["expires"] = Json::nullValue;
        o["revoked"]     = t.revoked;
        arr.append(std::move(o));
    }
    Json::Value root(Json::objectValue);
    root["result"] = arr; 
    root["error"] = Json::nullValue;
    root["rpc_schema"] = "din.rpc.v1";
    root["schema_rev"] = 1;
    return root;
}

Json::Value RpcRevokeAuth(Json::Value params, std::shared_ptr<AuthStore> store) {
    const auto hash_hex = params.get("token_hash","").asString();
    if (hash_hex.empty()) return err(-32602, "Missing 'token_hash'");
    const bool ok = store->revokeToken(hash_hex);
    Json::Value r(Json::objectValue);
    r["revoked"] = ok;
    Json::Value root(Json::objectValue);
    root["result"] = r; 
    root["error"] = Json::nullValue;
    root["rpc_schema"] = "din.rpc.v1";
    root["schema_rev"] = 1;
    return root;
}

// ===== din::Json versions (direct RPC integration) =====

din::Json RpcCreateAuthDnr(const din::Json& params, std::shared_ptr<AuthStore> store) {
    std::string label;
    bool has_ttl = false;
    int ttl_days = -1;
    
    if (params.isObject()) {
        if (params.isMember("label")) {
            label = params["label"].asString();
        }
        if (params.isMember("ttl_days") && !params["ttl_days"].isNull()) {
            has_ttl = true;
            ttl_days = params["ttl_days"].asInt();
        }
    }
    
    if (label.empty()) {
        throw std::runtime_error("Missing 'label'");
    }
    
    // Server-side guardrails
    const int MAX_TTL_DAYS = 730;  // 2 years max
    const int MIN_TTL_DAYS = 1;    // Minimum 1 day
    
    if (has_ttl) {
        if (ttl_days > MAX_TTL_DAYS) {
            throw std::runtime_error("ttl_days exceeds maximum allowed (" + std::to_string(MAX_TTL_DAYS) + " days)");
        }
        if (ttl_days < MIN_TTL_DAYS) {
            throw std::runtime_error("ttl_days must be >= " + std::to_string(MIN_TTL_DAYS) + " or null for no expiry");
        }
    }
    
    std::optional<sysclock::time_point> expires;
    if (has_ttl && ttl_days > 0) {
        expires = sysclock::now() + std::chrono::hours(24*ttl_days);
    }
    
    std::string token_plain;
    auto hash_hex = store->createToken(label, expires, token_plain);
    
    din::Json result = din::obj();
    result["token"] = token_plain;
    result["token_hash"] = hash_hex;
    
    if (expires.has_value()) {
        std::time_t t = sysclock::to_time_t(*expires);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        result["expires"] = std::string(buf);
    } else {
        result["expires"] = din::Json();  // null value
    }
    
    return result;
}

din::Json RpcListAuthDnr(const din::Json& params, std::shared_ptr<AuthStore> store) {
    auto tokens = store->listTokens();
    din::Json result = din::arr();
    
    for (const auto& token : tokens) {
        din::Json t = din::obj();
        t["token_hash"] = token.token_hash_hex;
        t["label"] = token.label;
        t["created"] = token.created_at;
        t["last_used"] = token.last_used;
        t["revoked"] = token.revoked;
        
        if (token.expires.has_value()) {
            t["expires"] = *token.expires;
        } else {
            t["expires"] = din::Json();  // null value
        }
        
        result.append(t);
    }
    
    return result;
}

din::Json RpcRevokeAuthDnr(const din::Json& params, std::shared_ptr<AuthStore> store) {
    std::string hash_hex;
    
    if (params.isObject() && params.isMember("token_hash")) {
        hash_hex = params["token_hash"].asString();
    }
    
    if (hash_hex.empty()) {
        throw std::runtime_error("Missing 'token_hash'");
    }
    
    bool ok = store->revokeToken(hash_hex);
    
    din::Json result = din::obj();
    result["revoked"] = ok;
    
    return result;
}

} // namespace dinero::rpc
