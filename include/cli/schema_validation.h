#pragma once

#include "compat/jsoncpp_compat.h"
#include <string>
#include <string>
#include "compat/jsoncpp_compat.h"

namespace dinero {

/**
 * @brief Validates RPC response schemas for CLI automation
 * 
 * This class provides schema validation for RPC responses to ensure
 * consistent output format for automation tools and scripts.
 */
class SchemaValidator {
public:
    /**
     * @brief Validates that an RPC response conforms to expected schema
     * @param response The JSON response to validate
     * @return true if response is valid, false otherwise
     */
    static bool validateRpcSchema(const Json::Value& response);
    
    /**
     * @brief Checks if response contains schema version information
     * @param response The JSON response to check
     * @return true if schema info is present
     */
    static bool hasRpcSchema(const Json::Value& response);
    
    /**
     * @brief Extracts schema version from response
     * @param response The JSON response
     * @return Schema version string or empty if not found
     */
    static std::string getRpcSchema(const Json::Value& response);
    
    /**
     * Emit warning for schema mismatch
     * @param found Schema found in response
     * @param expected Expected schema version
     */
    static void warnSchemaMismatch(const std::string& found, const std::string& expected);
    
    /**
     * Emit warning for missing schema
     */
    static void warnMissingSchema();
    
private:
    static const std::string EXPECTED_RPC_SCHEMA;
    static const std::string CLI_SCHEMA;
};

} // namespace dinero
