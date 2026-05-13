#include "rpc/methods_auth.h"
#include "rpc/rpc_registry.h"
#include "rpc/token_manager.h"
#include "rpc/session_manager.h"
#include "din_json.h"
#include "common/logger.h"
#include <chrono>

extern RpcRegistry g_rpcRegistry;

namespace dinero {
namespace rpc {

/**
 * auth.requesttoken - Generate a new access token
 *
 * Parameters:
 *   - scope (string, optional): "wallet", "mining", "read", "admin" (default: "wallet")
 *   - auto_refresh (bool, optional): Auto-extend lifetime on use (default: true)
 *   - persistent (bool, optional): Never expires (default: false)
 *   - lifetime (int, optional): Token lifetime in seconds (default: 3600)
 *
 * Returns:
 *   - token (string): Generated token
 *   - expires_in (int): Seconds until expiration
 *   - scope (string): Access scope
 *   - auto_refresh (bool): Auto-refresh enabled
 *   - persistent (bool): Never expires
 */
din::Json rpc_auth_requesttoken(const ExecutionContext& ctx, const din::Json& params) {
    // Get client IP from context
    std::string client_ip = ctx.metadata.count("client_ip") ?
                            ctx.metadata.at("client_ip") : "127.0.0.1";

    // Default values
    std::string scope = "wallet";
    bool auto_refresh = true;
    bool persistent = false;
    int lifetime = 3600;
    std::string device_name = "";  // Optional device name
    std::string user_agent = "";   // Extract from metadata if available

    // Get user_agent from metadata if available
    if (ctx.metadata.count("user-agent")) {
        user_agent = ctx.metadata.at("user-agent");
    }

    // Parse parameters only if params is an object or array
    if (params.isObject()) {
        // Direct object format: {"scope": "wallet", "device_name": "My Desktop"}
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
            // Limit to reasonable range
            if (lifetime < 60) lifetime = 60;           // Min 1 minute
            if (lifetime > 86400) lifetime = 86400;     // Max 24 hours
        }
        if (params.isMember("device_name")) {
            device_name = params["device_name"].asString();
        }
    } else if (params.isArray() && params.size() > 0 && params[0].isObject()) {
        // Array format: [{"scope": "wallet", "device_name": "My Desktop"}]
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
            // Limit to reasonable range
            if (lifetime < 60) lifetime = 60;           // Min 1 minute
            if (lifetime > 86400) lifetime = 86400;     // Max 24 hours
        }
        if (params_obj.isMember("device_name")) {
            device_name = params_obj["device_name"].asString();
        }
    }
    // If params is null or empty, use defaults

    // Generate token
    auto& tm = TokenManager::instance();
    std::string token = tm.GenerateToken(client_ip, scope, auto_refresh, persistent, lifetime);

    // Create session for this token
    auto& sm = SessionManager::instance();
    std::string session_id = sm.CreateSession(
        token,
        device_name,  // Can be empty, will default to "Unnamed Device"
        client_ip,
        user_agent,
        scope,
        persistent,
        lifetime
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
 *
 * Parameters:
 *   - token (string, required): Token to refresh
 *
 * Returns:
 *   - token (string): Same token (lifetime extended)
 *   - expires_in (int): New expiration time (seconds)
 */
din::Json rpc_auth_refreshtoken(const ExecutionContext& ctx, const din::Json& params) {
    // Support both object and array formats
    const Json::Value* params_obj = &params;
    if (params.isArray() && params.size() > 0) {
        if (params[0].isObject()) {
            params_obj = &params[0];
        } else if (params[0].isString()) {
            // Direct string parameter
            std::string token = params[0].asString();
            auto& tm = TokenManager::instance();
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

    auto& tm = TokenManager::instance();
    std::string refreshed = tm.RefreshToken(token);

    if (refreshed.empty()) {
        throw std::runtime_error("Token not found or refresh failed");
    }

    din::Json result = din::obj();
    result["token"] = refreshed;
    result["expires_in"] = 3600;  // Extended by 1 hour
    result["message"] = "Token lifetime extended";

    return result;
}

/**
 * auth.revoketoken - Revoke (delete) a token
 *
 * Parameters:
 *   - token (string, required): Token to revoke
 *
 * Returns:
 *   - success (bool): true if revoked
 */
din::Json rpc_auth_revoketoken(const ExecutionContext& ctx, const din::Json& params) {
    // Support both object and array formats
    const Json::Value* params_obj = &params;
    if (params.isArray() && params.size() > 0) {
        if (params[0].isObject()) {
            params_obj = &params[0];
        } else if (params[0].isString()) {
            // Direct string parameter
            std::string token = params[0].asString();

            // Remove session first
            auto& sm = SessionManager::instance();
            sm.RemoveSessionByToken(token);

            // Then revoke token
            auto& tm = TokenManager::instance();
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

    // Remove session first
    auto& sm = SessionManager::instance();
    sm.RemoveSessionByToken(token);

    // Then revoke token
    auto& tm = TokenManager::instance();
    bool revoked = tm.RevokeToken(token);

    din::Json result = din::obj();
    result["success"] = revoked;
    result["message"] = revoked ? "Token revoked" : "Token not found";

    return result;
}

/**
 * auth.whoami - Get current client information
 *
 * Returns client identity from ExecutionContext
 */
din::Json rpc_auth_whoami(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result = din::obj();
    result["client_id"] = ctx.client_id;
    result["wallet_name"] = ctx.walletName;
    result["user"] = ctx.user;

    // Add client IP if available
    if (ctx.metadata.count("client_ip")) {
        result["client_ip"] = ctx.metadata.at("client_ip");
    }

    return result;
}

/**
 * auth.stats - Get token statistics
 *
 * Returns:
 *   - total_tokens (int): Total active tokens
 *   - persistent_tokens (int): Tokens that never expire
 *   - auto_refresh_tokens (int): Tokens with auto-refresh
 *   - by_scope (object): Token counts by scope
 */
din::Json rpc_auth_stats(const ExecutionContext& ctx, const din::Json& params) {
    auto& tm = TokenManager::instance();
    std::string stats_json = tm.GetStatistics();

    // Parse JSON string back to Json::Value
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
 *
 * Returns information about all connected devices/applications
 *
 * Returns:
 *   - sessions (array): List of active sessions with metadata
 *   - total_sessions (int): Total number of active sessions
 */
din::Json rpc_auth_sessions_list(const ExecutionContext& ctx, const din::Json& params) {
    auto& sm = SessionManager::instance();
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

        // Convert timestamps to seconds since epoch
        auto created_sec = std::chrono::duration_cast<std::chrono::seconds>(
            session.created_at.time_since_epoch()).count();
        auto last_used_sec = std::chrono::duration_cast<std::chrono::seconds>(
            session.last_used.time_since_epoch()).count();

        session_obj["created_at"] = static_cast<Json::Int64>(created_sec);
        session_obj["last_used"] = static_cast<Json::Int64>(last_used_sec);

        // Calculate time ago
        auto now = std::chrono::system_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - session.last_used);
        session_obj["last_used_ago"] = static_cast<int>(duration.count());

        // Expiry info
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

        // Add user agent if present
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
 *
 * Parameters:
 *   - session_id (string, required): Session ID to revoke
 *
 * Returns:
 *   - success (bool): Whether session was revoked
 *   - message (string): Result message
 */
din::Json rpc_auth_sessions_revoke(const ExecutionContext& ctx, const din::Json& params) {
    // Parse session_id from params
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

    auto& sm = SessionManager::instance();
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
 *
 * Parameters:
 *   - session_id (string, required): Session ID to rename
 *   - device_name (string, required): New device name
 *
 * Returns:
 *   - success (bool): Whether session was renamed
 *   - message (string): Result message
 */
din::Json rpc_auth_sessions_rename(const ExecutionContext& ctx, const din::Json& params) {
    std::string session_id;
    std::string device_name;

    // Parse parameters
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

    auto& sm = SessionManager::instance();
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

/**
 * Register all authentication methods in the RPC registry
 */
void registerAuthMethods() {
    // auth.requesttoken
    RpcMethodMeta requestTokenMeta;
    requestTokenMeta.name = "auth.requesttoken";
    requestTokenMeta.ns = "auth";
    requestTokenMeta.description = "Generate a new access token for RPC authentication";

    RpcParamMeta scopeParam;
    scopeParam.name = "scope";
    scopeParam.type = "string";
    scopeParam.desc = "Access scope: wallet, mining, read, admin (default: wallet)";
    scopeParam.required = false;
    requestTokenMeta.params.push_back(scopeParam);

    RpcParamMeta autoRefreshParam;
    autoRefreshParam.name = "auto_refresh";
    autoRefreshParam.type = "boolean";
    autoRefreshParam.desc = "Auto-extend lifetime on use (default: true)";
    autoRefreshParam.required = false;
    requestTokenMeta.params.push_back(autoRefreshParam);

    RpcParamMeta persistentParam;
    persistentParam.name = "persistent";
    persistentParam.type = "boolean";
    persistentParam.desc = "Never expires (default: false)";
    persistentParam.required = false;
    requestTokenMeta.params.push_back(persistentParam);

    RpcParamMeta lifetimeParam;
    lifetimeParam.name = "lifetime";
    lifetimeParam.type = "number";
    lifetimeParam.desc = "Token lifetime in seconds (default: 3600, min: 60, max: 86400)";
    lifetimeParam.required = false;
    requestTokenMeta.params.push_back(lifetimeParam);

    RpcParamMeta deviceNameParam;
    deviceNameParam.name = "device_name";
    deviceNameParam.type = "string";
    deviceNameParam.desc = "Device name for session tracking (e.g., 'Desktop Wallet', 'Trading Bot')";
    deviceNameParam.required = false;
    requestTokenMeta.params.push_back(deviceNameParam);

    requestTokenMeta.result.type = "object";
    requestTokenMeta.result.desc = "Token information including token string, session_id, expiry, and features";

    g_rpcRegistry.registerHandler("auth.requesttoken", rpc_auth_requesttoken, requestTokenMeta, "auth");

    // auth.refreshtoken
    RpcMethodMeta refreshTokenMeta;
    refreshTokenMeta.name = "auth.refreshtoken";
    refreshTokenMeta.ns = "auth";
    refreshTokenMeta.description = "Manually refresh token lifetime";

    RpcParamMeta tokenParam;
    tokenParam.name = "token";
    tokenParam.type = "string";
    tokenParam.desc = "Token to refresh";
    tokenParam.required = true;
    refreshTokenMeta.params.push_back(tokenParam);

    refreshTokenMeta.result.type = "object";
    refreshTokenMeta.result.desc = "Refreshed token information";

    g_rpcRegistry.registerHandler("auth.refreshtoken", rpc_auth_refreshtoken, refreshTokenMeta, "auth");

    // auth.revoketoken
    RpcMethodMeta revokeTokenMeta;
    revokeTokenMeta.name = "auth.revoketoken";
    revokeTokenMeta.ns = "auth";
    revokeTokenMeta.description = "Revoke (delete) a token";

    RpcParamMeta revokeTokenParam;
    revokeTokenParam.name = "token";
    revokeTokenParam.type = "string";
    revokeTokenParam.desc = "Token to revoke";
    revokeTokenParam.required = true;
    revokeTokenMeta.params.push_back(revokeTokenParam);

    revokeTokenMeta.result.type = "object";
    revokeTokenMeta.result.desc = "Revocation status";

    g_rpcRegistry.registerHandler("auth.revoketoken", rpc_auth_revoketoken, revokeTokenMeta, "auth");

    // auth.whoami
    RpcMethodMeta whoamiMeta;
    whoamiMeta.name = "auth.whoami";
    whoamiMeta.ns = "auth";
    whoamiMeta.description = "Get current client information";
    whoamiMeta.result.type = "object";
    whoamiMeta.result.desc = "Client identity and connection info";

    g_rpcRegistry.registerHandler("auth.whoami", rpc_auth_whoami, whoamiMeta, "auth");

    // auth.stats
    RpcMethodMeta statsMeta;
    statsMeta.name = "auth.stats";
    statsMeta.ns = "auth";
    statsMeta.description = "Get token statistics";
    statsMeta.result.type = "object";
    statsMeta.result.desc = "Token counts by scope and type";

    g_rpcRegistry.registerHandler("auth.stats", rpc_auth_stats, statsMeta, "auth");

    // auth.sessions.list
    RpcMethodMeta sessionsListMeta;
    sessionsListMeta.name = "auth.sessions.list";
    sessionsListMeta.ns = "auth";
    sessionsListMeta.description = "List all active sessions (connected devices)";
    sessionsListMeta.result.type = "object";
    sessionsListMeta.result.desc = "List of active sessions with device names, IPs, and timestamps";

    g_rpcRegistry.registerHandler("auth.sessions.list", rpc_auth_sessions_list, sessionsListMeta, "auth");

    // auth.sessions.revoke
    RpcMethodMeta sessionsRevokeMeta;
    sessionsRevokeMeta.name = "auth.sessions.revoke";
    sessionsRevokeMeta.ns = "auth";
    sessionsRevokeMeta.description = "Revoke a session (disconnect device)";

    RpcParamMeta sessionIdParam;
    sessionIdParam.name = "session_id";
    sessionIdParam.type = "string";
    sessionIdParam.desc = "Session ID to revoke";
    sessionIdParam.required = true;
    sessionsRevokeMeta.params.push_back(sessionIdParam);

    sessionsRevokeMeta.result.type = "object";
    sessionsRevokeMeta.result.desc = "Success status and message";

    g_rpcRegistry.registerHandler("auth.sessions.revoke", rpc_auth_sessions_revoke, sessionsRevokeMeta, "auth");

    // auth.sessions.rename
    RpcMethodMeta sessionsRenameMeta;
    sessionsRenameMeta.name = "auth.sessions.rename";
    sessionsRenameMeta.ns = "auth";
    sessionsRenameMeta.description = "Rename a session (change device name)";

    RpcParamMeta sessionIdParam2;
    sessionIdParam2.name = "session_id";
    sessionIdParam2.type = "string";
    sessionIdParam2.desc = "Session ID to rename";
    sessionIdParam2.required = true;
    sessionsRenameMeta.params.push_back(sessionIdParam2);

    RpcParamMeta newDeviceNameParam;
    newDeviceNameParam.name = "device_name";
    newDeviceNameParam.type = "string";
    newDeviceNameParam.desc = "New device name";
    newDeviceNameParam.required = true;
    sessionsRenameMeta.params.push_back(newDeviceNameParam);

    sessionsRenameMeta.result.type = "object";
    sessionsRenameMeta.result.desc = "Success status and message";

    g_rpcRegistry.registerHandler("auth.sessions.rename", rpc_auth_sessions_rename, sessionsRenameMeta, "auth");

    dinero::g_logger.info("✅ Authentication RPC methods registered (8 methods)");
}

} // namespace rpc
} // namespace dinero
