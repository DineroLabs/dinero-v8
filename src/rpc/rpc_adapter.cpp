/**
 * RpcAdapter Implementation
 *
 * Bridges vNext RpcRegistry to legacy HttpRpcServer
 */

#include "rpc/rpc_adapter.h"
#include "common/logger.h"
#include <iostream>

RpcAdapter::RpcAdapter(HttpRpcServer* http_server)
    : http_server_(http_server) {
    if (!http_server_) {
        throw std::runtime_error("RpcAdapter: http_server cannot be null");
    }
}

void RpcAdapter::bind(RpcRegistry& registry) {
    if (!http_server_) {
        std::cerr << "[RpcAdapter] Error: HTTP server is null" << std::endl;
        return;
    }

    std::cout << "[RpcAdapter] Binding RpcRegistry methods to HttpRpcServer..." << std::endl;

    // Get all registered methods
    auto methods = registry.methodNames();
    int bound_count = 0;

    for (const auto& method_name : methods) {
        try {
            bindMethod(registry, method_name);
            bound_count++;
        } catch (const std::exception& e) {
            std::cerr << "[RpcAdapter] Failed to bind method '" << method_name
                      << "': " << e.what() << std::endl;
        }
    }

    std::cout << "[RpcAdapter] Bound " << bound_count << " methods from RpcRegistry" << std::endl;
}

void RpcAdapter::bindMethod(RpcRegistry& registry, const std::string& method_name) {
    // Get handler from registry
    RpcHandler* handler_ptr = registry.lookup(method_name);
    if (!handler_ptr) {
        throw std::runtime_error("Method not found in registry: " + method_name);
    }

    // Copy the handler (must capture by value for lambda)
    RpcHandler handler = *handler_ptr;

    // Register with HTTP server
    http_server_->register_method(method_name, [handler, method_name](const Json::Value& params) -> Json::Value {
        try {
            // Create execution context
            ExecutionContext ctx;
            // Note: HttpRpcServer could pass client IP here in future
            // ctx.client_address = "127.0.0.1";

            // Convert params to vNext format
            din::Json vnext_params = convertToVNext(params);

            // Call vNext handler
            din::Json vnext_result = handler(ctx, vnext_params);

            // Convert result back to legacy format
            return convertToLegacy(vnext_result);

        } catch (const std::exception& e) {
            // Return JSON-RPC error
            Json::Value error_response;
            error_response["error"]["code"] = -32603;
            error_response["error"]["message"] = std::string("Internal error: ") + e.what();
            return error_response;
        }
    });
}

// Convert legacy Json::Value to vNext din::Json
din::Json RpcAdapter::convertToVNext(const Json::Value& legacy_json) {
    // Since both use jsoncpp under the hood, this is often a direct copy
    // In your codebase, din::Json IS Json::Value (typedef)
    return legacy_json;
}

// Convert vNext din::Json to legacy Json::Value
Json::Value RpcAdapter::convertToLegacy(const din::Json& vnext_json) {
    // Since both use jsoncpp under the hood, this is often a direct copy
    // In your codebase, din::Json IS Json::Value (typedef)
    return vnext_json;
}
