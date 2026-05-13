// OpenRPC auto-documentation RPC methods
#include "rpc/rpc_registry.h"
#include "rpc/openrpc_generator.h"
#include "din_json.h"
#include "common/logger.h"

extern RpcRegistry g_rpcRegistry;

namespace dinero {
namespace rpc {

// ═══════════════════════════════════════════════════════════
// rpc.openrpc - Get full OpenRPC schema
// ═══════════════════════════════════════════════════════════

static din::Json rpc_openrpc(const ExecutionContext& ctx, const din::Json& params) {
    try {
        OpenRpcGenerator generator(&g_rpcRegistry);
        return generator.generateSchema();
    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = std::string("Failed to generate OpenRPC schema: ") + e.what();
        return error;
    }
}

// ═══════════════════════════════════════════════════════════
// rpc.schema - Get schema for specific method
// ═══════════════════════════════════════════════════════════

static din::Json rpc_schema(const ExecutionContext& ctx, const din::Json& params) {
    try {
        if (params.empty() || !params[0].isString()) {
            din::Json error;
            error["error"] = "Missing required parameter: method name";
            return error;
        }

        std::string method_name = params[0].asString();
        OpenRpcGenerator generator(&g_rpcRegistry);

        din::Json schema = generator.generateMethodSchema(method_name);
        if (schema.isNull()) {
            din::Json error;
            error["error"] = "Method not found: " + method_name;
            return error;
        }

        return schema;
    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = std::string("Failed to generate method schema: ") + e.what();
        return error;
    }
}

// ═══════════════════════════════════════════════════════════
// rpc.namespaces - Get methods grouped by namespace
// ═══════════════════════════════════════════════════════════

static din::Json rpc_namespaces(const ExecutionContext& ctx, const din::Json& params) {
    try {
        OpenRpcGenerator generator(&g_rpcRegistry);
        return generator.getMethodsByNamespace();
    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = std::string("Failed to get namespaces: ") + e.what();
        return error;
    }
}

// ═══════════════════════════════════════════════════════════
// rpc.apihash - Get API version hash for caching
// ═══════════════════════════════════════════════════════════

static din::Json rpc_apihash(const ExecutionContext& ctx, const din::Json& params) {
    try {
        OpenRpcGenerator generator(&g_rpcRegistry);

        din::Json result;
        result["hash"] = generator.getApiVersionHash();
        result["algorithm"] = "sha256";
        result["description"] = "SHA256 hash of all method signatures. Changes when API is modified.";
        return result;
    } catch (const std::exception& e) {
        din::Json error;
        error["error"] = std::string("Failed to compute API hash: ") + e.what();
        return error;
    }
}

// ═══════════════════════════════════════════════════════════
// Registration Function
// ═══════════════════════════════════════════════════════════

void registerOpenRpcMethods() {
    RpcMethodMeta meta;

    // rpc.openrpc
    meta.name = "rpc.openrpc";
    meta.ns = "rpc";
    meta.description = "Get full OpenRPC 1.3.2 schema for this API";
    meta.result.type = "object";
    meta.result.desc = "Complete OpenRPC schema document";
    g_rpcRegistry.registerHandler("rpc.openrpc", rpc_openrpc, meta, "openrpc");

    // rpc.schema
    meta = RpcMethodMeta();
    meta.name = "rpc.schema";
    meta.ns = "rpc";
    meta.description = "Get OpenRPC schema for a specific method";
    meta.params.push_back({"method", "string", "RPC method name", true});
    meta.result.type = "object";
    meta.result.desc = "OpenRPC method schema";
    g_rpcRegistry.registerHandler("rpc.schema", rpc_schema, meta, "openrpc");

    // rpc.namespaces
    meta = RpcMethodMeta();
    meta.name = "rpc.namespaces";
    meta.ns = "rpc";
    meta.description = "Get all methods grouped by namespace/category";
    meta.result.type = "object";
    meta.result.desc = "Map of namespace -> array of method names";
    g_rpcRegistry.registerHandler("rpc.namespaces", rpc_namespaces, meta, "openrpc");

    // rpc.apihash
    meta = RpcMethodMeta();
    meta.name = "rpc.apihash";
    meta.ns = "rpc";
    meta.description = "Get API version hash (for client caching and version detection)";
    meta.result.type = "object";
    meta.result.desc = "API hash and metadata";
    g_rpcRegistry.registerHandler("rpc.apihash", rpc_apihash, meta, "openrpc");

    dinero::g_logger.info("Registered 4 OpenRPC auto-documentation methods");
}

} // namespace rpc
} // namespace dinero
