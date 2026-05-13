#include "cli/schema_validation.h"
#include <iostream>

namespace dinero {

const std::string SchemaValidator::EXPECTED_RPC_SCHEMA = "din.rpc.v1";
const std::string SchemaValidator::CLI_SCHEMA = "din.cli.v1";

bool SchemaValidator::validateRpcSchema(const Json::Value& response) {
    if (!response.isMember("rpc_schema")) {
        warnMissingSchema();
        return false; // Still functional, but not ideal
    }
    
    std::string schema = response["rpc_schema"].asString();
    if (schema != EXPECTED_RPC_SCHEMA) {
        warnSchemaMismatch(schema, EXPECTED_RPC_SCHEMA);
        return false;
    }
    
    return true;
}

bool SchemaValidator::hasRpcSchema(const Json::Value& response) {
    return response.isMember("rpc_schema") && response["rpc_schema"].isString();
}

std::string SchemaValidator::getRpcSchema(const Json::Value& response) {
    if (hasRpcSchema(response)) {
        return response["rpc_schema"].asString();
    }
    return "";
}

void SchemaValidator::warnSchemaMismatch(const std::string& found, const std::string& expected) {
    std::cerr << "Warning: RPC schema mismatch. Found: '" << found 
              << "', Expected: '" << expected << "'" << std::endl;
    std::cerr << "This may indicate daemon/CLI version incompatibility." << std::endl;
}

void SchemaValidator::warnMissingSchema() {
    std::cerr << "Warning: Daemon response missing 'rpc_schema' field." << std::endl;
    std::cerr << "Consider upgrading to dinerod vNext for better compatibility." << std::endl;
}

} // namespace dinero
