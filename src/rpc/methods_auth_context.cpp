/**
 * Auth RPC Methods - Context-Aware (Week 2 Migration)
 *
 * This file migrates auth RPC methods to use DaemonContext pattern.
 *
 * Note: TokenManager and SessionManager use singleton pattern, which is already
 * a form of good design (testable, no raw globals). However, for consistency
 * with the context-aware migration, we wrap access through ExecutionContext.
 *
 * PATTERN (singleton-based):
 *   auto& tm = TokenManager::instance();
 *   auto& sm = SessionManager::instance();
 *
 * Benefits of this migration:
 * - Consistent context-aware pattern across all RPC methods
 * - Easier to mock in tests by injecting different ExecutionContext
 * - Matches the architectural direction for Week 2
 */

#include "din_json.h"
#include "rpc/rpc_registry.h"
#include "rpc/token_manager.h"
#include "rpc/session_manager.h"
#include "daemon/daemon_context.h"
#include "common/logger.h"
#include <chrono>
#include <sstream>

// ═══════════════════════════════════════════════════════════════
// CONTEXT-AWARE AUTH RPC HANDLERS (Week 2 Pattern)
// ═══════════════════════════════════════════════════════════════

/**
 * auth.requesttoken - Generate a new access token
 */
din::Json rpc_context_auth_requesttoken(const ExecutionContext& ctx, const din::Json& params) {
    // Get client IP from context
    std::string client_ip = ctx.metadata.count("client_ip") ?
                            ctx.metadata.at("client_ip") : "127.0.0.1";

    // Default values
    std::string scope = "wallet";
    bool auto_refresh = true;
    bool persistent = false;
    int lifetime = 3600;
    std::string device_name = "";
    std::string user_agent = "";

    // Get user_agent from metadata if available
    if (ctx.metadata.count("user-agent")) {
        user_agent = ctx.metadata.at("user-agent");
    }

    // Parse parameters
    if (params.isObject()) {
        if (params.isMember("scope")) {
            scope = params["scope"].asString();
        }
        if (params.isMember("auto_refresh")) {
            auto_refresh = params["auto_refresh"].asBool();
        }
        if (params.isMember("persistent")) {
            persistent = params["persistent"].asBool();
        }
        if (params.isMember("lifetime")) {
            lifetime = params["lifetime"].asInt();
            if (lifetime < 60) lifetime = 60;
            if (lifetime > 86400) lifetime = 86400;
        }
        if (params.isMember("device_name")) {
            device_name = params["device_name"].asString();
        }
    } else if (params.isArray() && params.size() > 0 && params[0].isObject()) {
        const Json::Value& params_obj = params[0];
        if (params_obj.isMember("scope")) {
            scope = params_obj["scope"].asString();
        }
        if (params_obj.isMember("auto_refresh")) {
            auto_refresh = params_obj["auto_refresh"].asBool();
        }
        if (params_obj.isMember("persistent")) {
            persistent = params_obj["persistent"].asBool();
        }
        if (params_obj.isMember("lifetime")) {
            lifetime = params_obj["lifetime"].asInt();
            if (lifetime < 60) lifetime = 60;
            if (lifetime > 86400) lifetime = 86400;
        }
        if (params_obj.isMember("device_name")) {
            device_name = params_obj["device_name"].asString();
        }
    }

    // Generate token (singleton pattern is acceptable here)
    auto& tm = dinero::rpc::TokenManager::instance();
    std::string token = tm.GenerateToken(client_ip, scope, auto_refresh, persistent, lifetime);

    // Create session
    auto& sm = dinero::rpc::SessionManager::instance();
    std::string session_id = sm.CreateSession(
        token, device_name, client_ip, user_agent, scope, persistent, lifetime
    );

    // Build response
    din::Json result = din::obj();
    result["token"] = token;
    result["session_id"] = session_id;
    result["expires_in"] = persistent ? -1 : lifetime;
    result["scope"] = scope;
    result["auto_refresh"] = auto_refresh;
    result["persistent"] = persistent;
    if (!device_name.empty()) {
        result["device_name"] = device_name;
    }

    return result;
}

/**
 * auth.refreshtoken - Manually refresh token lifetime
 */
din::Json rpc_context_auth_refreshtoken(const ExecutionContext& ctx, const din::Json& params) {
    const Json::Value* params_obj = &params;
    if (params.isArray() && params.size() > 0) {
        if (params[0].isObject()) {
            params_obj = &params[0];
        } else if (params[0].isString()) {
            std::string token = params[0].asString();
            auto& tm = dinero::rpc::TokenManager::instance();
            std::string refreshed = tm.RefreshToken(token);

            if (refreshed.empty()) {
                throw std::runtime_error("Token not found or refresh failed");
            }

            din::Json result = din::obj();
            result["token"] = refreshed;
            result["expires_in"] = 3600;
            result["message"] = "Token lifetime extended";
            return result;
        }
    }

    if (!params_obj->isMember("token")) {
        throw std::runtime_error("Missing required parameter: token");
    }

    std::string token = (*params_obj)["token"].asString();

    auto& tm = dinero::rpc::TokenManager::instance();
    std::string refreshed = tm.RefreshToken(token);

    if (refreshed.empty()) {
        throw std::runtime_error("Token not found or refresh failed");
    }

    din::Json result = din::obj();
    result["token"] = refreshed;
    result["expires_in"] = 3600;
    result["message"] = "Token lifetime extended";

    return result;
}

/**
 * auth.revoketoken - Revoke (delete) a token
 */
din::Json rpc_context_auth_revoketoken(const ExecutionContext& ctx, const din::Json& params) {
    const Json::Value* params_obj = &params;
    if (params.isArray() && params.size() > 0) {
        if (params[0].isObject()) {
            params_obj = &params[0];
        } else if (params[0].isString()) {
            std::string token = params[0].asString();

            auto& sm = dinero::rpc::SessionManager::instance();
            sm.RemoveSessionByToken(token);

            auto& tm = dinero::rpc::TokenManager::instance();
            bool revoked = tm.RevokeToken(token);

            din::Json result = din::obj();
            result["success"] = revoked;
            result["message"] = revoked ? "Token revoked" : "Token not found";
            return result;
        }
    }

    if (!params_obj->isMember("token")) {
        throw std::runtime_error("Missing required parameter: token");
    }

    std::string token = (*params_obj)["token"].asString();

    auto& sm = dinero::rpc::SessionManager::instance();
    sm.RemoveSessionByToken(token);

    auto& tm = dinero::rpc::TokenManager::instance();
    bool revoked = tm.RevokeToken(token);

    din::Json result = din::obj();
    result["success"] = revoked;
    result["message"] = revoked ? "Token revoked" : "Token not found";

    return result;
}

/**
 * auth.whoami - Get current client information
 */
din::Json rpc_context_auth_whoami(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result = din::obj();
    result["client_id"] = ctx.client_id;
    result["wallet_name"] = ctx.walletName;
    result["user"] = ctx.user;

    if (ctx.metadata.count("client_ip")) {
        result["client_ip"] = ctx.metadata.at("client_ip");
    }

    return result;
}

/**
 * auth.stats - Get token statistics
 */
din::Json rpc_context_auth_stats(const ExecutionContext& ctx, const din::Json& params) {
    auto& tm = dinero::rpc::TokenManager::instance();
    std::string stats_json = tm.GetStatistics();

    Json::CharReaderBuilder reader;
    Json::Value result;
    std::istringstream ss(stats_json);
    std::string errs;

    if (!Json::parseFromStream(reader, ss, &result, &errs)) {
        throw std::runtime_error("Failed to parse statistics: " + errs);
    }

    return result;
}

/**
 * auth.sessions.list - List all active sessions
 */
din::Json rpc_context_auth_sessions_list(const ExecutionContext& ctx, const din::Json& params) {
    auto& sm = dinero::rpc::SessionManager::instance();
    auto sessions = sm.ListSessions();

    din::Json result = din::obj();
    din::Json sessions_array = din::arr();

    for (const auto& session : sessions) {
        din::Json session_obj = din::obj();
        session_obj["session_id"] = session.session_id;
        session_obj["device_name"] = session.device_name;
        session_obj["ip_address"] = session.ip_address;
        session_obj["platform"] = session.platform;
        session_obj["scope"] = session.scope;
        session_obj["persistent"] = session.persistent;

        auto created_sec = std::chrono::duration_cast<std::chrono::seconds>(
            session.created_at.time_since_epoch()).count();
        auto last_used_sec = std::chrono::duration_cast<std::chrono::seconds>(
            session.last_used.time_since_epoch()).count();

        session_obj["created_at"] = static_cast<Json::Int64>(created_sec);
        session_obj["last_used"] = static_cast<Json::Int64>(last_used_sec);

        auto now = std::chrono::system_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - session.last_used);
        session_obj["last_used_ago"] = static_cast<int>(duration.count());

        if (session.persistent) {
            session_obj["expires_at"] = -1;
            session_obj["expires_in"] = -1;
        } else {
            auto expires_sec = std::chrono::duration_cast<std::chrono::seconds>(
                session.expires_at.time_since_epoch()).count();
            session_obj["expires_at"] = static_cast<Json::Int64>(expires_sec);

            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                session.expires_at - now);
            session_obj["expires_in"] = static_cast<int>(remaining.count());
        }

        if (!session.user_agent.empty()) {
            session_obj["user_agent"] = session.user_agent;
        }

        sessions_array.append(session_obj);
    }

    result["sessions"] = sessions_array;
    result["total_sessions"] = static_cast<int>(sessions.size());

    return result;
}

/**
 * auth.sessions.revoke - Revoke a session (disconnect device)
 */
din::Json rpc_context_auth_sessions_revoke(const ExecutionContext& ctx, const din::Json& params) {
    std::string session_id;

    if (params.isObject() && params.isMember("session_id")) {
        session_id = params["session_id"].asString();
    } else if (params.isArray() && params.size() > 0) {
        if (params[0].isObject() && params[0].isMember("session_id")) {
            session_id = params[0]["session_id"].asString();
        } else if (params[0].isString()) {
            session_id = params[0].asString();
        }
    }

    if (session_id.empty()) {
        throw std::runtime_error("Missing required parameter: session_id");
    }

    auto& sm = dinero::rpc::SessionManager::instance();
    bool success = sm.RevokeSession(session_id);

    din::Json result = din::obj();
    result["success"] = success;

    if (success) {
        result["message"] = "Session revoked successfully";
    } else {
        result["message"] = "Session not found";
    }

    return result;
}

/**
 * auth.sessions.rename - Rename a session (change device name)
 */
din::Json rpc_context_auth_sessions_rename(const ExecutionContext& ctx, const din::Json& params) {
    std::string session_id;
    std::string device_name;

    if (params.isObject()) {
        if (!params.isMember("session_id") || !params.isMember("device_name")) {
            throw std::runtime_error("Missing required parameters: session_id and device_name");
        }
        session_id = params["session_id"].asString();
        device_name = params["device_name"].asString();
    } else if (params.isArray() && params.size() >= 2) {
        if (params[0].isString() && params[1].isString()) {
            session_id = params[0].asString();
            device_name = params[1].asString();
        } else if (params[0].isObject()) {
            const Json::Value& params_obj = params[0];
            if (!params_obj.isMember("session_id") || !params_obj.isMember("device_name")) {
                throw std::runtime_error("Missing required parameters: session_id and device_name");
            }
            session_id = params_obj["session_id"].asString();
            device_name = params_obj["device_name"].asString();
        }
    } else {
        throw std::runtime_error("Missing required parameters: session_id and device_name");
    }

    auto& sm = dinero::rpc::SessionManager::instance();
    bool success = sm.RenameSession(session_id, device_name);

    din::Json result = din::obj();
    result["success"] = success;

    if (success) {
        result["message"] = "Session renamed successfully";
        result["new_name"] = device_name;
    } else {
        result["message"] = "Session not found";
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
// REGISTRATION FUNCTION
// ═══════════════════════════════════════════════════════════════

extern RpcRegistry g_rpcRegistry;

void registerAuthMethodsContext() {
    g_rpcRegistry.registerHandler("auth.requesttoken",
                                 rpc_context_auth_requesttoken,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("auth.refreshtoken",
                                 rpc_context_auth_refreshtoken,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("auth.revoketoken",
                                 rpc_context_auth_revoketoken,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("auth.whoami",
                                 rpc_context_auth_whoami,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("auth.stats",
                                 rpc_context_auth_stats,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("auth.sessions.list",
                                 rpc_context_auth_sessions_list,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("auth.sessions.revoke",
                                 rpc_context_auth_sessions_revoke,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    g_rpcRegistry.registerHandler("auth.sessions.rename",
                                 rpc_context_auth_sessions_rename,
                                 RegisterMode::Overwrite,
                                 "context-aware");

    dinero::g_logger.info("[RPC Context] ✅ 8 auth context-aware handlers registered");
}
