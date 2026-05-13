#pragma once

namespace dinero {
namespace rpc {

/**
 * Register OpenRPC auto-documentation methods
 *
 * Registers:
 * - rpc.openrpc: Get full OpenRPC 1.3.2 schema
 * - rpc.schema: Get schema for specific method
 * - rpc.namespaces: Get methods grouped by namespace
 * - rpc.apihash: Get API version hash
 */
void registerOpenRpcMethods();

} // namespace rpc
} // namespace dinero
