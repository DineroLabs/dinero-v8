#include "rpc_integration.h"
#if DIN_ENABLE_LEGACY_RPC
#include "daemon/rpc_server.h"
#endif
#include "daemon/activity_bus.hpp"
#include "../common/logger.h"
#include <stdexcept>

extern dinero::Logger g_logger;
#if DIN_ENABLE_LEGACY_RPC
extern std::unique_ptr<dinero::RPCServer> g_rpc_server;
#endif

// Convert Json::Value to jsoncpp Json::Value
Json::Value nlohmann_to_jsoncpp(const Json::Value& nj) {
    if (nj.isNull()) {
        return Json::Value(Json::nullValue);
    } else if (nj.isBool()) {
        return Json::Value(nj.asBool());
    } else if (nj.isInt()) {
        return Json::Value(nj.asInt64());
    } else if (nj.isUInt()) {
        return Json::Value(nj.asUInt64());
    } else if (nj.isDouble()) {
        return Json::Value(nj.asDouble());
    } else if (nj.isString()) {
        return Json::Value(nj.asString());
    } else if (nj.isArray()) {
        Json::Value arr(Json::arrayValue);
        for (const auto& item : nj) {
            arr.append(nlohmann_to_jsoncpp(item));
        }
        return arr;
    } else if (nj.isObject()) {
        Json::Value obj(Json::objectValue);
        for (auto it = nj.begin(); it != nj.end(); ++it) {
            obj[it.key().asString()] = nlohmann_to_jsoncpp(*it);
        }
        return obj;
    }
    return Json::Value(Json::nullValue);
}

// Convert jsoncpp Json::Value to Json::Value
Json::Value jsoncpp_to_nlohmann(const Json::Value& jv) {
    if (jv.isNull()) {
        return Json::Value(Json::nullValue);
    } else if (jv.isBool()) {
        return Json::Value(jv.asBool());
    } else if (jv.isInt()) {
        return Json::Value(jv.asInt());
    } else if (jv.isInt64()) {
        return Json::Value(jv.asInt64());
    } else if (jv.isUInt()) {
        return Json::Value(jv.asUInt());
    } else if (jv.isUInt64()) {
        return Json::Value(jv.asUInt64());
    } else if (jv.isDouble()) {
        return Json::Value(jv.asDouble());
    } else if (jv.isString()) {
        return Json::Value(jv.asString());
    } else if (jv.isArray()) {
        Json::Value arr = Json::Value(Json::arrayValue);
        for (const auto& item : jv) {
            arr.append(jsoncpp_to_nlohmann(item));
        }
        return arr;
    } else if (jv.isObject()) {
        Json::Value obj = Json::Value(Json::objectValue);
        for (const auto& key : jv.getMemberNames()) {
            obj[key] = jsoncpp_to_nlohmann(jv[key]);
        }
        return obj;
    }
    return Json::Value(Json::nullValue);
}

// Main RPC dispatch function for the daemon
Json::Value daemon_rpc_dispatch(const std::string& path, const std::string& method,
                                   const Json::Value& params, const Json::Value& id) {
    try {
#if DIN_ENABLE_LEGACY_RPC
        if (!g_rpc_server) {
            throw std::runtime_error("RPC server not initialized");
        }
#endif

        // Log the RPC call
        g_logger.info("RPC call: " + method + (params.empty() ? "" : " params=<provided>"));
        
        // Publish to activity bus
        Json::Value activity_event;
        activity_event["kind"] = "rpc_request";
        activity_event["method"] = method;
        activity_event["auth"] = "ok"; // TODO: Get actual auth status
        dinero_daemon::ActivityBus::publish(activity_event);

        // Handle wallet routing if path starts with /wallet/
        std::string wallet_name;
        if (path.rfind("/wallet/", 0) == 0) {
            wallet_name = path.substr(8); // Remove "/wallet/" prefix
            // TODO: Set active wallet context if needed
        }

        // Convert params to string for RPC server
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string params_str = Json::writeString(builder, nlohmann_to_jsoncpp(params));

        // Call the appropriate method handler
        std::string result_str;
#if DIN_ENABLE_LEGACY_RPC
        if (g_rpc_server->hasMethod(method)) {
            result_str = g_rpc_server->callMethod(method, params_str);
        } else {
            // Return a proper JSON-RPC error for unknown methods
            throw std::runtime_error("Method not found: " + method);
        }
#else
        // vNext build: use vNext registry
        throw std::runtime_error("Method not found: " + method);
#endif

        // Parse result string back to JSON
        Json::CharReaderBuilder reader_builder;
        std::unique_ptr<Json::CharReader> reader(reader_builder.newCharReader());
        Json::Value result_json;
        std::string parse_errors;
        if (!reader->parse(result_str.data(), result_str.data() + result_str.size(), &result_json, &parse_errors)) {
            throw std::runtime_error("Failed to parse RPC result: " + parse_errors);
        }

        // Convert jsoncpp result back to Json::Value
        return jsoncpp_to_nlohmann(result_json);

    } catch (const std::exception& e) {
        g_logger.error("RPC error for method '" + method + "': " + e.what());
        throw; // Re-throw to be handled by rpc_http.hpp error handling
    }
}
