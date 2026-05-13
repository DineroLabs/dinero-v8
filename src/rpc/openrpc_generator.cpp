#include "rpc/openrpc_generator.h"
#include "crypto/sha256.h"
#include "common/logger.h"
#include <sstream>
#include <iomanip>

namespace dinero {
namespace rpc {

OpenRpcGenerator::OpenRpcGenerator(RpcRegistry* registry)
    : registry_(registry) {
}

din::Json OpenRpcGenerator::generateSchema() const {
    din::Json schema;

    // OpenRPC version
    schema["openrpc"] = "1.3.2";

    // API info
    din::Json info;
    info["title"] = "Dinero RPC API";
    info["version"] = "0.1.0";
    info["description"] = "Unified RPC interface for Dinero blockchain node. "
                          "Supports blockchain, wallet, mining, bridge, contracts, and more.";

    din::Json contact;
    contact["name"] = "Dinero Development Team";
    contact["url"] = "https://dinero-coin.com";
    schema["info"] = info;

    // Servers
    din::Json servers = din::arr();
    din::Json http_server;
    http_server["name"] = "HTTP RPC";
    http_server["url"] = "http://localhost:20998/";
    http_server["description"] = "HTTP JSON-RPC 2.0 endpoint";
    servers.append(http_server);

    din::Json ws_server;
    ws_server["name"] = "WebSocket RPC";
    ws_server["url"] = "ws://localhost:21001/ws";
    ws_server["description"] = "WebSocket JSON-RPC 2.0 endpoint with real-time event subscriptions";
    servers.append(ws_server);

    schema["servers"] = servers;

    // Methods
    din::Json methods = din::arr();
    auto method_names = registry_->methodNames();

    for (const auto& method_name : method_names) {
        din::Json method_schema = generateMethodSchema(method_name);
        if (!method_schema.isNull()) {
            methods.append(method_schema);
        }
    }

    schema["methods"] = methods;

    // Components (reusable schemas)
    din::Json components;
    components["schemas"] = din::obj();  // Can add common types here later
    schema["components"] = components;

    return schema;
}

din::Json OpenRpcGenerator::generateMethodSchema(const std::string& method_name) const {
    const RpcMethodMeta* meta = registry_->getMethodMeta(method_name);

    din::Json method;
    method["name"] = method_name;

    if (meta) {
        // Use metadata if available
        method["summary"] = meta->description.empty() ? method_name : meta->description;
        method["description"] = meta->help.empty() ? meta->description : meta->help;

        // Add namespace/category as tag
        if (!meta->ns.empty()) {
            din::Json tags = din::arr();
            tags.append(meta->ns);
            method["tags"] = tags;
        }

        // Parameters
        din::Json params = din::arr();
        for (const auto& param : meta->params) {
            params.append(paramToSchema(param));
        }
        method["params"] = params;

        // Result
        method["result"] = resultToSchema(meta->result);
    } else {
        // Fallback for methods without metadata
        method["summary"] = method_name;
        method["description"] = "No documentation available";
        method["params"] = din::arr();

        din::Json result;
        result["name"] = "result";
        result["schema"] = typeToJsonSchema("object");
        method["result"] = result;
    }

    return method;
}

din::Json OpenRpcGenerator::paramToSchema(const RpcParamMeta& param) const {
    din::Json p;
    p["name"] = param.name;
    p["description"] = param.desc;
    p["required"] = param.required;
    p["schema"] = typeToJsonSchema(param.type);
    return p;
}

din::Json OpenRpcGenerator::resultToSchema(const RpcResultMeta& result) const {
    din::Json r;
    r["name"] = "result";
    r["description"] = result.desc;
    r["schema"] = typeToJsonSchema(result.type);
    return r;
}

din::Json OpenRpcGenerator::typeToJsonSchema(const std::string& type) const {
    din::Json schema;

    if (type == "string") {
        schema["type"] = "string";
    } else if (type == "int" || type == "integer" || type == "number") {
        schema["type"] = "integer";
    } else if (type == "bool" || type == "boolean") {
        schema["type"] = "boolean";
    } else if (type == "array") {
        schema["type"] = "array";
        schema["items"] = din::obj();
    } else if (type == "object") {
        schema["type"] = "object";
    } else if (type.empty()) {
        // Default to object
        schema["type"] = "object";
    } else {
        // Try to parse complex types like "array<string>"
        schema["type"] = "object";
        schema["description"] = type;
    }

    return schema;
}

din::Json OpenRpcGenerator::getMethodsByNamespace() const {
    din::Json by_namespace;

    auto method_names = registry_->methodNames();
    for (const auto& method_name : method_names) {
        const RpcMethodMeta* meta = registry_->getMethodMeta(method_name);
        std::string ns = meta ? meta->ns : "uncategorized";

        if (ns.empty()) ns = "uncategorized";

        if (!by_namespace.isMember(ns)) {
            by_namespace[ns] = din::arr();
        }
        by_namespace[ns].append(method_name);
    }

    return by_namespace;
}

std::string OpenRpcGenerator::getApiVersionHash() const {
    // Collect all method signatures
    std::ostringstream signature_stream;

    auto method_names = registry_->methodNames();
    std::vector<std::string> sorted_methods(method_names.begin(), method_names.end());
    std::sort(sorted_methods.begin(), sorted_methods.end());

    for (const auto& method_name : sorted_methods) {
        signature_stream << method_name << ":";

        const RpcMethodMeta* meta = registry_->getMethodMeta(method_name);
        if (meta) {
            signature_stream << meta->description << ":";
            for (const auto& param : meta->params) {
                signature_stream << param.name << ":" << param.type << ":" << param.required << ";";
            }
            signature_stream << meta->result.type;
        }
        signature_stream << "\n";
    }

    // Compute SHA256 hash using project-native CSHA256 helper.
    std::string signature = signature_stream.str();
    uint8_t hash_bytes[32] = {0};
    dinero::crypto::CSHA256()
        .Write(reinterpret_cast<const uint8_t*>(signature.data()), signature.size())
        .Finalize(hash_bytes);

    // Convert to hex
    std::ostringstream hex_stream;
    for (uint8_t byte : hash_bytes) {
        hex_stream << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }

    return hex_stream.str();
}

} // namespace rpc
} // namespace dinero
