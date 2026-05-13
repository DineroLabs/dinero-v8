#pragma once

#include "rpc_registry.h"
#include "http_rpc_server.h"
#include <json/json.h>

/**
 * RpcAdapter - Bridge between vNext RpcRegistry and legacy HttpRpcServer
 *
 * This adapter allows RPC methods registered in the modern RpcRegistry
 * to be automatically exposed through the legacy HttpRpcServer interface.
 *
 * Benefits:
 * - Single source of truth for RPC method definitions
 * - Methods work across HTTP, WebSocket, and future transports
 * - Eliminates duplicate registration code
 * - Enables gradual migration from legacy to vNext
 *
 * Usage:
 *   RpcRegistry registry;
 *   HttpRpcServer* http_server = ...;
 *
 *   // Register methods in vNext registry
 *   registry.registerHandler("getinfo", handler);
 *
 *   // Bind to legacy HTTP server
 *   RpcAdapter adapter(http_server);
 *   adapter.bind(registry);
 */
class RpcAdapter {
public:
    explicit RpcAdapter(HttpRpcServer* http_server);

    /**
     * Bind all methods from RpcRegistry to HttpRpcServer
     *
     * This iterates through all registered RPC methods and creates
     * HTTP handler wrappers that convert between Json::Value (legacy)
     * and din::Json (vNext).
     */
    void bind(RpcRegistry& registry);

    /**
     * Bind a single method from RpcRegistry to HttpRpcServer
     *
     * Useful for selective migration or testing.
     */
    void bindMethod(RpcRegistry& registry, const std::string& method_name);

private:
    HttpRpcServer* http_server_;

    // Convert legacy Json::Value to vNext din::Json
    static din::Json convertToVNext(const Json::Value& legacy_json);

    // Convert vNext din::Json to legacy Json::Value
    static Json::Value convertToLegacy(const din::Json& vnext_json);
};
