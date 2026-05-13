#pragma once

/**
 * @file consensus_rpc_handlers.h
 * @brief RPC handlers for consensus and chain introspection
 * 
 * Provides RPC methods for developers and operators to inspect:
 * - Chain tips and fork status (getchaintips)
 * - Total chainwork (getchainwork)  
 * - Reorganization status and safe mode (getreorgstatus)
 */

namespace dinero {

/**
 * Register all consensus RPC handlers with the global RPC registry
 * 
 * This function registers the following RPC methods:
 * - getchaintips: Returns all known chain tips with status
 * - getchainwork: Returns total chainwork for active tip
 * - getreorgstatus: Returns last reorg stats and safe mode status
 */
void RegisterConsensusRPCHandlers();

} // namespace dinero
