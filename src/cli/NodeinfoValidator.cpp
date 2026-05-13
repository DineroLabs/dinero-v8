#include "cli/NodeinfoValidator.h"
#include "compat/jsoncpp_compat.h"
#include <algorithm>
#include <regex>
#include <sstream>

namespace dinero::cli {

SchemaValidationResult validateNodeinfoSchema(const std::string& json_content) {
    SchemaValidationResult result;
    
    // Parse JSON
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(json_content);
    
    if (!Json::parseFromStream(builder, stream, &root, &errors)) {
        result.errors.push_back("Invalid JSON: " + errors);
        return result;
    }
    
    return validateNodeinfoObject(&root);
}

SchemaValidationResult validateNodeinfoObject(const void* json_object) {
    SchemaValidationResult result;
    const Json::Value* root = static_cast<const Json::Value*>(json_object);
    
    // Root must be an object
    if (!root->isObject()) {
        result.errors.push_back("Root must be an object");
        return result;
    }
    
    // Check schema version
    if (!root->isMember("schema") || !(*root)["schema"].isString()) {
        result.errors.push_back("Missing required field: schema");
    } else {
        std::string schema = (*root)["schema"].asString();
        if (schema != "din.nodeinfo.v1") {
            result.errors.push_back("Unsupported schema version: " + schema + " (expected: din.nodeinfo.v1)");
        }
    }
    
    // Check version field
    if (!root->isMember("version") || !(*root)["version"].isString()) {
        result.errors.push_back("Missing required field: version");
    } else {
        std::string version = (*root)["version"].asString();
        if (version != "din.nodeinfo.v1") {
            result.errors.push_back("Unsupported version: " + version + " (expected: din.nodeinfo.v1)");
        }
    }
    
    // Check network field
    if (!root->isMember("network") || !(*root)["network"].isString()) {
        result.errors.push_back("Missing required field: network");
    } else {
        std::string network = (*root)["network"].asString();
        if (network != "regtest" && network != "testnet" && network != "mainnet") {
            result.errors.push_back("Invalid network: " + network + " (expected: regtest, testnet, or mainnet)");
        }
    }
    
    // Check rpc section
    if (!root->isMember("rpc") || !(*root)["rpc"].isObject()) {
        result.errors.push_back("Missing required field: rpc");
        return result;
    }
    
    const Json::Value& rpc = (*root)["rpc"];
    
    // Check rpc.url
    if (!rpc.isMember("url") || !rpc["url"].isString()) {
        result.errors.push_back("Missing required field: rpc.url");
    } else {
        std::string url = rpc["url"].asString();
        // Basic URL validation
        std::regex url_regex(R"(^https?://[^:]+:\d+(/.*)?$)");
        if (!std::regex_match(url, url_regex)) {
            result.errors.push_back("Invalid rpc.url format: " + url + " (expected: http://host:port or https://host:port)");
        }
    }
    
    // Check cookie configuration
    bool has_cookie_path = false;
    bool has_cookie_file = false;
    bool has_cookie_object = false;
    bool has_cookie_literal = false;
    
    if (rpc.isMember("cookie_path") && rpc["cookie_path"].isString()) {
        has_cookie_path = true;
    }
    if (rpc.isMember("cookie_file") && rpc["cookie_file"].isString()) {
        has_cookie_file = true;
        result.warnings.push_back("rpc.cookie_file is deprecated, use rpc.cookie_path instead");
    }
    if (rpc.isMember("cookie") && rpc["cookie"].isObject()) {
        has_cookie_object = true;
        const Json::Value& cookie_obj = rpc["cookie"];
        if (cookie_obj.isMember("path") && cookie_obj["path"].isString()) {
            result.warnings.push_back("rpc.cookie.path is deprecated, use rpc.cookie_path instead");
        }
    }
    if (rpc.isMember("cookie") && rpc["cookie"].isString()) {
        has_cookie_literal = true;
        result.warnings.push_back("rpc.cookie (literal) is insecure and deprecated, use rpc.cookie_path instead");
    }
    
    if (!has_cookie_path && !has_cookie_file && !has_cookie_object && !has_cookie_literal) {
        result.errors.push_back("Missing cookie configuration: provide rpc.cookie_path");
    }
    
    // Check timeout_seconds (optional)
    if (rpc.isMember("timeout_seconds")) {
        if (!rpc["timeout_seconds"].isInt()) {
            result.errors.push_back("rpc.timeout_seconds must be an integer");
        } else {
            int timeout = rpc["timeout_seconds"].asInt();
            if (timeout < 1 || timeout > 300) {
                result.errors.push_back("rpc.timeout_seconds must be between 1 and 300 seconds");
            }
        }
    }
    
    // Check for unknown fields
    std::vector<std::string> known_fields = {
        "schema", "version", "network", "rpc", "datadir", "ws"
    };
    
    for (const auto& member : root->getMemberNames()) {
        if (std::find(known_fields.begin(), known_fields.end(), member) == known_fields.end()) {
            result.warnings.push_back("Unknown field: " + member);
        }
    }
    
    // Check for unknown rpc fields
    std::vector<std::string> known_rpc_fields = {
        "url", "cookie_path", "cookie_file", "cookie", "timeout_seconds"
    };
    
    for (const auto& member : rpc.getMemberNames()) {
        if (std::find(known_rpc_fields.begin(), known_rpc_fields.end(), member) == known_rpc_fields.end()) {
            result.warnings.push_back("Unknown rpc field: " + member);
        }
    }
    
    // Determine if valid
    result.valid = result.errors.empty();
    
    return result;
}

} // namespace dinero::cli
