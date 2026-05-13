#pragma once

/**
 * CT Fee Configuration RPC Methods - Phase 3 (CT Fee Market Tuning)
 *
 * Provides RPC commands for configuring Confidential Transaction fee policies:
 * - ct.setminfee         : Set minimum CT fee rate (sat/vB)
 * - ct.setweightmultiplier: Set CT weight multiplier
 * - ct.setmaxperblock    : Set max CT transactions per block
 * - ct.getfeeconfig      : Get current CT fee configuration
 * - ct.estimatefee       : Estimate fee for CT transaction
 *
 * These commands allow runtime adjustment of CT fee policies without restarting.
 */

// Forward declaration
class RpcRegistry;

/**
 * Register CT fee configuration RPC methods
 *
 * @param registry  The RPC registry to register handlers with
 */
void register_ct_fee_rpc_methods(RpcRegistry& registry);
