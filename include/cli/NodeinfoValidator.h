#pragma once
#include <string>
#include <optional>
#include <vector>

namespace dinero::cli {

struct SchemaValidationResult {
    bool valid = false;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/**
 * Validates nodeinfo.json against the canonical schema.
 * 
 * Canonical schema:
 * {
 *   "schema": "din.nodeinfo.v1",
 *   "version": "din.nodeinfo.v1", 
 *   "network": "regtest|testnet|mainnet",
 *   "rpc": {
 *     "url": "http://host:port",
 *     "cookie_path": "/path/to/.cookie",
 *     "timeout_seconds": 30
 *   }
 * }
 * 
 * Accepted aliases (with deprecation warnings):
 * - rpc.cookie_file (prefer cookie_path)
 * - rpc.cookie.path (prefer cookie_path)
 * - rpc.cookie (literal string, requires --accept-insecure-cookie)
 */
SchemaValidationResult validateNodeinfoSchema(const std::string& json_content);

/**
 * Validates a parsed JSON object against the schema.
 */
SchemaValidationResult validateNodeinfoObject(const void* json_object); // Json::Value*

} // namespace dinero::cli
