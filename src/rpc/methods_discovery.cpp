#include "rpc/rpc_registry.h"
#include "din_json.h"
#include <vector>
#include <string>

extern RpcRegistry g_rpcRegistry;

namespace din {
namespace rpc {

/**
 * RPC method discovery
 * Returns all available RPC methods with their metadata
 */
din::Json rpc_discover_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result = din::obj();

    // Get all registered methods
    auto methods = g_rpcRegistry.methodNames();

    din::Json methods_array = din::arr();
    for (const auto& method : methods) {
        din::Json method_obj = din::obj();
        method_obj["name"] = method;

        // Try to get method owner/category
        std::string owner = g_rpcRegistry.getMethodOwner(method);
        if (!owner.empty()) {
            method_obj["category"] = owner;
        }

        // Get full metadata if available
        const RpcMethodMeta* meta = g_rpcRegistry.getMethodMeta(method);
        if (meta) {
            method_obj["description"] = meta->description;

            // Add parameters
            if (!meta->params.empty()) {
                din::Json params_array = din::arr();
                for (const auto& param : meta->params) {
                    din::Json param_obj = din::obj();
                    param_obj["name"] = param.name;
                    param_obj["type"] = param.type;
                    param_obj["description"] = param.desc;
                    param_obj["required"] = param.required;
                    params_array.append(param_obj);
                }
                method_obj["parameters"] = params_array;
            }

            // Add result info
            if (!meta->result.type.empty()) {
                din::Json result_obj = din::obj();
                result_obj["type"] = meta->result.type;
                result_obj["description"] = meta->result.desc;
                method_obj["returns"] = result_obj;
            }

            // Add help text if available
            if (!meta->help.empty()) {
                method_obj["help"] = meta->help;
            }
        }

        methods_array.append(method_obj);
    }

    result["methods"] = methods_array;
    result["count"] = static_cast<int>(methods.size());
    result["version"] = "2.0";  // Updated version for enhanced metadata
    result["rpc_schema"] = "din.discovery.v2";

    return result;
}

/**
 * Get detailed information about the RPC server
 */
din::Json rpc_info_impl(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result = din::obj();

    result["jsonrpc_version"] = "2.0";
    result["server"] = "Dinero RPC Server";
    result["transports"] = din::arr();
    result["transports"].append("http");
    result["transports"].append("websocket");

    // Method counts by category
    auto methods = g_rpcRegistry.methodNames();
    din::Json categories = din::obj();

    for (const auto& method : methods) {
        std::string owner = g_rpcRegistry.getMethodOwner(method);
        if (owner.empty()) owner = "core";

        if (!categories.isMember(owner)) {
            categories[owner] = 0;
        }
        categories[owner] = categories[owner].asInt() + 1;
    }

    result["categories"] = categories;
    result["total_methods"] = static_cast<int>(methods.size());
    result["rpc_schema"] = "din.discovery.v1";

    return result;
}

/**
 * Register discovery methods in the RPC registry
 */
void register_discovery_methods() {
    g_rpcRegistry.registerHandler("rpc.discover",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_discover_impl(ctx, params);
        },
        "discovery");

    g_rpcRegistry.registerHandler("rpc.info",
        [](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            return rpc_info_impl(ctx, params);
        },
        "discovery");
}

} // namespace rpc
} // namespace din
