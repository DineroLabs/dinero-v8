#pragma once

#include "rpc/rpc_registry.h"
#include <string>
#include <vector>
#include <functional>

/**
 * RPC Method Builder - Fluent DSL for declaring RPC methods with full metadata
 *
 * Usage:
 *   RPC_METHOD("blockchain.getblockcount", "blockchain")
 *       .description("Returns the current blockchain height")
 *       .params({})
 *       .result("number", "Current block height")
 *       .handler([](const ExecutionContext& ctx, const din::Json& params) {
 *           return getBlockCount();
 *       })
 *       .examples({
 *           "blockchain.getblockcount"
 *       });
 *
 * This creates a self-documenting, introspectable RPC method that works
 * across HTTP, WebSocket, CLI, and GUI with unified metadata.
 */

extern RpcRegistry g_rpcRegistry;

class RpcMethodBuilder {
public:
    RpcMethodBuilder(const std::string& method_name, const std::string& category)
        : method_name_(method_name), category_(category) {
        meta_.name = method_name;
        meta_.ns = category;
    }

    // Set method description
    RpcMethodBuilder& description(const std::string& desc) {
        meta_.description = desc;
        return *this;
    }

    // Add a parameter with metadata
    RpcMethodBuilder& param(const std::string& name, const std::string& type,
                           const std::string& desc, bool required = true) {
        RpcParamMeta param;
        param.name = name;
        param.type = type;
        param.desc = desc;
        param.required = required;
        meta_.params.push_back(param);
        return *this;
    }

    // Set all parameters at once (shorthand)
    RpcMethodBuilder& params(const std::vector<RpcParamMeta>& params) {
        meta_.params = params;
        return *this;
    }

    // Set return type metadata
    RpcMethodBuilder& result(const std::string& type, const std::string& desc) {
        meta_.result.type = type;
        meta_.result.desc = desc;
        return *this;
    }

    // Set the actual handler function
    RpcMethodBuilder& handler(RpcHandler fn) {
        handler_ = fn;
        has_handler_ = true;
        return *this;
    }

    // Add usage examples
    RpcMethodBuilder& examples(const std::vector<std::string>& examples) {
        // Store examples in the help field (keeps output visible in current help surfaces).
        meta_.help = "Examples:\n";
        for (const auto& ex : examples) {
            meta_.help += "  " + ex + "\n";
        }
        return *this;
    }

    // Set registration mode (Overwrite or IfAbsent)
    RpcMethodBuilder& mode(RegisterMode m) {
        mode_ = m;
        has_mode_ = true;
        return *this;
    }

    // Destructor auto-registers when object goes out of scope
    ~RpcMethodBuilder() {
        if (has_handler_) {
            if (has_mode_) {
                g_rpcRegistry.registerHandler(method_name_, handler_, meta_, mode_, category_);
            } else {
                g_rpcRegistry.registerHandler(method_name_, handler_, meta_, category_);
            }
        }
    }

private:
    std::string method_name_;
    std::string category_;
    RpcMethodMeta meta_;
    RpcHandler handler_;
    bool has_handler_ = false;
    RegisterMode mode_ = RegisterMode::IfAbsent;
    bool has_mode_ = false;
};

// Macro for clean syntax - creates temp builder and finalizes immediately
#define RPC_METHOD(name, category) \
    RpcMethodBuilder(name, category)
