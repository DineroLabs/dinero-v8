#pragma once
#include "rpc/rpc_registry.h"
#include "din_json.h"
#include <string>

namespace dinero {
namespace rpc {

/**
 * OpenRPC Schema Generator
 *
 * Generates OpenRPC 1.3.2 compliant JSON schema from RpcRegistry metadata.
 * Enables automatic API documentation, client code generation, and tooling.
 *
 * Spec: https://spec.open-rpc.org/
 */
class OpenRpcGenerator {
public:
    explicit OpenRpcGenerator(RpcRegistry* registry);

    /**
     * Generate full OpenRPC schema document
     *
     * Returns a complete OpenRPC 1.3.2 schema including:
     * - API metadata (version, title, description)
     * - All registered methods with full documentation
     * - Parameter and result schemas
     * - Example requests/responses
     *
     * @return OpenRPC schema as din::Json object
     */
    din::Json generateSchema() const;

    /**
     * Generate schema for a single method
     *
     * @param method_name RPC method name
     * @return OpenRPC method schema or null if method doesn't exist
     */
    din::Json generateMethodSchema(const std::string& method_name) const;

    /**
     * Get methods grouped by namespace/category
     *
     * @return Map of namespace -> array of method names
     */
    din::Json getMethodsByNamespace() const;

    /**
     * Get API version hash (for client caching/versioning)
     *
     * Computes SHA256 hash of all method signatures and metadata.
     * Clients can cache schemas and detect API changes.
     *
     * @return Hex-encoded SHA256 hash
     */
    std::string getApiVersionHash() const;

private:
    RpcRegistry* registry_;

    // Convert RpcParamMeta to OpenRPC parameter schema
    din::Json paramToSchema(const RpcParamMeta& param) const;

    // Convert RpcResultMeta to OpenRPC result schema
    din::Json resultToSchema(const RpcResultMeta& result) const;

    // Convert type string to JSON Schema type
    din::Json typeToJsonSchema(const std::string& type) const;
};

} // namespace rpc
} // namespace dinero
