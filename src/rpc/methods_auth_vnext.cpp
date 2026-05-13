/**
 * Auth RPC Methods - vNext Architecture
 *
 * Full migration to RPC_METHOD DSL with complete metadata.
 * Token-based authentication and session management.
 */

#include "rpc/rpc_method_builder.h"
#include "rpc/methods_auth.h"
#include "common/logger.h"

namespace din {
namespace rpc {

// Implementation functions from methods_auth.cpp
extern din::Json rpc_auth_requesttoken(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_auth_refreshtoken(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_auth_revoketoken(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_auth_whoami(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_auth_stats(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_auth_sessions_list(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_auth_sessions_revoke(const ExecutionContext& ctx, const din::Json& params);
extern din::Json rpc_auth_sessions_rename(const ExecutionContext& ctx, const din::Json& params);

void registerAuthMethodsVNext() {
    // ═══════════════════════════════════════════════════════════════
    // TOKEN MANAGEMENT
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("auth.requesttoken", "auth")
        .description("Generate a new authentication token for API access")
        .param("scope", "string", "Token scope: wallet, mining, read, admin (default: wallet)", false)
        .param("auto_refresh", "boolean", "Auto-extend lifetime on use (default: true)", false)
        .param("persistent", "boolean", "Never expires (default: false)", false)
        .param("lifetime", "number", "Token lifetime in seconds, 60-86400 (default: 3600)", false)
        .param("device_name", "string", "Device identifier for this token", false)
        .result("object", "Token data with token string, expiration time, and scope")
        .handler(rpc_auth_requesttoken)
        .examples({
            "auth.requesttoken",
            "auth.requesttoken '{\"scope\":\"wallet\",\"device_name\":\"My Laptop\"}'",
            "auth.requesttoken '{\"scope\":\"admin\",\"lifetime\":7200}'"
        });

    RPC_METHOD("auth.refreshtoken", "auth")
        .description("Refresh an existing authentication token to extend its lifetime")
        .param("token", "string", "Existing token to refresh", true)
        .result("object", "New token data with updated expiration")
        .handler(rpc_auth_refreshtoken)
        .examples({
            "auth.refreshtoken \"dint_abc123...\""
        });

    RPC_METHOD("auth.revoketoken", "auth")
        .description("Revoke an authentication token, immediately invalidating it")
        .param("token", "string", "Token to revoke (default: current token)", false)
        .result("object", "Revocation confirmation with revoked token info")
        .handler(rpc_auth_revoketoken)
        .examples({
            "auth.revoketoken",
            "auth.revoketoken \"dint_abc123...\""
        });

    RPC_METHOD("auth.whoami", "auth")
        .description("Get information about the current authenticated session")
        .params({})
        .result("object", "Session info including token, scope, IP, and expiration")
        .handler(rpc_auth_whoami)
        .examples({
            "auth.whoami"
        });

    RPC_METHOD("auth.stats", "auth")
        .description("Get authentication system statistics")
        .params({})
        .result("object", "Stats including active tokens, sessions, and revoked count")
        .handler(rpc_auth_stats)
        .examples({
            "auth.stats"
        });

    // ═══════════════════════════════════════════════════════════════
    // SESSION MANAGEMENT
    // ═══════════════════════════════════════════════════════════════

    RPC_METHOD("auth.sessions.list", "auth")
        .description("List all active sessions for the current token")
        .params({})
        .result("array", "Array of active sessions with IDs, devices, IPs, and timestamps")
        .handler(rpc_auth_sessions_list)
        .examples({
            "auth.sessions.list"
        });

    RPC_METHOD("auth.sessions.revoke", "auth")
        .description("Revoke a specific session by session ID")
        .param("session_id", "string", "Session ID to revoke", true)
        .result("object", "Revocation confirmation")
        .handler(rpc_auth_sessions_revoke)
        .examples({
            "auth.sessions.revoke \"sess_abc123...\""
        });

    RPC_METHOD("auth.sessions.rename", "auth")
        .description("Rename a session to a more descriptive device name")
        .param("session_id", "string", "Session ID to rename", true)
        .param("device_name", "string", "New device name", true)
        .result("object", "Updated session info")
        .handler(rpc_auth_sessions_rename)
        .examples({
            "auth.sessions.rename \"sess_abc123...\" \"My Desktop PC\""
        });

    dinero::g_logger.info("✅ Registered 8 auth methods (vNext DSL)");
}

// Auto-register at program startup
static auto _auth_vnext_init = (din::rpc::registerAuthMethodsVNext(), 0);

} // namespace rpc
} // namespace din
