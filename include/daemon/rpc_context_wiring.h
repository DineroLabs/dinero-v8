#pragma once

// Forward declarations
class HttpRpcServer;
struct DaemonContext;  // In global namespace

namespace dinero {

/**
 * Wire DaemonContext to the RPC system
 *
 * This function:
 * 1. Injects DaemonContext into HttpRpcServer
 * 2. Registers all context-aware RPC handlers
 *
 * Call this AFTER all services have started.
 *
 * @param ctx The DaemonContext with all initialized services
 * @param http_server The HTTP RPC server to wire context into
 * @return true if wiring succeeded, false otherwise
 */
bool WireRpcContext(DaemonContext& ctx, HttpRpcServer* http_server);

/**
 * Persist daemon_id to <datadir>/.daemon_id
 *
 * Optional Layer 2.5: Enables monitoring/automation tools to read daemon_id
 * without making RPC calls.
 *
 * @param ctx The DaemonContext with initialized services
 */
void PersistDaemonId(DaemonContext& ctx);

} // namespace dinero
