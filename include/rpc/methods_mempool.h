#pragma once

#include "http_rpc_server.h"
#include "transaction_pool.h"
#include <json/json.h>

namespace dinero {
namespace rpc {

// ═══════════════════════════════════════════════════════════
// Mempool RPC Method Registrations
// ═══════════════════════════════════════════════════════════

/**
 * Register mempool-related RPC methods with the HTTP RPC server.
 *
 * Methods registered:
 * - getmempoolinfo: Get memory pool information
 * - getrawmempool: Get all transaction ids in memory pool
 *
 * @param server RPC server to register methods with
 * @param tx_pool Transaction pool instance
 */
void registerMempoolMethods(
    HttpRpcServer* server,
    mempool::TransactionPool* tx_pool
);

/**
 * Register mempool fee estimation RPC methods (vnext).
 *
 * Methods registered:
 * - estimatefee: Estimate fee for target confirmation blocks
 * - getfeeestimates: Get comprehensive fee estimates for all priority levels
 * - estimatesmartfee: Enhanced fee estimation with multiple targets
 */
void registerMempoolFeeEstimationMethods();

} // namespace rpc
} // namespace dinero
