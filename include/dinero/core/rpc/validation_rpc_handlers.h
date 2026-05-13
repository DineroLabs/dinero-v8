#pragma once

// Forward declaration
class HttpRpcServer;

/**
 * @file validation_rpc_handlers.h
 * @brief RPC handlers for chain validation and health checks
 *
 * Provides RPC methods for validating chain state and health:
 * - validatechain: Comprehensive chain health and consensus validation
 */

namespace dinero {

/**
 * Register all validation RPC handlers with the HTTP RPC server
 *
 * This function registers the following RPC methods:
 * - validatechain: Returns comprehensive chain validation status including
 *   genesis verification, network type, peer count, sync state, and MTP consistency
 *
 * @param rpc_server Pointer to the HttpRpcServer instance
 */
void RegisterValidationRPCHandlers(HttpRpcServer* rpc_server);

} // namespace dinero
