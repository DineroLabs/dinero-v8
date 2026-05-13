#pragma once

/**
 * @brief RPC system startup and validation functions
 */
namespace RpcStartup {

/**
 * @brief Initialize the RPC system and validate configuration
 * 
 * This function should be called after all RPC handlers are registered
 * but before the RPC server starts accepting requests. It performs:
 * - Alias validation (ensures all aliases point to registered handlers)
 * - Registry integrity checks
 * - Capability registration
 * 
 * @throws std::runtime_error if validation fails
 */
void initialize();

/**
 * @brief Register meta RPC handlers (rpc.capabilities, rpc.listmethods, etc.)
 * 
 * This registers the introspection and meta handlers that provide
 * information about the RPC API itself.
 */
void registerMetaHandlers();

} // namespace RpcStartup
