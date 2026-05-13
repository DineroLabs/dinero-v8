/**
 * Mining Control RPC Methods - Phase Y (CPU Miner Rebuild)
 *
 * Header file for mining control RPC registration.
 */

#pragma once

// Forward declaration
class RpcRegistry;

/**
 * Register mining control RPC methods
 *
 * Registers the following RPC methods:
 * - mining.start      : Start CPU mining
 * - mining.stop       : Stop CPU mining
 * - mining.getstatus  : Get mining status
 * - mining.setthreads : Adjust thread count
 * - mining.setaddress : Set mining address
 *
 * Called during daemon initialization from RegisterAllRPCMethods().
 */
void register_mining_control_rpc_methods(RpcRegistry& registry);
